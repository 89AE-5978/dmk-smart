#include "MqttOta.h"
#include "DmkConfig.h"
#include "DmkCrypto.h"
#include "RemoteOta.h"
#include "firmware_info.h"
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <time.h>
#if defined(DMK_BOARD_ESP8266)
#include <ESP8266WiFi.h>
#include <Updater.h>
#else
#include <Update.h>
#include <WiFi.h>
#endif

// ----------------------------------------------------------------------------
// Comandos chegam em dmk-ota/<id>/cmd assinados com HMAC-SHA256:
//   {"id":"...","cmd":"check|install|config|status","ts":1730000000,
//    "p":"{...parametros em JSON...}","sig":"hex"}
//   sig = HMAC(senha_do_painel, id \n cmd \n ts \n p)
// A senha NUNCA trafega pelo broker. ts precisa estar a menos de 5 min do
// relogio da placa e ser maior que o ultimo aceito (bloqueia repeticao).
// ----------------------------------------------------------------------------
namespace MqttOta {
static WiFiClient network;
static PubSubClient mqtt(network);
static String id, base, password, pending, bridgeError, hostCache, userCache, passCache;
static uint16_t portCache = 0;
static uint32_t lastConnect = 0, lastPublish = 0, lastCheck = 0, restartAt = 0;
static bool prepared = false, autoQueued = false;
#if defined(DMK_ROLE_CONTROLE)
static bool timeStarted = false;
#endif
static RemoteOta::Manifest available;
static bool newer = false;
static uint32_t lastTs = 0;

String deviceId() { return id; }
bool busy() { return RemoteOta::busy() || prepared || restartAt || pending.length(); }

String statusJson() {
  JsonDocument doc;
  doc["id"] = id;
  doc["target"] = DMK_OTA_TARGET;
  doc["name"] = FIRMWARE_NAME;
  doc["version"] = FIRMWARE_VERSION;
  doc["online"] = mqtt.connected();
  doc["availableVersion"] = available.version;
  doc["available"] = newer;
  doc["error"] = bridgeError;
  doc["clock"] = (uint32_t)time(nullptr);
  JsonDocument state;
  deserializeJson(state, RemoteOta::statusJson());
  doc["ota"].set(state.as<JsonVariantConst>());
  doc["ota"]["busy"] = busy();
  if (pending.length()) {
    doc["ota"]["phase"] = "queued";
    doc["ota"]["message"] = "Solicitacao recebida";
    doc["ota"]["busy"] = true;
  }
  JsonDocument cfg;
  deserializeJson(cfg, RemoteOta::configJson());
  doc["repo"] = cfg["repo"];
  doc["manifestUrl"] = cfg["effective_url"];
  doc["autoInstall"] = cfg["auto_install"];
  String out;
  serializeJson(doc, out);
  return out;
}

static void publish() {
  if (!mqtt.connected()) return;
  String state = statusJson();
  mqtt.publish((base + "/status").c_str(), state.c_str(), true);
}

// Comando interno ja autenticado (HTTP local com Basic Auth ou MQTT assinado).
static bool queueTrusted(const String &cmd, JsonVariantConst params, String &error) {
  if (cmd == "status") {
    publish();
    return true;
  }
  if (cmd != "check" && cmd != "install" && cmd != "config") {
    error = "Comando desconhecido";
    return false;
  }
  if (busy() || Update.isRunning()) {
    error = "Outra atualizacao em andamento";
    return false;
  }
  JsonDocument job;
  job["cmd"] = cmd;
  job["p"].set(params);
  pending = "";
  serializeJson(job, pending);
  bridgeError = "";
  return true;
}

static bool verifySigned(JsonDocument &doc, String &error) {
  if (password.length() < 8) {
    error = "Senha do painel nao configurada";
    return false;
  }
  String cmd = doc["cmd"] | "";
  String p = doc["p"] | "";
  String sig = doc["sig"] | "";
  uint32_t ts = doc["ts"] | 0;
  String requestedId = doc["id"] | "";
  if (requestedId != id) {
    error = "ID diferente desta placa";
    return false;
  }
  String msg = requestedId + "\n" + cmd + "\n" + String(ts) + "\n" + p;
  uint8_t mac[32], given[32];
  DmkCrypto::hmacSha256((const uint8_t *)password.c_str(), password.length(), msg.c_str(), msg.length(), mac);
  if (!DmkCrypto::fromHex(sig.c_str(), given, 32) || !DmkCrypto::equalConst(mac, given, 32)) {
    error = "Senha incorreta (assinatura invalida)";
    return false;
  }
  uint32_t now = (uint32_t)time(nullptr);
  if (now > 1700000000UL) {
    uint32_t diff = now > ts ? now - ts : ts - now;
    if (diff > 300) {
      error = "Relogio do painel e da placa diferem mais de 5 minutos";
      return false;
    }
  }
  if (ts <= lastTs) {
    error = "Comando repetido recusado";
    return false;
  }
  lastTs = ts;
  return true;
}

bool request(const String &json, String &error) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    error = "JSON invalido";
    return false;
  }
  if (!verifySigned(doc, error)) return false;
  String cmd = doc["cmd"] | "";
  JsonDocument params;
  String p = doc["p"] | "";
  if (p.length() && deserializeJson(params, p)) {
    error = "Parametros invalidos";
    return false;
  }
  return queueTrusted(cmd, params.as<JsonVariantConst>(), error);
}

// Pedido vindo da API HTTP local, ja autenticado por Basic Auth no main.
bool requestLocal(const String &json, String &error) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    error = "JSON invalido";
    return false;
  }
  String cmd = doc["cmd"] | "";
  return queueTrusted(cmd, doc.as<JsonVariantConst>(), error);
}

static void received(char *, uint8_t *payload, unsigned length) {
  if (length > 3800) {
    bridgeError = "Comando grande demais";
    publish();
    return;
  }
  String json;
  json.reserve(length);
  for (unsigned i = 0; i < length; i++) json += (char)payload[i];
  String error;
  if (!request(json, error)) {
    bridgeError = error;
    publish();
  }
}

void begin() {
  id = String(FIRMWARE_NAME) + "-" + WiFi.macAddress();
  id.replace(":", "");
  base = "dmk-ota/" + id;
  RemoteOta::begin(DMK_OTA_TARGET, FIRMWARE_VERSION);
#if defined(DMK_BOARD_ESP8266)
  mqtt.setBufferSize(2048);  // ESP8266: preserva RAM para o HTTPS
#else
  mqtt.setBufferSize(4096);
#endif
  mqtt.setCallback(received);
  mqtt.setSocketTimeout(2);
  mqtt.setKeepAlive(30);
}

static void handlePending() {
  if (pending.isEmpty()) return;
  JsonDocument doc;
  deserializeJson(doc, pending);
  pending = "";
  String cmd = doc["cmd"] | "";
  JsonVariantConst p = doc["p"];
  String error;
  if (cmd == "config") {
    bool ok = true;
    if (!p["allow_local_http"].isNull()) ok = RemoteOta::allowHttpLocal(p["allow_local_http"] | false, error);
    if (ok && !p["auto_install"].isNull()) ok = RemoteOta::setAutoInstall(p["auto_install"] | false, error);
    if (ok && (!p["repo"].isNull() || !p["manifest_url"].isNull()))
      ok = RemoteOta::saveConfig(p["repo"] | "", p["manifest_url"] | "", p["server_token"] | "", p["ca_cert"] | "", error);
    if (!ok) bridgeError = error;
    else {
      bridgeError = "";
      lastCheck = 0;
      available = RemoteOta::Manifest{};
      newer = false;
    }
  } else if (cmd == "check") {
    lastCheck = millis();
    if (!RemoteOta::check(available, error)) {
      bridgeError = error;
      newer = false;
    } else {
      bridgeError = "";
      newer = RemoteOta::compareWithInstalled(available.version) > 0;
      Serial.printf("[OTA] Publicada: %s | instalada: %s%s\n", available.version.c_str(), FIRMWARE_VERSION,
                    newer ? " -> ATUALIZACAO DISPONIVEL" : "");
    }
  } else if (cmd == "install") {
    if (!dmkPrepareMqttOta(error)) {
      bridgeError = error;
      publish();
      return;
    }
    prepared = true;
    publish();
    if (!RemoteOta::startInstall(p["force"] | false, error)) {
      bridgeError = error;
      prepared = false;
      dmkEndMqttOta(false);
    }
  }
  publish();
}

void loop(const String &host, uint16_t port, const String &user, const String &pass, const String &otaPass) {
  password = otaPass;
  if (host != hostCache || port != portCache || user != userCache || pass != passCache) {
    mqtt.disconnect();
    hostCache = host;
    portCache = port;
    userCache = user;
    passCache = pass;
  }
  if (WiFi.status() == WL_CONNECTED) {
#if defined(DMK_ROLE_CONTROLE)
    // O SERVE ja tem relogio proprio (DmkClock). O CONTROLE precisa de hora
    // para validar certificados HTTPS e assinaturas.
    if (!timeStarted) {
#if defined(DMK_BOARD_ESP8266)
      configTime("BRT3", "pool.ntp.org", "time.google.com");
#else
      configTzTime("BRT3", "pool.ntp.org", "time.google.com");
#endif
      timeStarted = true;
    }
#endif
    if (!mqtt.connected() && hostCache.length() && millis() - lastConnect > 15000UL) {
      lastConnect = millis();
      mqtt.setServer(hostCache.c_str(), portCache);
      String client = "ota-" + id;
      JsonDocument will;
      will["id"] = id;
      will["online"] = false;
      will["version"] = FIRMWARE_VERSION;
      will["target"] = DMK_OTA_TARGET;
      String offline;
      serializeJson(will, offline);
      bool ok = userCache.length()
                    ? mqtt.connect(client.c_str(), userCache.c_str(), passCache.c_str(), (base + "/status").c_str(), 0, true, offline.c_str())
                    : mqtt.connect(client.c_str(), (base + "/status").c_str(), 0, true, offline.c_str());
      if (ok) {
        mqtt.subscribe((base + "/cmd").c_str());
        publish();
      }
    }
    if (mqtt.connected()) mqtt.loop();
  }
  handlePending();
  RemoteOta::tick();
  if (prepared && !RemoteOta::busy()) {
    prepared = false;
    if (RemoteOta::finished()) {
      dmkEndMqttOta(true);
      publish();
      restartAt = millis() + 2500;
    } else {
      dmkEndMqttOta(false);
      publish();
    }
  }
  if (millis() - lastPublish >= (RemoteOta::busy() ? 1000UL : 15000UL)) {
    lastPublish = millis();
    publish();
  }
  if (restartAt && (int32_t)(millis() - restartAt) >= 0) ESP.restart();

  // Verificacao automatica (e instalacao automatica, se habilitada).
  if (!busy() && WiFi.status() == WL_CONNECTED && RemoteOta::configured() && time(nullptr) > 1700000000 &&
      (lastCheck == 0 || millis() - lastCheck >= (bridgeError.length() ? 600000UL : DMK_OTA_CHECK_INTERVAL_MS))) {
    lastCheck = millis();
    JsonDocument none;
    String error;
    queueTrusted("check", none.as<JsonVariantConst>(), error);
    autoQueued = RemoteOta::autoInstall();
  }
  if (autoQueued && !busy() && lastCheck && newer && RemoteOta::autoInstall() && dmkCanAutoUpdate()) {
    autoQueued = false;
    JsonDocument none;
    String error;
    Serial.println("[OTA] Instalacao automatica iniciada.");
    queueTrusted("install", none.as<JsonVariantConst>(), error);
  }
}
} // namespace MqttOta
