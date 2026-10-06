#include "RemoteOta.h"
#include "DmkCrypto.h"
#include "DmkPrefs.h"
#include "DmkRootCerts.h"
#include "DmkConfig.h"
#include <ArduinoJson.h>

#if defined(DMK_BOARD_ESP8266)
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <Updater.h>
#else
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#endif

namespace {

Preferences prefs;
const char *NS = "dmkota";

String target;
String installedVersion;
String cfgRepo;
String cfgManifestUrl;
String cfgToken;
String cfgCaCert;
bool allowLocalHttp = false;
bool autoInstallEnabled = false;

constexpr size_t MAX_MANIFEST_BYTES = 12288;
constexpr uint32_t NETWORK_TIMEOUT_MS = 20000UL;

String jsonEscape(const String &input) {
  String out;
  out.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); ++i) {
    char c = input[i];
    if (c == '\\') out += "\\\\";
    else if (c == '"') out += "\\\"";
    else if ((uint8_t)c >= 0x20) out += c;
  }
  return out;
}

bool validRepo(const String &repo) {
  int slash = repo.indexOf('/');
  if (slash <= 0 || slash != repo.lastIndexOf('/') || slash == (int)repo.length() - 1) return false;
  for (size_t i = 0; i < repo.length(); i++) {
    char c = repo[i];
    if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '/')) return false;
  }
  return repo.indexOf("SEU-USUARIO") < 0;
}

bool isHttps(const String &url) { return url.startsWith("https://"); }

String urlOrigin(const String &url) {
  if (!url.startsWith("https://") && !url.startsWith("http://")) return "";
  int start = isHttps(url) ? 8 : 7;
  int slash = url.indexOf('/', start);
  return slash < 0 ? url : url.substring(0, slash);
}

String urlDir(const String &url) {
  int q = url.indexOf('?');
  String clean = q >= 0 ? url.substring(0, q) : url;
  int slash = clean.lastIndexOf('/');
  return slash < 0 ? clean : clean.substring(0, slash + 1);
}

bool privateIpHost(const String &url) {
  int end = url.indexOf('/', 7);
  String host = url.substring(7, end < 0 ? url.length() : end);
  int colon = host.indexOf(':');
  if (colon >= 0) host = host.substring(0, colon);
  IPAddress ip;
  if (!ip.fromString(host)) return false;
  return ip[0] == 10 || (ip[0] == 192 && ip[1] == 168) || (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31);
}

bool allowedUrl(const String &url) {
  if (isHttps(url)) return true;
  return allowLocalHttp && url.startsWith("http://") && privateIpHost(url);
}

String manifestUrl() {
  if (cfgManifestUrl.length()) return cfgManifestUrl;
  String repo = cfgRepo.length() ? cfgRepo : String(DMK_GITHUB_REPO);
  if (!validRepo(repo)) return "";
  return "https://github.com/" + repo + "/releases/latest/download/manifest.json";
}

const char *caBundle() { return cfgCaCert.length() >= 100 ? cfgCaCert.c_str() : DMK_ROOT_CA_BUNDLE; }

int versionPart(const String &v, int index) {
  int part = 0, start = 0;
  String s = v;
  if (s.startsWith("v") || s.startsWith("V")) s = s.substring(1);
  while (start <= (int)s.length()) {
    int end = s.indexOf('.', start);
    if (end < 0) end = s.length();
    if (part == index) {
      int n = 0;
      for (int i = start; i < end && isdigit((unsigned char)s[i]); i++) n = n * 10 + (s[i] - '0');
      return n;
    }
    ++part;
    if (end >= (int)s.length()) break;
    start = end + 1;
  }
  return 0;
}

int compareVersions(const String &a, const String &b) {
  for (int i = 0; i < 4; ++i) {
    int av = versionPart(a, i), bv = versionPart(b, i);
    if (av != bv) return av > bv ? 1 : -1;
  }
  return 0;
}

// Cliente HTTP(S) compartilhado pelo manifesto e pelo download.
struct Net {
  HTTPClient http;
  WiFiClient plain;
#if defined(DMK_BOARD_ESP8266)
  BearSSL::WiFiClientSecure secure;
  BearSSL::X509List *trust = nullptr;
  String caCopy;
#else
  WiFiClientSecure secure;
#endif
  bool open(const String &url) {
    http.setTimeout(NETWORK_TIMEOUT_MS);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setUserAgent(String("DMK-OTA/") + installedVersion);
    if (!isHttps(url)) return http.begin(plain, url);
#if defined(DMK_BOARD_ESP8266)
    if (!trust) {
      caCopy = cfgCaCert.length() >= 100 ? cfgCaCert : String(FPSTR(DMK_ROOT_CA_BUNDLE));
      trust = new BearSSL::X509List(caCopy.c_str());
      caCopy = String();
    }
    secure.setTrustAnchors(trust);
    secure.setBufferSizes(16384, 1024);
    secure.setTimeout(NETWORK_TIMEOUT_MS);
#else
    secure.setCACert(caBundle());
    secure.setTimeout(NETWORK_TIMEOUT_MS / 1000);
#endif
    return http.begin(secure, url);
  }
  void close() {
    http.end();
#if defined(DMK_BOARD_ESP8266)
    delete trust;
    trust = nullptr;
#endif
  }
};

void addHeaders(HTTPClient &http) {
  if (cfgToken.length() > 0) http.addHeader("Authorization", "Bearer " + cfgToken);
}

bool parseManifest(const String &body, RemoteOta::Manifest &out, String &error) {
  JsonDocument filter;
  filter["device"] = true;
  filter["version"] = true;
  filter["url"] = true;
  filter["file"] = true;
  filter["size"] = true;
  filter["sha256"] = true;
  filter["targets"][target] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) {
    error = "Manifesto JSON invalido.";
    return false;
  }
  JsonVariantConst entry;
  if (doc["targets"].is<JsonObjectConst>()) {
    entry = doc["targets"][target];
    if (entry.isNull()) {
      error = "Esta versao nao tem firmware para " + target + ".";
      return false;
    }
    out.device = target;
  } else {
    entry = doc.as<JsonVariantConst>();
    out.device = String((const char *)(entry["device"] | ""));
  }
  out.version = String((const char *)(entry["version"] | ""));
  out.url = String((const char *)(entry["url"] | ""));
  String file = String((const char *)(entry["file"] | ""));
  out.sha256 = String((const char *)(entry["sha256"] | ""));
  out.sha256.toLowerCase();
  out.size = entry["size"] | 0;
  String mUrl = manifestUrl();
  if (out.url.length() == 0 && file.length() > 0) out.url = urlDir(mUrl) + file;
  if (out.url.startsWith("/")) out.url = urlOrigin(mUrl) + out.url;

  uint8_t shaBin[32];
  if (out.version.length() == 0 || out.url.length() == 0 || out.size == 0 ||
      !DmkCrypto::fromHex(out.sha256.c_str(), shaBin, 32)) {
    error = "Manifesto incompleto (version, url/file, size e sha256 sao obrigatorios).";
    return false;
  }
  if (out.device != target) {
    error = "Manifesto pertence a outra placa (" + out.device + ").";
    return false;
  }
  if (!allowedUrl(out.url)) {
    error = "Firmware recusado: use HTTPS (ou HTTP de IP local autorizado).";
    return false;
  }
  if (urlOrigin(out.url) != urlOrigin(mUrl)) {
    error = "Firmware recusado: precisa estar no mesmo servidor do manifesto.";
    return false;
  }
#if defined(DMK_BOARD_ESP8266)
  size_t freeSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
  if (out.size > freeSpace) {
    error = "Firmware maior que o espaco OTA livre.";
    return false;
  }
#endif
  return true;
}

bool fetchManifest(RemoteOta::Manifest &manifest, String &error) {
  String url = manifestUrl();
  if (url.length() == 0) {
    error = "Configure o repositorio GitHub (usuario/repositorio) ou a URL do manifesto.";
    return false;
  }
  if (!allowedUrl(url)) {
    error = "URL do manifesto precisa ser HTTPS (ou HTTP de IP local autorizado).";
    return false;
  }
  if (isHttps(url) && time(nullptr) < 1700000000) {
    error = "Relogio ainda nao sincronizado (NTP). Tente em instantes.";
    return false;
  }
  Net net;
  if (!net.open(url)) {
    error = "Nao foi possivel abrir a URL do manifesto.";
    return false;
  }
  addHeaders(net.http);
  net.http.addHeader("Accept", "application/json, application/octet-stream");
  int code = net.http.GET();
  if (code != HTTP_CODE_OK) {
    error = code == 404 ? String("Nenhuma versao publicada encontrada (HTTP 404).")
                        : "Servidor respondeu HTTP " + String(code) + ".";
    net.close();
    return false;
  }
  int length = net.http.getSize();
  if (length > (int)MAX_MANIFEST_BYTES) {
    error = "Manifesto maior que o permitido.";
    net.close();
    return false;
  }
  String body = net.http.getString();
  net.close();
  if (body.length() == 0 || body.length() > MAX_MANIFEST_BYTES) {
    error = "Manifesto vazio ou invalido.";
    return false;
  }
  return parseManifest(body, manifest, error);
}

void cancelUpdateSession() {
#if defined(DMK_BOARD_ESP8266)
  if (Update.isRunning()) {
    if (Update.isFinished()) {
      uint8_t unused = 0;
      Update.write(&unused, 1);
    }
    Update.end(false);
  }
#else
  Update.abort();
#endif
}

// ---- estado da instalacao em etapas ----
Net *dl = nullptr;
DmkCrypto::Sha256 shaCtx;
bool active = false, completed = false;
size_t received = 0;
uint32_t lastData = 0;
RemoteOta::Manifest release;
String phase = "idle", message = "", failure = "";

void closeDownload() {
  if (dl) {
    dl->close();
    delete dl;
    dl = nullptr;
  }
}

void fail(const String &error) {
  cancelUpdateSession();
  closeDownload();
  active = false;
  completed = false;
  phase = "error";
  message = error;
  failure = error;
  Serial.printf("[OTA] Falha: %s\n", error.c_str());
}

} // namespace

namespace RemoteOta {

bool begin(const char *targetName, const char *currentVersion) {
  target = targetName ? targetName : "";
  installedVersion = currentVersion ? currentVersion : "";
  if (!prefs.begin(NS, false)) return false;
  cfgRepo = prefs.isKey("repo") ? prefs.getString("repo", "") : String("");
  cfgManifestUrl = prefs.isKey("manifest") ? prefs.getString("manifest", "") : String("");
  cfgToken = prefs.isKey("token") ? prefs.getString("token", "") : String("");
  cfgCaCert = prefs.isKey("cacert") ? prefs.getString("cacert", "") : String("");
  allowLocalHttp = prefs.isKey("localhttp") ? prefs.getBool("localhttp", false) : false;
  autoInstallEnabled = prefs.isKey("auto") ? prefs.getBool("auto", false) : false;
  prefs.end();
  return true;
}

bool saveConfig(const String &repoIn, const String &manifestIn, const String &token, const String &caCert,
                String &error) {
  if (active) {
    error = "OTA em andamento";
    return false;
  }
  String repo = repoIn, url = manifestIn, ca = caCert;
  repo.trim();
  url.trim();
  ca.trim();
  if (repo.startsWith("https://github.com/")) repo = repo.substring(19);
  while (repo.endsWith("/")) repo.remove(repo.length() - 1);
  if (repo.length() && !validRepo(repo)) {
    error = "Repositorio invalido. Use o formato usuario/repositorio.";
    return false;
  }
  if (url.length() && !allowedUrl(url)) {
    error = "URL do manifesto precisa ser HTTPS (ou HTTP de IP local com autorizacao).";
    return false;
  }
  if (!prefs.begin(NS, false)) {
    error = "Memoria OTA indisponivel.";
    return false;
  }
  prefs.putString("repo", repo);
  prefs.putString("manifest", url);
  prefs.putString("token", token);
  prefs.putString("cacert", ca);
  bool ok = prefs.getString("repo", "") == repo && prefs.getString("manifest", "") == url &&
            prefs.getString("token", "") == token && prefs.getString("cacert", "") == ca;
  prefs.end();
  if (!ok) {
    error = "A memoria nao confirmou a configuracao OTA.";
    return false;
  }
  cfgRepo = repo;
  cfgManifestUrl = url;
  cfgToken = token;
  cfgCaCert = ca;
  return true;
}

bool setAutoInstall(bool enabled, String &error) {
  if (!prefs.begin(NS, false)) {
    error = "Memoria OTA indisponivel.";
    return false;
  }
  prefs.putBool("auto", enabled);
  bool ok = prefs.getBool("auto", !enabled) == enabled;
  prefs.end();
  if (!ok) {
    error = "Falha ao confirmar configuracao.";
    return false;
  }
  autoInstallEnabled = enabled;
  return true;
}

bool autoInstall() { return autoInstallEnabled; }

bool allowHttpLocal(bool enabled, String &error) {
  if (active) {
    error = "OTA em andamento";
    return false;
  }
  if (!prefs.begin(NS, false)) {
    error = "Memoria OTA indisponivel.";
    return false;
  }
  prefs.putBool("localhttp", enabled);
  bool ok = prefs.getBool("localhttp", !enabled) == enabled;
  prefs.end();
  if (!ok) {
    error = "Falha ao confirmar configuracao";
    return false;
  }
  allowLocalHttp = enabled;
  return true;
}

String effectiveManifestUrl() { return manifestUrl(); }
bool configured() { return manifestUrl().length() > 0 && allowedUrl(manifestUrl()); }

String configJson() {
  String repo = cfgRepo.length() ? cfgRepo : (validRepo(DMK_GITHUB_REPO) ? String(DMK_GITHUB_REPO) : String(""));
  String json = "{\"ok\":true";
  json += ",\"repo\":\"" + jsonEscape(repo) + "\"";
  json += ",\"manifest_url\":\"" + jsonEscape(cfgManifestUrl) + "\"";
  json += ",\"effective_url\":\"" + jsonEscape(manifestUrl()) + "\"";
  json += ",\"has_token\":" + String(cfgToken.length() ? "true" : "false");
  json += ",\"custom_ca\":" + String(cfgCaCert.length() >= 100 ? "true" : "false");
  json += ",\"configured\":" + String(configured() ? "true" : "false");
  json += ",\"allow_local_http\":" + String(allowLocalHttp ? "true" : "false");
  json += ",\"auto_install\":" + String(autoInstallEnabled ? "true" : "false");
  json += ",\"target\":\"" + target + "\",\"version\":\"" + installedVersion + "\"}";
  return json;
}

bool check(Manifest &manifest, String &error) { return fetchManifest(manifest, error); }
int compareWithInstalled(const String &remoteVersion) { return compareVersions(remoteVersion, installedVersion); }
bool busy() { return active; }
bool finished() { return completed; }

String statusJson() {
  unsigned progress = release.size ? (unsigned)((uint64_t)received * 100 / release.size) : 0;
  return "{\"phase\":\"" + phase + "\",\"message\":\"" + jsonEscape(message) + "\",\"error\":\"" +
         jsonEscape(failure) + "\",\"busy\":" + String(active ? "true" : "false") +
         ",\"received\":" + String((unsigned)received) + ",\"size\":" + String((unsigned)release.size) +
         ",\"progress\":" + String(progress) + ",\"version\":\"" + jsonEscape(release.version) + "\"}";
}

bool startInstall(bool force, String &error) {
  if (active) {
    error = "OTA ja em andamento";
    return false;
  }
  closeDownload();
  completed = false;
  received = 0;
  failure = "";
  phase = "preparing";
  message = "Consultando versao publicada";
  if (!fetchManifest(release, error)) {
    fail(error);
    return false;
  }
  int cmp = compareVersions(release.version, installedVersion);
  if (cmp < 0 || (!force && cmp == 0)) {
    error = "Versao " + release.version + " ja instalada (ou anterior). Nada a fazer.";
    fail(error);
    return false;
  }
  dl = new Net();
  if (!dl->open(release.url)) {
    error = "Falha ao abrir o arquivo de firmware";
    fail(error);
    return false;
  }
  addHeaders(dl->http);
  dl->http.addHeader("Accept", "application/octet-stream");
  int code = dl->http.GET();
  int size = dl->http.getSize();
  if (code != 200 || (size > 0 && size != (int)release.size)) {
    error = "Download recusado (HTTP " + String(code) + ", tamanho " + String(size) + ").";
    fail(error);
    return false;
  }
  if (!Update.begin(release.size)) {
    error = "Sem espaco OTA (particao) para " + String((unsigned)release.size) + " bytes.";
    fail(error);
    return false;
  }
  shaCtx.begin();
  active = true;
  lastData = millis();
  phase = "downloading";
  message = "Baixando e gravando " + release.version;
  Serial.printf("[OTA] Instalando %s (%u bytes)\n", release.version.c_str(), (unsigned)release.size);
  return true;
}

void tick() {
  if (!active || !dl) return;
  WiFiClient *stream = dl->http.getStreamPtr();
  if (!stream) {
    fail("Conexao de download perdida");
    return;
  }
  for (uint8_t round = 0; round < 8 && received < release.size; round++) {
    size_t avail = stream->available();
    if (!avail) break;
    uint8_t buffer[1024];
    size_t wanted = avail < sizeof(buffer) ? avail : sizeof(buffer);
    if (wanted > release.size - received) wanted = release.size - received;
    size_t got = stream->readBytes(buffer, wanted);
    if (!got) break;
    shaCtx.update(buffer, got);
    if (Update.write(buffer, got) != got) {
      fail("Falha gravando a particao OTA");
      return;
    }
    received += got;
    lastData = millis();
  }
  if (received >= release.size) {
    uint8_t digest[32];
    shaCtx.finish(digest);
    char hex[65];
    DmkCrypto::toHex(digest, 32, hex);
    if (release.sha256 != String(hex)) {
      fail("SHA-256 diferente do publicado: atualizacao cancelada");
      return;
    }
    if (!Update.end(false)) {
      fail("Imagem de firmware recusada pelo bootloader");
      return;
    }
    closeDownload();
    active = false;
    completed = true;
    phase = "restarting";
    message = "Firmware " + release.version + " validado. Reiniciando";
    Serial.println("[OTA] Firmware validado (SHA-256 OK).");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    fail("Wi-Fi caiu durante o download");
    return;
  }
  if ((uint32_t)(millis() - lastData) > 20000UL) fail("Download parado por 20 segundos");
}

bool installLatest(bool force, String &remoteVersion, size_t &written, String &error) {
  if (!startInstall(force, error)) return false;
  while (active) {
    tick();
    delay(1);
  }
  written = received;
  remoteVersion = release.version;
  if (!completed) {
    error = failure;
    return false;
  }
  return true;
}

} // namespace RemoteOta
