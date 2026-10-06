/*
  ============================================================================
  PROJETO: DMK SMART - SERVE (Monitor Trifasico + Bomba por Contatora)
  Comunicacao com o CONTROLE: 1) ESP-NOW  2) Rede local (UDP)  3) MQTT
  ============================================================================
  Hardware:
    - ESP32 Dev Module
    - 3 x ZMPT101B (um para cada fase R/S/T, medicao FASE-NEUTRO)
    - 1 x modulo rele para comandar a BOBINA DA CONTATORA
    - Contatora + protecao do motor instaladas separadamente

  Recursos:
    - Mede as 3 fases sem bloquear o loop.
    - Publica MQTT a cada 2 segundos.
    - Liga/desliga bomba por MQTT.
    - Estado da bomba publicado com retain.
    - Last Will MQTT: online/offline.
    - Wi-Fi salvo em NVS/Preferences.
    - Se nao houver Wi-Fi, abre AP de configuracao automaticamente.
    - Se nao houver Wi-Fi salvo, abre AP de configuracao automaticamente.
    - Com Wi-Fi salvo, o AP manual abre segurando FLASH/BOOT por 30 segundos.
    - Sem clientes conectados ao AP por 5 minutos, o AP e fechado.
    - Pagina web local para configuracao.
    - Protecao opcional de minima/maxima tensao.
    - O rele SEMPRE inicia desligado por seguranca.

  MQTT:
    - Usa o cliente MQTT nativo do ESP-IDF (esp-mqtt).
    - Nenhuma biblioteca MQTT externa é necessária.

  ATENCAO - REDE ELETRICA:
    - ZMPT101B 0-250 V deve medir FASE-NEUTRO.
    - NAO aplique 380 V fase-fase em modulo de entrada 0-250 V.
    - Se o ZMPT for alimentado com 5 V, NAO ligue OUT direto no ESP32.
      Use divisor/condicionamento para manter a entrada do ADC abaixo de ~3,0 V.
    - Rede eletrica, contatora e protecoes devem ficar em caixa apropriada.
    - O firmware NAO substitui disjuntor, rele termico, falta de fase,
      emergencia ou intertravamento eletrico.
  ============================================================================
*/

#include <Arduino.h>
#include <string>

#if defined(DMK_BOARD_ESP8266)

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <Updater.h>
#include <PubSubClient.h>
#include "DmkPrefs.h"

using DmkWebServer = ESP8266WebServer;

#else

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Update.h>
#include <LittleFS.h>
#include <esp_idf_version.h>
#include "mqtt_client.h"

using DmkWebServer = WebServer;

#endif

#include "PinsConfig.h"
#include "UserSettings.h"
#include "SensorConfig.h"
#include "firmware_info.h"
#include "MqttOta.h"
#include "RemoteOta.h"
#include <ArduinoJson.h>
#include "PumpHistory.h"
#include "DmkClock.h"
#include "AutomationManager.h"
#include "DmkConfig.h"
#include "DmkProto.h"
#include "DmkLink.h"
#include "DmkStatusLed.h"
#include "DmkCrypto.h"
#include "DmkWebAssets.h"

// ---------------------------------------------------------------------------
// PINOS
// ---------------------------------------------------------------------------
// ADC1: funcionam junto com Wi-Fi no ESP32 classico.
// ---------------------------------------------------------------------------
// OBJETOS
// ---------------------------------------------------------------------------
DmkWebServer server(80);
DNSServer dnsServer;
Preferences prefs;
#if defined(DMK_BOARD_ESP8266)
WiFiClient mqttNetClient8266;
PubSubClient mqttClient8266(mqttNetClient8266);
uint32_t lastMqttReconnectAt = 0;
#else
esp_mqtt_client_handle_t mqttClientNative = nullptr;
#endif

// ---------------------------------------------------------------------------
// CONFIGURACOES SALVAS
// ---------------------------------------------------------------------------
String cfgDeviceName = DEFAULT_DEVICE_NAME;
String cfgWifiSsid = DEFAULT_WIFI_SSID;
String cfgWifiPass = DEFAULT_WIFI_PASSWORD;

String cfgMqttHost = DEFAULT_MQTT_HOST;
uint16_t cfgMqttPort = DEFAULT_MQTT_PORT;
String cfgMqttUser = DEFAULT_MQTT_USER;
String cfgMqttPass = DEFAULT_MQTT_PASSWORD;
String cfgTopicBase = DEFAULT_MQTT_TOPIC;

bool cfgRelayActiveLow = true;
bool cfgLightRelayActiveLow = true;
String cfgOtaPassword = DEFAULT_OTA_PASSWORD;

// O fator de calibracao converte Vrms medido NO ADC para Vrms da rede.
// Cada ZMPT tem potenciometro e ganho diferente.
// Comece com estes valores e depois calibre com multimetro confiavel.
float cfgCalR = DEFAULT_CAL_R;
float cfgCalS = DEFAULT_CAL_S;
float cfgCalT = DEFAULT_CAL_T;

// Protecao opcional. Deixe desabilitada ate terminar a calibracao.
bool cfgProtectionEnabled = false;
float cfgVoltageMin = DEFAULT_VOLTAGE_MIN;
float cfgVoltageMax = DEFAULT_VOLTAGE_MAX;

// Calibracoes dos novos sensores.
float cfgCurrentAmpsPerVolt = DEFAULT_CURRENT_A_PER_V; // ACS712-20A: aproximadamente 10 A/V
float cfgFlowPulsesPerLiter = DEFAULT_FLOW_PULSES_L;   // YF-S201 tipico
uint16_t cfgRainDryRaw = DEFAULT_RAIN_DRY_RAW;
uint16_t cfgRainWetRaw = DEFAULT_RAIN_WET_RAW;
bool cfgAlarmActiveLow = true;
bool cfgPairShare = true; // envia Wi-Fi/MQTT ao controle durante o pareamento

// ---------------------------------------------------------------------------
// ESTADO
// ---------------------------------------------------------------------------
bool configApActive = false;
bool wifiWasConnected = false;
uint32_t wifiConnectStartedAt = 0;
uint32_t lastWifiRetryAt = 0;
uint32_t configButtonPressedAt = 0;
uint32_t configButtonLastLogAt = 0;
bool configButtonArmed = false;
bool configButtonTriggered = false;
uint32_t configApNoClientSince = 0;
uint8_t configApLastClientCount = 0;
bool configApOpenedManually = false;
uint32_t scheduledRestartAt = 0;
bool otaUploadOk = false;
bool otaInProgress = false;

volatile bool mqttJustConnected = false;

bool mqttConfigured = false;
bool mqttStarted = false;
volatile bool mqttConnected = false;
bool webServerConfigured = false;
bool webServerStarted = false;
bool littleFsReady = false;
bool wifiScanInProgress = false;

bool pumpOn = false;
bool lightOn = false;

bool measurementReady = false;
float voltageR = 0.0f;
float voltageS = 0.0f;
float voltageT = 0.0f;

// Vrms presente no pino ADC antes do fator de calibracao.
// Muito util para calibrar.
float adcVrmsR = 0.0f;
float adcVrmsS = 0.0f;
float adcVrmsT = 0.0f;
float currentA = 0.0f;
float temperatureC = 0.0f;
float rainPercent = 0.0f;
float flowLitersMinute = 0.0f;
float totalLiters = 0.0f;
bool alarmActive = false;
volatile uint32_t flowPulseCount = 0;
uint32_t lastFlowCalcMs = 0;
uint32_t lastFlowPulseSnapshot = 0;

bool protectionOk = false;
String protectionReason = "AGUARDANDO_MEDICAO";

// Topicos mantidos em std::string para a biblioteca MQTT.
std::string topicTelemetry;
std::string topicPumpSet;
std::string topicPumpState;
std::string topicAvailability;
std::string topicLightSet;
std::string topicLightState;

// Historico remoto via MQTT/WebSocket.
// O ESP continua usando MQTT TCP; o navegador usa WebSocket no mesmo broker.
std::string topicPumpHistoryGet;
std::string topicPumpHistoryResponse;
std::string topicPumpHistoryClear;

// Automacoes remotas pelo mesmo broker MQTT usado pelo aplicativo.
std::string topicScheduleSet;
std::string topicScheduleGet;
std::string topicScheduleState;
std::string topicScheduleEvent;
uint32_t lastScheduleStatePublishMs = 0;

// Enlace com os controles (ESP-NOW + UDP).
uint32_t linkSession = 0;
uint32_t linkPrevSession = 0;
uint32_t linkSessionAt = 0;
uint32_t lastEspNowStatusMs = 0;
uint32_t lastLanStatusMs = 0;
uint8_t lastLinkSignature = 0xFF;
bool linkStatusDirty = true;
uint32_t mqttSubscribedAt = 0;

// ---------------------------------------------------------------------------
// ACUMULADOR RMS - ALGORITMO DE WELFORD
// Calcula a variancia sem precisar guardar centenas de amostras na RAM.
// ---------------------------------------------------------------------------
struct RmsAccumulator
{
  uint32_t n = 0;
  double mean = 0.0;
  double m2 = 0.0;

  void reset()
  {
    n = 0;
    mean = 0.0;
    m2 = 0.0;
  }

  void add(double x)
  {
    n++;
    double delta = x - mean;
    mean += delta / (double)n;
    double delta2 = x - mean;
    m2 += delta * delta2;
  }

  double rmsAc() const
  {
    if (n < 2)
      return 0.0;
    // Desvio padrao da componente AC em torno do offset DC.
    return sqrt(m2 / (double)n);
  }
};

RmsAccumulator accR, accS, accT, accCurrent;
bool samplingActive = false;
uint32_t sampleWindowStartedUs = 0;
uint32_t lastSampleUs = 0;
uint32_t lastMeasureCycleMs = 0;

// ---------------------------------------------------------------------------
// UTILITARIOS
// ---------------------------------------------------------------------------
String chipSuffix()
{
#if defined(DMK_BOARD_ESP8266)
  uint32_t id = ESP.getChipId();
  char buf[7];
  snprintf(buf, sizeof(buf), "%06lX", (unsigned long)(id & 0xFFFFFFUL));
#else
  uint64_t id = ESP.getEfuseMac();
  char buf[7];
  snprintf(buf, sizeof(buf), "%06lX", (unsigned long)(id & 0xFFFFFFULL));
#endif
  return String(buf);
}

String apSsid()
{
  return String(CONFIG_AP_PREFIX) + chipSuffix();
}

// Declaracoes antecipadas usadas pelo salvamento de configuracao.
void applyPumpOutput(bool turnOn);
void applyLightOutput(bool turnOn);
void sendJsonMessage(int code, bool ok, const String &message);

// ---------------------------------------------------------------------------
// PREFERENCES / NVS
// ---------------------------------------------------------------------------
void loadConfig()
{
  Serial.println("[BOOT] 1/6 - Abrindo configuracoes NVS...");

  if (!prefs.begin("energycfg", false))
  {
    Serial.println("[NVS][ERRO] Falha ao abrir Preferences. Usando valores padrao.");
    return;
  }

  // isKey evita mensagens NOT_FOUND no primeiro boot e permite distinguir
  // "chave ainda nao criada" de uma string vazia salva intencionalmente.
  cfgDeviceName = prefs.isKey("devName") ? prefs.getString("devName", DEFAULT_DEVICE_NAME) : String(DEFAULT_DEVICE_NAME);
  cfgWifiSsid = prefs.isKey("ssid") ? prefs.getString("ssid", "") : String("");
  cfgWifiPass = prefs.isKey("wifiPass") ? prefs.getString("wifiPass", "") : String("");

  cfgMqttHost = prefs.isKey("mqHost") ? prefs.getString("mqHost", DEFAULT_MQTT_HOST) : String(DEFAULT_MQTT_HOST);
  cfgMqttPort = prefs.isKey("mqPort") ? prefs.getUShort("mqPort", DEFAULT_MQTT_PORT) : DEFAULT_MQTT_PORT;
  cfgMqttUser = prefs.isKey("mqUser") ? prefs.getString("mqUser", DEFAULT_MQTT_USER) : String(DEFAULT_MQTT_USER);
  cfgMqttPass = prefs.isKey("mqPass") ? prefs.getString("mqPass", DEFAULT_MQTT_PASSWORD) : String(DEFAULT_MQTT_PASSWORD);
  cfgTopicBase = prefs.isKey("topic") ? prefs.getString("topic", DEFAULT_MQTT_TOPIC) : String(DEFAULT_MQTT_TOPIC);

  cfgRelayActiveLow = prefs.isKey("relayLow") ? prefs.getBool("relayLow", true) : true;
  cfgLightRelayActiveLow = prefs.isKey("lightLow") ? prefs.getBool("lightLow", true) : true;
  cfgOtaPassword = prefs.isKey("otaPass") ? prefs.getString("otaPass", DEFAULT_OTA_PASSWORD) : String(DEFAULT_OTA_PASSWORD);

  cfgCalR = prefs.isKey("calR") ? prefs.getFloat("calR", DEFAULT_CAL_R) : DEFAULT_CAL_R;
  cfgCalS = prefs.isKey("calS") ? prefs.getFloat("calS", DEFAULT_CAL_S) : DEFAULT_CAL_S;
  cfgCalT = prefs.isKey("calT") ? prefs.getFloat("calT", DEFAULT_CAL_T) : DEFAULT_CAL_T;

  cfgProtectionEnabled = prefs.isKey("prot") ? prefs.getBool("prot", false) : false;
  cfgVoltageMin = prefs.isKey("vMin") ? prefs.getFloat("vMin", DEFAULT_VOLTAGE_MIN) : DEFAULT_VOLTAGE_MIN;
  cfgVoltageMax = prefs.isKey("vMax") ? prefs.getFloat("vMax", DEFAULT_VOLTAGE_MAX) : DEFAULT_VOLTAGE_MAX;
  cfgCurrentAmpsPerVolt = prefs.isKey("curCal") ? prefs.getFloat("curCal", DEFAULT_CURRENT_A_PER_V) : DEFAULT_CURRENT_A_PER_V;
  cfgFlowPulsesPerLiter = prefs.isKey("flowCal") ? prefs.getFloat("flowCal", DEFAULT_FLOW_PULSES_L) : DEFAULT_FLOW_PULSES_L;
  cfgRainDryRaw = prefs.isKey("rainDry") ? prefs.getUShort("rainDry", DEFAULT_RAIN_DRY_RAW) : DEFAULT_RAIN_DRY_RAW;
  cfgRainWetRaw = prefs.isKey("rainWet") ? prefs.getUShort("rainWet", DEFAULT_RAIN_WET_RAW) : DEFAULT_RAIN_WET_RAW;
  cfgAlarmActiveLow = prefs.isKey("alarmLow") ? prefs.getBool("alarmLow", true) : true;
  cfgPairShare = prefs.isKey("pairShare") ? prefs.getBool("pairShare", true) : true;

  prefs.end();

  cfgDeviceName.trim();
  cfgWifiSsid.trim();
  cfgMqttHost.trim();
  cfgMqttUser.trim();
  cfgTopicBase.trim();
  cfgOtaPassword.trim();

  if (cfgDeviceName.length() == 0) cfgDeviceName = DEFAULT_DEVICE_NAME;
  if (cfgMqttHost.length() == 0) cfgMqttHost = DEFAULT_MQTT_HOST;
  if (cfgMqttPort == 0) cfgMqttPort = DEFAULT_MQTT_PORT;
  if (cfgMqttHost == "broker.hivemq.com" && cfgMqttPort == 8000) cfgMqttPort = 1883;
  if (cfgTopicBase.length() == 0) cfgTopicBase = DEFAULT_MQTT_TOPIC;
  if (cfgOtaPassword.length() < 8) cfgOtaPassword = DEFAULT_OTA_PASSWORD;

  while (cfgTopicBase.endsWith("/")) cfgTopicBase.remove(cfgTopicBase.length() - 1);

  // Sanitizacao defensiva contra NVS antiga/corrompida.
  if (!isfinite(cfgCalR) || cfgCalR <= 0.0f || cfgCalR > 5000.0f) cfgCalR = DEFAULT_CAL_R;
  if (!isfinite(cfgCalS) || cfgCalS <= 0.0f || cfgCalS > 5000.0f) cfgCalS = DEFAULT_CAL_S;
  if (!isfinite(cfgCalT) || cfgCalT <= 0.0f || cfgCalT > 5000.0f) cfgCalT = DEFAULT_CAL_T;
  if (!isfinite(cfgVoltageMin) || cfgVoltageMin < 0.0f || cfgVoltageMin > 500.0f) cfgVoltageMin = DEFAULT_VOLTAGE_MIN;
  if (!isfinite(cfgVoltageMax) || cfgVoltageMax <= cfgVoltageMin || cfgVoltageMax > 600.0f) cfgVoltageMax = DEFAULT_VOLTAGE_MAX;
  if (!isfinite(cfgCurrentAmpsPerVolt) || cfgCurrentAmpsPerVolt <= 0.0f || cfgCurrentAmpsPerVolt > 1000.0f) cfgCurrentAmpsPerVolt = DEFAULT_CURRENT_A_PER_V;
  if (!isfinite(cfgFlowPulsesPerLiter) || cfgFlowPulsesPerLiter <= 0.0f || cfgFlowPulsesPerLiter > 100000.0f) cfgFlowPulsesPerLiter = DEFAULT_FLOW_PULSES_L;
  if (cfgRainDryRaw > 4095) cfgRainDryRaw = DEFAULT_RAIN_DRY_RAW;
  if (cfgRainWetRaw > 4095) cfgRainWetRaw = DEFAULT_RAIN_WET_RAW;
  if (cfgRainDryRaw == cfgRainWetRaw) { cfgRainDryRaw = DEFAULT_RAIN_DRY_RAW; cfgRainWetRaw = DEFAULT_RAIN_WET_RAW; }

  Serial.printf("[NVS] Wi-Fi salvo: %s | SSID='%s'\n", cfgWifiSsid.length() ? "SIM" : "NAO", cfgWifiSsid.c_str());
  Serial.printf("[NVS] Polaridade carregada: BOMBA_LOW=%s LUZ_LOW=%s ALARME_LOW=%s\n",
                cfgRelayActiveLow ? "SIM" : "NAO",
                cfgLightRelayActiveLow ? "SIM" : "NAO",
                cfgAlarmActiveLow ? "SIM" : "NAO");
  Serial.printf("[NVS] Protecao: %s | VMIN=%.1f VMAX=%.1f | CUR=%.3f FLOW=%.1f | CHUVA=%u/%u\n",
                cfgProtectionEnabled ? "ON" : "OFF",
                cfgVoltageMin, cfgVoltageMax, cfgCurrentAmpsPerVolt,
                cfgFlowPulsesPerLiter, (unsigned)cfgRainDryRaw, (unsigned)cfgRainWetRaw);
  Serial.println("[NVS] Configuracoes carregadas.");
}

bool webArgTrue(const char *name)
{
  if (!server.hasArg(name))
    return false;

  String value = server.arg(name);
  value.trim();
  value.toLowerCase();
  return value == "1" || value == "true" || value == "on" || value == "yes";
}

void saveConfigFromWeb()
{
  Serial.printf("[CONFIG] POST /save recebido. args=%d configApActive=%s\n",
                server.args(), configApActive ? "SIM" : "NAO");

  if (!configApActive)
  {
    sendJsonMessage(403, false, "Abra o modo de configuracao segurando FLASH/BOOT por 30 segundos.");
    return;
  }

  // O painel atual sempre envia o conjunto completo. Recusar POST parcial evita
  // apagar configuracoes por uma pagina velha, cache antigo ou requisicao incompleta.
  const char *requiredArgs[] = {
      "devName", "ssid", "wifiPass", "mqttHost", "mqttPort", "mqttUser", "mqttPass", "topic",
      "relayLow", "lightLow", "alarmLow", "protection",
      "calR", "calS", "calT", "vMin", "vMax", "currentCal", "flowCal", "rainDry", "rainWet", "otaPass"};
  for (const char *arg : requiredArgs)
  {
    if (!server.hasArg(arg))
    {
      Serial.printf("[CONFIG][ERRO] Campo ausente no POST: %s\n", arg);
      sendJsonMessage(400, false, String("Formulario incompleto. Atualize a pagina e tente novamente. Campo ausente: ") + arg);
      return;
    }
  }

  String newDevName = server.arg("devName");
  String newSsid = server.arg("ssid");
  String newWifiPass = server.arg("wifiPass");
  String newMqttHost = server.arg("mqttHost");
  String newMqttUser = server.arg("mqttUser");
  String newMqttPass = server.arg("mqttPass");
  String newTopic = server.arg("topic");
  String newOtaPassword = server.arg("otaPass");

  newDevName.trim(); newSsid.trim(); newMqttHost.trim(); newMqttUser.trim(); newTopic.trim();
  if (newDevName.length() == 0) newDevName = DEFAULT_DEVICE_NAME;
  if (newMqttHost.length() == 0) newMqttHost = DEFAULT_MQTT_HOST;
  if (newTopic.length() == 0) newTopic = DEFAULT_MQTT_TOPIC;
  while (newTopic.endsWith("/")) newTopic.remove(newTopic.length() - 1);

  long portLong = server.arg("mqttPort").toInt();
  if (portLong < 1 || portLong > 65535)
  {
    sendJsonMessage(400, false, "Porta MQTT invalida. Use 1 a 65535.");
    return;
  }
  uint16_t newPort = (uint16_t)portLong;

  float newCalR = server.arg("calR").toFloat();
  float newCalS = server.arg("calS").toFloat();
  float newCalT = server.arg("calT").toFloat();
  float newVMin = server.arg("vMin").toFloat();
  float newVMax = server.arg("vMax").toFloat();
  float newCurrentCal = server.arg("currentCal").toFloat();
  float newFlowCal = server.arg("flowCal").toFloat();
  int newRainDry = server.arg("rainDry").toInt();
  int newRainWet = server.arg("rainWet").toInt();

  if (!isfinite(newCalR) || newCalR <= 0.0f || newCalR > 5000.0f ||
      !isfinite(newCalS) || newCalS <= 0.0f || newCalS > 5000.0f ||
      !isfinite(newCalT) || newCalT <= 0.0f || newCalT > 5000.0f)
  {
    sendJsonMessage(400, false, "Fatores ZMPT invalidos.");
    return;
  }
  if (!isfinite(newVMin) || !isfinite(newVMax) || newVMin < 0.0f || newVMax <= newVMin || newVMax > 600.0f)
  {
    sendJsonMessage(400, false, "Limites de tensao invalidos.");
    return;
  }
  if (!isfinite(newCurrentCal) || newCurrentCal <= 0.0f || newCurrentCal > 1000.0f)
  {
    sendJsonMessage(400, false, "Calibracao de corrente invalida.");
    return;
  }
  if (!isfinite(newFlowCal) || newFlowCal <= 0.0f || newFlowCal > 100000.0f)
  {
    sendJsonMessage(400, false, "Calibracao de fluxo invalida.");
    return;
  }
  if (newRainDry < 0 || newRainDry > 4095 || newRainWet < 0 || newRainWet > 4095 || newRainDry == newRainWet)
  {
    sendJsonMessage(400, false, "Calibracao de chuva invalida: seco e molhado devem ser diferentes e ficar entre 0 e 4095.");
    return;
  }
  if (newOtaPassword.length() > 0 && newOtaPassword.length() < 8)
  {
    sendJsonMessage(400, false, "Nova senha OTA precisa ter pelo menos 8 caracteres.");
    return;
  }

  bool newRelayLow = webArgTrue("relayLow");
  bool newLightLow = webArgTrue("lightLow");
  bool newProtection = webArgTrue("protection");
  bool newAlarmLow = webArgTrue("alarmLow");

  // Senha vazia = manter a atual, exceto ao limpar SSID.
  String wifiPassToStore = (newWifiPass.length() > 0) ? newWifiPass : cfgWifiPass;
  if (newSsid.length() == 0) wifiPassToStore = "";
  String mqttPassToStore = (newMqttPass.length() > 0) ? newMqttPass : cfgMqttPass;
  String otaPassToStore = (newOtaPassword.length() >= 8) ? newOtaPassword : cfgOtaPassword;

  if (!prefs.begin("energycfg", false))
  {
    Serial.println("[CONFIG][ERRO] Nao foi possivel abrir namespace energycfg para escrita.");
    sendJsonMessage(500, false, "Falha ao abrir a memoria NVS para salvar configuracoes.");
    return;
  }

  // Grava tudo; a verificacao real e feita por leitura de volta logo abaixo.
  prefs.putString("devName", newDevName);
  prefs.putString("ssid", newSsid);
  prefs.putString("wifiPass", wifiPassToStore);
  prefs.putString("mqHost", newMqttHost);
  prefs.putUShort("mqPort", newPort);
  prefs.putString("mqUser", newMqttUser);
  prefs.putString("mqPass", mqttPassToStore);
  prefs.putString("topic", newTopic);
  prefs.putBool("relayLow", newRelayLow);
  prefs.putBool("lightLow", newLightLow);
  prefs.putBool("alarmLow", newAlarmLow);
  prefs.putString("otaPass", otaPassToStore);
  prefs.putFloat("calR", newCalR);
  prefs.putFloat("calS", newCalS);
  prefs.putFloat("calT", newCalT);
  prefs.putBool("prot", newProtection);
  prefs.putFloat("vMin", newVMin);
  prefs.putFloat("vMax", newVMax);
  prefs.putFloat("curCal", newCurrentCal);
  prefs.putFloat("flowCal", newFlowCal);
  prefs.putUShort("rainDry", (uint16_t)newRainDry);
  prefs.putUShort("rainWet", (uint16_t)newRainWet);

  const bool verifyOk =
      prefs.getString("devName", "") == newDevName &&
      prefs.getString("ssid", "") == newSsid &&
      prefs.getString("wifiPass", "") == wifiPassToStore &&
      prefs.getString("mqHost", "") == newMqttHost &&
      prefs.getUShort("mqPort", 0) == newPort &&
      prefs.getString("mqUser", "") == newMqttUser &&
      prefs.getString("mqPass", "") == mqttPassToStore &&
      prefs.getString("topic", "") == newTopic &&
      prefs.getBool("relayLow", !newRelayLow) == newRelayLow &&
      prefs.getBool("lightLow", !newLightLow) == newLightLow &&
      prefs.getBool("alarmLow", !newAlarmLow) == newAlarmLow &&
      prefs.getBool("prot", !newProtection) == newProtection &&
      fabsf(prefs.getFloat("calR", -1.0f) - newCalR) < 0.0005f &&
      fabsf(prefs.getFloat("calS", -1.0f) - newCalS) < 0.0005f &&
      fabsf(prefs.getFloat("calT", -1.0f) - newCalT) < 0.0005f &&
      fabsf(prefs.getFloat("vMin", -1.0f) - newVMin) < 0.0005f &&
      fabsf(prefs.getFloat("vMax", -1.0f) - newVMax) < 0.0005f &&
      fabsf(prefs.getFloat("curCal", -1.0f) - newCurrentCal) < 0.0005f &&
      fabsf(prefs.getFloat("flowCal", -1.0f) - newFlowCal) < 0.0005f &&
      prefs.getUShort("rainDry", 65535) == (uint16_t)newRainDry &&
      prefs.getUShort("rainWet", 65535) == (uint16_t)newRainWet;

  prefs.end();

  if (!verifyOk)
  {
    Serial.println("[CONFIG][ERRO] Verificacao NVS falhou. Reinicio CANCELADO.");
    sendJsonMessage(500, false, "A memoria nao confirmou todos os valores. Nada sera reiniciado; tente novamente.");
    return;
  }

  // Atualiza as variaveis em RAM imediatamente. Isto e especialmente importante
  // para a polaridade do rele: OFF passa a usar a NOVA configuracao antes do reboot.
  cfgDeviceName = newDevName;
  cfgWifiSsid = newSsid;
  cfgWifiPass = wifiPassToStore;
  cfgMqttHost = newMqttHost;
  cfgMqttPort = newPort;
  cfgMqttUser = newMqttUser;
  cfgMqttPass = mqttPassToStore;
  cfgTopicBase = newTopic;
  cfgRelayActiveLow = newRelayLow;
  cfgLightRelayActiveLow = newLightLow;
  cfgAlarmActiveLow = newAlarmLow;
  cfgOtaPassword = otaPassToStore;
  cfgCalR = newCalR; cfgCalS = newCalS; cfgCalT = newCalT;
  cfgProtectionEnabled = newProtection;
  cfgVoltageMin = newVMin; cfgVoltageMax = newVMax;
  cfgCurrentAmpsPerVolt = newCurrentCal;
  cfgFlowPulsesPerLiter = newFlowCal;
  cfgRainDryRaw = (uint16_t)newRainDry;
  cfgRainWetRaw = (uint16_t)newRainWet;

  applyPumpOutput(false);
  applyLightOutput(false);

  Serial.printf("[CONFIG][OK] SSID='%s' MQTT='%s:%u' TOPIC='%s'\n",
                cfgWifiSsid.c_str(), cfgMqttHost.c_str(), cfgMqttPort, cfgTopicBase.c_str());
  Serial.printf("[CONFIG][OK] Polaridade aplicada AGORA: BOMBA_LOW=%s LUZ_LOW=%s ALARME_LOW=%s | GPIO bomba OFF=%s\n",
                cfgRelayActiveLow ? "SIM" : "NAO",
                cfgLightRelayActiveLow ? "SIM" : "NAO",
                cfgAlarmActiveLow ? "SIM" : "NAO",
                digitalRead(PIN_RELAY) == HIGH ? "HIGH" : "LOW");
  Serial.println("[CONFIG][OK] Gravacao NVS conferida por leitura. Reinicio agendado.");

  server.send(200, "application/json; charset=utf-8",
              "{\"ok\":true,\"message\":\"Configuracao gravada e verificada. O ESP32 vai reiniciar.\"}");
  scheduledRestartAt = millis() + 1800;
}

// ---------------------------------------------------------------------------
// RELE / CONTATORA
// ---------------------------------------------------------------------------
int pumpOutputLevel(bool turnOn)
{
  return cfgRelayActiveLow ? (turnOn ? LOW : HIGH)
                           : (turnOn ? HIGH : LOW);
}

int lightOutputLevel(bool turnOn)
{
  return cfgLightRelayActiveLow ? (turnOn ? LOW : HIGH)
                                : (turnOn ? HIGH : LOW);
}

void applyPumpOutput(bool turnOn)
{
  if(turnOn&&(otaInProgress||MqttOta::busy()))return;
  pumpOn = turnOn;
  digitalWrite(PIN_RELAY, pumpOutputLevel(turnOn));
}

void applyLightOutput(bool turnOn)
{
  if(turnOn&&(otaInProgress||MqttOta::busy()))return;
  lightOn = turnOn;
  digitalWrite(PIN_LIGHT_RELAY, lightOutputLevel(turnOn));
}

// Estado seguro de partida. O nivel de OFF e carregado na trava do GPIO
// antes de trocar o pino para OUTPUT, reduzindo pulsos indevidos na inicializacao.
void initializeActuatorOutputsSafe()
{
  const int pumpOffLevel = pumpOutputLevel(false);
  const int lightOffLevel = lightOutputLevel(false);

  digitalWrite(PIN_RELAY, pumpOffLevel);
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, pumpOffLevel);
  pumpOn = false;

  digitalWrite(PIN_LIGHT_RELAY, lightOffLevel);
  pinMode(PIN_LIGHT_RELAY, OUTPUT);
  digitalWrite(PIN_LIGHT_RELAY, lightOffLevel);
  lightOn = false;

  Serial.printf("[GPIO] Bomba iniciada OFF | activeLow=%s | nivelOFF=%s\n",
                cfgRelayActiveLow ? "SIM" : "NAO",
                pumpOffLevel == HIGH ? "HIGH" : "LOW");
  Serial.printf("[GPIO] Luz iniciada OFF | activeLow=%s | nivelOFF=%s\n",
                cfgLightRelayActiveLow ? "SIM" : "NAO",
                lightOffLevel == HIGH ? "HIGH" : "LOW");
}

void IRAM_ATTR onFlowPulse()
{
  flowPulseCount = flowPulseCount + 1;
}

void publishPumpState();
void publishLightState();
String jsonEscape(const String &input);

void scheduledPumpCommand(bool turnOn, uint32_t automationId);
void scheduledLightCommand(bool turnOn, uint32_t automationId);

void publishAutomationStateMqtt();
void publishAutomationEventMqtt(uint32_t automationId,
                                const char *target,
                                const char *state,
                                const char *status,
                                const char *reason = "");

bool evaluateProtection()
{
  if (!measurementReady)
  {
    protectionReason = "AGUARDANDO_MEDICAO";
    protectionOk = false;
    return false;
  }

  if (!cfgProtectionEnabled)
  {
    protectionReason = "DESABILITADA";
    protectionOk = true;
    return true;
  }

  if (voltageR < cfgVoltageMin)
  {
    protectionReason = "FASE_R_BAIXA";
    protectionOk = false;
    return false;
  }
  if (voltageR > cfgVoltageMax)
  {
    protectionReason = "FASE_R_ALTA";
    protectionOk = false;
    return false;
  }

#if PIN_ZMPT_S >= 0
  if (voltageS < cfgVoltageMin) { protectionReason = "FASE_S_BAIXA"; protectionOk = false; return false; }
  if (voltageS > cfgVoltageMax) { protectionReason = "FASE_S_ALTA"; protectionOk = false; return false; }
#endif
#if PIN_ZMPT_T >= 0
  if (voltageT < cfgVoltageMin) { protectionReason = "FASE_T_BAIXA"; protectionOk = false; return false; }
  if (voltageT > cfgVoltageMax) { protectionReason = "FASE_T_ALTA"; protectionOk = false; return false; }
#endif

  protectionReason = "OK";
  protectionOk = true;
  return true;
}

void processPumpCommand()
{
  PumpCommandContext ctx;

  // O modulo PumpHistory mantem uma fila interna de um comando.
  // Se nao houver comando pendente, nao ha nada para fazer.
  if (!PumpHistory::takePending(ctx))
    return;

  uint32_t scheduledAutomationId = 0;
  if (strcmp(ctx.source, "SCHEDULE") == 0 &&
      strncmp(ctx.requestId, "schedule-", 9) == 0)
  {
    scheduledAutomationId = (uint32_t)strtoul(ctx.requestId + 9, nullptr, 10);
  }

  // Garantia extra: localizacao ausente/invalida nunca bloqueia a bomba.
  // O modulo ja entrega 0.0/0.0 quando nao existe GPS valido.
  if (!ctx.hasLocation)
  {
    ctx.latitude = 0.0;
    ctx.longitude = 0.0;
    ctx.accuracyM = 0.0f;
  }

  // -------------------------------------------------------------------------
  // DESLIGAR
  // -------------------------------------------------------------------------
  if (ctx.command == 0)
  {
    bool mudouEstado = pumpOn;

    applyPumpOutput(false);
    publishPumpState();

    // Historico registra transicao real ON -> OFF.
    if (mudouEstado)
    {
      PumpHistory::logEvent(false, ctx, "EXECUTADO", "");
    }

    Serial.printf("[BOMBA] OFF | origem=%s | GPS=%s | lat=%.7f lon=%.7f\n",
                  ctx.source,
                  ctx.hasLocation ? "SIM" : "NAO",
                  ctx.latitude,
                  ctx.longitude);

    if (scheduledAutomationId > 0)
    {
      publishAutomationEventMqtt(
          scheduledAutomationId,
          "PUMP",
          "OFF",
          "EXECUTADO",
          "");
      publishAutomationStateMqtt();
    }
    return;
  }

  // -------------------------------------------------------------------------
  // LIGAR
  // -------------------------------------------------------------------------
  if (ctx.command == 1)
  {
    if (evaluateProtection())
    {
      bool mudouEstado = !pumpOn;

      applyPumpOutput(true);
      publishPumpState();

      // Historico registra transicao real OFF -> ON.
      if (mudouEstado)
      {
        PumpHistory::logEvent(true, ctx, "EXECUTADO", "");
      }

      Serial.printf("[BOMBA] ON | origem=%s | GPS=%s | lat=%.7f lon=%.7f\n",
                    ctx.source,
                    ctx.hasLocation ? "SIM" : "NAO",
                    ctx.latitude,
                    ctx.longitude);

      if (scheduledAutomationId > 0)
      {
        publishAutomationEventMqtt(
            scheduledAutomationId,
            "PUMP",
            "ON",
            "EXECUTADO",
            "");
        publishAutomationStateMqtt();
      }
    }
    else
    {
      // A tentativa de ligar e registrada mesmo que a protecao bloqueie.
      applyPumpOutput(false);
      publishPumpState();
      PumpHistory::logEvent(false, ctx, "BLOQUEADO", protectionReason.c_str());

      Serial.printf("[BOMBA] ON bloqueado pela protecao: %s\n",
                    protectionReason.c_str());

      if (scheduledAutomationId > 0)
      {
        publishAutomationEventMqtt(
            scheduledAutomationId,
            "PUMP",
            "ON",
            "BLOQUEADO",
            protectionReason.c_str());
        publishAutomationStateMqtt();
      }
    }
  }
}

// ---------------------------------------------------------------------------
// AUTOMACOES DE HORARIO
// ---------------------------------------------------------------------------
// A bomba programada NAO aciona o rele diretamente.
// Ela entra na mesma fila dos outros comandos e passa por evaluateProtection()
// dentro de processPumpCommand().
void scheduledPumpCommand(bool turnOn, uint32_t automationId)
{
  char requestId[40];
  snprintf(requestId, sizeof(requestId), "schedule-%lu",
           (unsigned long)automationId);

  if (!PumpHistory::queueCommand(
          turnOn,
          "SCHEDULE",
          false,
          0.0,
          0.0,
          0.0f,
          requestId))
  {
    Serial.printf("[AUTO] Falha ao enfileirar bomba #%lu\n",
                  (unsigned long)automationId);
  }
}

void scheduledLightCommand(bool turnOn, uint32_t automationId)
{
  applyLightOutput(turnOn);
  publishLightState();

  publishAutomationEventMqtt(
      automationId,
      "LIGHT",
      turnOn ? "ON" : "OFF",
      "EXECUTADO",
      "");

  publishAutomationStateMqtt();

  Serial.printf("[AUTO] Luz #%lu executada: %s | hora ESP=%s\n",
                (unsigned long)automationId,
                turnOn ? "ON" : "OFF",
                DmkClock::localDateTime().c_str());
}

// ---------------------------------------------------------------------------
// MQTT - ESP32/S3 usa ESP-MQTT nativo; ESP8266 usa PubSubClient.
// ---------------------------------------------------------------------------
bool mqttCanPublish()
{
#if defined(DMK_BOARD_ESP8266)
  return mqttStarted && mqttConnected && mqttClient8266.connected();
#else
  return mqttStarted && mqttConnected && mqttClientNative != nullptr;
#endif
}

int dmkMqttPublish(const char *topic, const char *payload, int len, int qos, bool retain)
{
  (void)qos;
  if (!topic || !payload || !mqttCanPublish()) return -1;
#if defined(DMK_BOARD_ESP8266)
  unsigned int n = len > 0 ? (unsigned int)len : (unsigned int)strlen(payload);
  return mqttClient8266.publish(topic, reinterpret_cast<const uint8_t*>(payload), n, retain) ? 1 : -1;
#else
  return esp_mqtt_client_publish(mqttClientNative, topic, payload, len, qos, retain ? 1 : 0);
#endif
}

// ---------------------------------------------------------------------------
// MQTT NATIVO ESP-IDF / ESP-MQTT
// ---------------------------------------------------------------------------
void buildMqttTopics()
{
  std::string base(cfgTopicBase.c_str());
  topicTelemetry = base + "/telemetry";
  topicPumpSet = base + "/pump/set";
  topicPumpState = base + "/pump/state";
  topicAvailability = base + "/availability";
  topicLightSet = base + "/light/set";
  topicLightState = base + "/light/state";

  // Historico remoto.
  topicPumpHistoryGet = base + "/pump/history/get";
  topicPumpHistoryResponse = base + "/pump/history/response";
  topicPumpHistoryClear = base + "/pump/history/clear";

  topicScheduleSet = base + "/schedule/set";
  topicScheduleGet = base + "/schedule/get";
  topicScheduleState = base + "/schedule/state";
  topicScheduleEvent = base + "/schedule/event";
}

void publishPumpState()
{
  if (!mqttCanPublish())
    return;

  dmkMqttPublish(topicPumpState.c_str(), pumpOn ? "ON" : "OFF", 0, 1, true);
}

void publishLightState()
{
  if (!mqttCanPublish())
    return;
  dmkMqttPublish(topicLightState.c_str(), lightOn ? "ON" : "OFF", 0, 1, true);
}

void publishTelemetry()
{
  if (!mqttCanPublish() || !measurementReady)
  {
    return;
  }

  char payload[1450];
  int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -127;

  snprintf(
      payload,
      sizeof(payload),
      "{"
      "\"vR\":%.2f,"
      "\"vS\":%.2f,"
      "\"vT\":%.2f,"
      "\"adcVrmsR\":%.4f,"
      "\"adcVrmsS\":%.4f,"
      "\"adcVrmsT\":%.4f,"
      "\"current_a\":%.3f,"
      "\"temperature_c\":%.2f,"
      "\"flow_l_min\":%.3f,"
      "\"total_l\":%.3f,"
      "\"rain_pct\":%.1f,"
      "\"alarm\":%s,"
      "\"light\":%s,"
      "\"firmware\":\"%s\","
      "\"pump\":%s,"
      "\"protection_enabled\":%s,"
      "\"protection_ok\":%s,"
      "\"protection_reason\":\"%s\","
      "\"clock_valid\":%s,"
      "\"clock_local\":\"%s\","
      "\"clock_source\":\"%s\","
      "\"vmin\":%.1f,"
      "\"vmax\":%.1f,"
      "\"rssi\":%d,"
      "\"uptime_s\":%lu"
      "}",
      voltageR,
      voltageS,
      voltageT,
      adcVrmsR,
      adcVrmsS,
      adcVrmsT,
      currentA,
      temperatureC,
      flowLitersMinute,
      totalLiters,
      rainPercent,
      alarmActive ? "true" : "false",
      lightOn ? "true" : "false",
      FIRMWARE_VERSION,
      pumpOn ? "true" : "false",
      cfgProtectionEnabled ? "true" : "false",
      protectionOk ? "true" : "false",
      protectionReason.c_str(),
      DmkClock::isValid() ? "true" : "false",
      DmkClock::localDateTime().c_str(),
      DmkClock::source(),
      cfgVoltageMin,
      cfgVoltageMax,
      rssi,
      millis() / 1000UL);

  dmkMqttPublish(topicTelemetry.c_str(), payload, 0, 0, false);
}

// ---------------------------------------------------------------------------
// HISTORICO DA BOMBA VIA MQTT
// ---------------------------------------------------------------------------
// O navegador publica em:
//   <topico-base>/pump/history/get
//   <topico-base>/pump/history/clear
//
// O ESP responde em:
//   <topico-base>/pump/history/response
//
// O historico e enviado registro por registro para nao criar payload MQTT
// gigante nem pressionar a RAM do ESP32.
// ---------------------------------------------------------------------------

String mqttDataToString(const char *data, int len)
{
  String out;
  if (data == nullptr || len <= 0)
    return out;

  out.reserve((size_t)len + 1);
  for (int i = 0; i < len; ++i)
    out += data[i];

  out.trim();
  return out;
}

String mqttJsonStringField(const String &json, const char *key, const String &fallback = "")
{
  String token = "\"" + String(key) + "\"";
  int p = json.indexOf(token);
  if (p < 0)
    return fallback;

  p = json.indexOf(':', p + token.length());
  if (p < 0)
    return fallback;

  ++p;
  while (p < (int)json.length() && isspace((unsigned char)json[p]))
    ++p;

  if (p >= (int)json.length() || json[p] != '"')
    return fallback;

  ++p;
  String value;
  bool escaped = false;

  for (; p < (int)json.length(); ++p)
  {
    char c = json[p];

    if (escaped)
    {
      value += c;
      escaped = false;
      continue;
    }

    if (c == '\\')
    {
      escaped = true;
      continue;
    }

    if (c == '"')
      return value;

    value += c;
  }

  return fallback;
}

int mqttJsonIntField(const String &json, const char *key, int fallback)
{
  String token = "\"" + String(key) + "\"";
  int p = json.indexOf(token);
  if (p < 0)
    return fallback;

  p = json.indexOf(':', p + token.length());
  if (p < 0)
    return fallback;

  ++p;
  while (p < (int)json.length() && isspace((unsigned char)json[p]))
    ++p;

  bool negative = false;
  if (p < (int)json.length() && json[p] == '-')
  {
    negative = true;
    ++p;
  }

  long value = 0;
  bool foundDigit = false;

  while (p < (int)json.length() && isdigit((unsigned char)json[p]))
  {
    foundDigit = true;
    value = value * 10L + (json[p] - '0');
    ++p;
  }

  if (!foundDigit)
    return fallback;

  if (negative)
    value = -value;

  return (int)value;
}

void publishHistoryMqttMessage(const String &payload)
{
  if (!mqttCanPublish())
    return;

  // QoS 0: respostas pequenas, sem acumular dezenas de mensagens no outbox.
  dmkMqttPublish(topicPumpHistoryResponse.c_str(), payload.c_str(), (int)payload.length(), 0, false);
}

struct MqttHistoryPublishContext
{
  String requestId;
};

bool publishOneHistoryLine(const String &line, void *userData)
{
  MqttHistoryPublishContext *ctx =
      static_cast<MqttHistoryPublishContext *>(userData);

  if (ctx == nullptr || !mqttConnected)
    return false;

  String payload;
  payload.reserve(line.length() + ctx->requestId.length() + 80);
  payload += "{\"type\":\"event\",\"request_id\":\"";
  payload += jsonEscape(ctx->requestId);
  payload += "\",\"event\":";
  payload += line;
  payload += "}";

  publishHistoryMqttMessage(payload);

  // Entrega tempo para a tarefa MQTT escoar a fila.
  delay(1);
  return true;
}

void processHistoryGetMqtt(const char *data, int len)
{
  String request = mqttDataToString(data, len);

  String requestId = mqttJsonStringField(request, "request_id", "");
  if (requestId.length() == 0)
    requestId = "esp-" + String(millis());

  int limit = mqttJsonIntField(request, "limit", 50);
  if (limit < 1)
    limit = 1;
  if (limit > 60)
    limit = 60;

  uint32_t total = PumpHistory::count();
  uint16_t expected = (uint16_t)((total < (uint32_t)limit) ? total : (uint32_t)limit);

  String beginPayload;
  beginPayload.reserve(150);
  beginPayload += "{\"type\":\"begin\",\"request_id\":\"";
  beginPayload += jsonEscape(requestId);
  beginPayload += "\",\"total\":";
  beginPayload += String(total);
  beginPayload += ",\"count\":";
  beginPayload += String(expected);
  beginPayload += "}";

  publishHistoryMqttMessage(beginPayload);

  MqttHistoryPublishContext ctx;
  ctx.requestId = requestId;

  uint32_t totalRows = 0;
  uint16_t sent = PumpHistory::forEachRecent(
      (uint16_t)limit,
      publishOneHistoryLine,
      &ctx,
      &totalRows);

  String endPayload;
  endPayload.reserve(140);
  endPayload += "{\"type\":\"end\",\"request_id\":\"";
  endPayload += jsonEscape(requestId);
  endPayload += "\",\"sent\":";
  endPayload += String(sent);
  endPayload += ",\"total\":";
  endPayload += String(totalRows);
  endPayload += "}";

  publishHistoryMqttMessage(endPayload);

  Serial.printf("[PUMP-HISTORY][MQTT] request=%s | enviados=%u | total=%lu\n",
                requestId.c_str(),
                sent,
                (unsigned long)totalRows);
}

void processHistoryClearMqtt(const char *data, int len)
{
  String request = mqttDataToString(data, len);

  String requestId = mqttJsonStringField(request, "request_id", "");
  if (requestId.length() == 0)
    requestId = "esp-" + String(millis());

  String confirm = mqttJsonStringField(request, "confirm", "");
  confirm.trim();
  confirm.toUpperCase();

  // Tambem aceita payload puro "APAGAR".
  String rawUpper = request;
  rawUpper.trim();
  rawUpper.toUpperCase();

  bool authorized = (confirm == "APAGAR" || rawUpper == "APAGAR");
  bool ok = false;

  if (authorized)
    ok = PumpHistory::clear();

  String response;
  response.reserve(190);
  response += "{\"type\":\"clear\",\"request_id\":\"";
  response += jsonEscape(requestId);
  response += "\",\"ok\":";
  response += ok ? "true" : "false";
  response += ",\"message\":\"";

  if (!authorized)
    response += "Confirmacao invalida";
  else if (ok)
    response += "Historico apagado";
  else
    response += "Falha ao apagar historico";

  response += "\"}";

  publishHistoryMqttMessage(response);
}

// ---------------------------------------------------------------------------
// AUTOMACOES VIA MQTT
// ---------------------------------------------------------------------------
void publishAutomationStateMqtt()
{
  if (!mqttCanPublish())
    return;

  String payload = AutomationManager::toJson();

  dmkMqttPublish(topicScheduleState.c_str(), payload.c_str(), (int)payload.length(), 0, true);

  lastScheduleStatePublishMs = millis();
}

void publishAutomationEventMqtt(uint32_t automationId,
                                const char *target,
                                const char *state,
                                const char *status,
                                const char *reason)
{
  if (!mqttCanPublish())
    return;

  String payload;
  payload.reserve(260);
  payload += "{\"id\":";
  payload += String(automationId);
  payload += ",\"target\":\"";
  payload += jsonEscape(String(target ? target : ""));
  payload += "\",\"state\":\"";
  payload += jsonEscape(String(state ? state : ""));
  payload += "\",\"status\":\"";
  payload += jsonEscape(String(status ? status : ""));
  payload += "\",\"reason\":\"";
  payload += jsonEscape(String(reason ? reason : ""));
  payload += "\",\"time_local\":\"";
  payload += jsonEscape(DmkClock::localDateTime());
  payload += "\"}";

  dmkMqttPublish(topicScheduleEvent.c_str(), payload.c_str(), (int)payload.length(), 0, false);
}

void processScheduleGetMqtt(const char *data, int len)
{
  (void)data;
  (void)len;
  publishAutomationStateMqtt();
}

void processScheduleSetMqtt(const char *data, int len)
{
  String request = mqttDataToString(data, len);
  if (request.length() == 0)
    return;

  uint32_t id = (uint32_t)mqttJsonIntField(request, "id", 0);
  int enabledValue = mqttJsonIntField(request, "enabled", 1);
  bool enabled = enabledValue != 0;

  int deviceLegacy = mqttJsonIntField(request, "device", -1);
  int actionLegacy = mqttJsonIntField(request, "action", -1);

  String targetText = mqttJsonStringField(request, "target", "");
  targetText.trim();
  targetText.toUpperCase();

  uint8_t target = 0;
  if (targetText == "PUMP" || targetText == "BOMBA")
    target = AutomationManager::TARGET_PUMP;
  else if (targetText == "LIGHT" || targetText == "LUZ")
    target = AutomationManager::TARGET_LIGHT;
  else if (deviceLegacy == 0)
    target = AutomationManager::TARGET_PUMP;
  else if (deviceLegacy == 1)
    target = AutomationManager::TARGET_LIGHT;

  String stateText = mqttJsonStringField(request, "state", "");
  stateText.trim();
  stateText.toUpperCase();

  bool turnOn = false;
  bool stateValid = false;

  if (stateText == "ON")
  {
    turnOn = true;
    stateValid = true;
  }
  else if (stateText == "OFF")
  {
    turnOn = false;
    stateValid = true;
  }
  else if (actionLegacy == 0 || actionLegacy == 1)
  {
    turnOn = actionLegacy == 1;
    stateValid = true;
  }

  int hour = mqttJsonIntField(request, "hour", -1);
  int minute = mqttJsonIntField(request, "minute", -1);
  int daysMask = mqttJsonIntField(
      request,
      "days_mask",
      mqttJsonIntField(request, "days", 0));

  String name = mqttJsonStringField(request, "name", "");
  name.trim();
  if (name.length() == 0)
  {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%s %02d:%02d",
             target == AutomationManager::TARGET_LIGHT ? "Luz" : "Bomba",
             hour < 0 ? 0 : hour,
             minute < 0 ? 0 : minute);
    name = tmp;
  }

  bool ok = false;
  uint32_t savedId = id;

  if (id > 0 && !enabled)
  {
    ok = AutomationManager::setEnabled(id, false);
  }
  else if (target != 0 &&
           stateValid &&
           hour >= 0 && hour <= 23 &&
           minute >= 0 && minute <= 59 &&
           (daysMask & AutomationManager::EVERY_DAY) != 0)
  {
    ok = AutomationManager::save(
        id,
        name.c_str(),
        target,
        turnOn,
        (uint8_t)hour,
        (uint8_t)minute,
        (uint8_t)daysMask,
        enabled,
        &savedId);
  }

  if (!ok)
  {
    Serial.printf("[AUTO][MQTT] Falha ao salvar/desativar. payload=%s\n",
                  request.c_str());
  }
  else
  {
    Serial.printf("[AUTO][MQTT] Agenda atualizada. id=%lu\n",
                  (unsigned long)savedId);
  }

  publishAutomationStateMqtt();
}

// ============================================================
// COMANDO DA BOMBA RECEBIDO PELO MQTT
// Aceita:
//   ON
//   OFF
// e também JSON com localização do aplicativo.
// ============================================================
void processMqttCommand(const char *data, int len)
{

  if (!PumpHistory::queueCommandFromMqtt(data, len))
  {
    Serial.println("[BOMBA] Comando MQTT invalido.");
    return;
  }

  Serial.println("[BOMBA] Comando MQTT recebido.");
}

void processLightCommand(const char *data, int len)
{
  if (data == nullptr || len <= 0)
    return;
  std::string payload(data, len);
  if (payload == "ON" || payload == "on" || payload == "1" || payload == "true")
  {
    applyLightOutput(true);
    publishLightState();
  }
  else if (payload == "OFF" || payload == "off" || payload == "0" || payload == "false")
  {
    applyLightOutput(false);
    publishLightState();
  }
}

#if defined(DMK_BOARD_ESP8266)
void mqttCallback8266(char *topic, byte *payload, unsigned int length)
{
  std::string t(topic ? topic : "");
  const char *data = reinterpret_cast<const char *>(payload);
  int len = (int)length;
  // Seguranca: o PubSubClient nao informa "retain". Comandos que chegam logo
  // apos assinar o topico sao os retidos no broker e sao ignorados.
  bool justSubscribed = (uint32_t)(millis() - mqttSubscribedAt) < 1500UL;
  if ((t == topicPumpSet || t == topicLightSet) && justSubscribed) {
    Serial.println("[MQTT][SEGURANCA] Comando retido ignorado apos conectar.");
    return;
  }
  if (t == topicPumpSet) processMqttCommand(data, len);
  else if (t == topicLightSet) processLightCommand(data, len);
  else if (t == topicPumpHistoryGet) processHistoryGetMqtt(data, len);
  else if (t == topicPumpHistoryClear) processHistoryClearMqtt(data, len);
  else if (t == topicScheduleSet) processScheduleSetMqtt(data, len);
  else if (t == topicScheduleGet) processScheduleGetMqtt(data, len);
}
#else
void mqttEventHandler(void *handler_args,
                      esp_event_base_t base,
                      int32_t event_id,
                      void *event_data)
{
  (void)handler_args;
  (void)base;

  esp_mqtt_event_handle_t event =
      static_cast<esp_mqtt_event_handle_t>(event_data);

  switch ((esp_mqtt_event_id_t)event_id)
  {

  case MQTT_EVENT_CONNECTED:
    mqttConnected = true;
    mqttJustConnected = true;

    Serial.println("[MQTT] Conectado ao broker.");
    Serial.printf("[MQTT] Assinando comando: %s\n",
                  topicPumpSet.c_str());

    esp_mqtt_client_subscribe(
        event->client,
        topicPumpSet.c_str(),
        1);
    esp_mqtt_client_subscribe(event->client, topicLightSet.c_str(), 1);

    // Historico remoto.
    esp_mqtt_client_subscribe(event->client, topicPumpHistoryGet.c_str(), 0);
    esp_mqtt_client_subscribe(event->client, topicPumpHistoryClear.c_str(), 0);

    esp_mqtt_client_subscribe(event->client, topicScheduleSet.c_str(), 0);
    esp_mqtt_client_subscribe(event->client, topicScheduleGet.c_str(), 0);

    // Publica a agenda atual com retain para o aplicativo.
    publishAutomationStateMqtt();
    break;

  case MQTT_EVENT_DISCONNECTED:
    mqttConnected = false;
    Serial.println("[MQTT] Desconectado. Reconexao automatica ativa.");
    break;

  case MQTT_EVENT_SUBSCRIBED:
    Serial.printf("[MQTT] SUBACK recebido. msg_id=%d\n", event->msg_id);
    break;

  case MQTT_EVENT_DATA:
  {
    std::string topic(event->topic, event->topic_len);

    if (topic == topicPumpSet)
    {
      // Seguranca: um comando ON/OFF retido no broker nao pode restaurar
      // automaticamente um estado antigo logo apos reinicio/reconexao.
      if (event->retain)
      {
        Serial.println("[MQTT][SEGURANCA] Comando RETIDO da bomba ignorado.");
      }
      else
      {
        processMqttCommand(event->data, event->data_len);
      }
    }
    else if (topic == topicLightSet)
    {
      if (event->retain)
      {
        Serial.println("[MQTT][SEGURANCA] Comando RETIDO da luz ignorado.");
      }
      else
      {
        processLightCommand(event->data, event->data_len);
      }
    }
    else if (topic == topicPumpHistoryGet)
    {
      processHistoryGetMqtt(event->data, event->data_len);
    }
    else if (topic == topicPumpHistoryClear)
    {
      processHistoryClearMqtt(event->data, event->data_len);
    }
    else if (topic == topicScheduleSet)
    {
      processScheduleSetMqtt(event->data, event->data_len);
    }
    else if (topic == topicScheduleGet)
    {
      processScheduleGetMqtt(event->data, event->data_len);
    }
    break;
  }

  case MQTT_EVENT_ERROR:
    mqttConnected = false;
    Serial.println("[MQTT] Evento de erro.");

    if (event->error_handle != nullptr)
    {
      Serial.printf("[MQTT] Tipo erro=%d\n",
                    event->error_handle->error_type);
    }
    break;

  default:
    break;
  }
}

#endif

void setupMqtt()
{
  Serial.println("[BOOT] 6/6 - Configurando MQTT nativo...");

  if (cfgMqttHost.length() == 0)
  {
    cfgMqttHost = "broker.hivemq.com";
  }

  buildMqttTopics();

  // O ESP32 usa MQTT TCP; o painel no navegador continua usando WebSocket.
  Serial.printf("[MQTT] Host: %s\n", cfgMqttHost.c_str());
  Serial.printf("[MQTT] Porta: %u\n", cfgMqttPort);
  Serial.println("[MQTT] Transporte: TCP");
  Serial.printf("[MQTT] Topico base: %s\n", cfgTopicBase.c_str());

#if defined(DMK_BOARD_ESP8266)
  mqttClient8266.setServer(cfgMqttHost.c_str(), cfgMqttPort);
  mqttClient8266.setCallback(mqttCallback8266);
  mqttClient8266.setBufferSize(4096);
  mqttClient8266.setKeepAlive(30);
#endif
  mqttConfigured = true;

  Serial.println("[MQTT] Configurado. Aguardando Wi-Fi obter IP.");
}

void startMqttIfReady()
{
  if (!mqttConfigured || WiFi.status() != WL_CONNECTED) return;

#if defined(DMK_BOARD_ESP8266)
  if (mqttClient8266.connected()) {
    mqttConnected = true;
    mqttClient8266.loop();
    return;
  }

  mqttConnected = false;
  uint32_t now = millis();
  if ((uint32_t)(now - lastMqttReconnectAt) < 5000UL) return;
  lastMqttReconnectAt = now;

  String clientId = cfgDeviceName + "-" + chipSuffix();
  bool ok;
  if (cfgMqttUser.length() > 0) {
    ok = mqttClient8266.connect(clientId.c_str(), cfgMqttUser.c_str(), cfgMqttPass.c_str(),
                                topicAvailability.c_str(), 1, true, "offline");
  } else {
    ok = mqttClient8266.connect(clientId.c_str(), nullptr, nullptr,
                                topicAvailability.c_str(), 1, true, "offline");
  }

  if (!ok) {
    Serial.printf("[MQTT] ESP8266 falha conexao, estado=%d\n", mqttClient8266.state());
    return;
  }

  mqttStarted = true;
  mqttConnected = true;
  mqttJustConnected = true;
  mqttClient8266.subscribe(topicPumpSet.c_str(), 1);
  mqttClient8266.subscribe(topicLightSet.c_str(), 1);
  mqttClient8266.subscribe(topicPumpHistoryGet.c_str(), 0);
  mqttClient8266.subscribe(topicPumpHistoryClear.c_str(), 0);
  mqttClient8266.subscribe(topicScheduleSet.c_str(), 0);
  mqttClient8266.subscribe(topicScheduleGet.c_str(), 0);
  mqttSubscribedAt = millis();
  Serial.println("[MQTT] ESP8266 conectado ao broker.");
#else
  if (mqttStarted) return;

  Serial.println("[MQTT] Wi-Fi pronto. Criando cliente MQTT nativo...");
  static String nativeClientId;
  nativeClientId = cfgDeviceName + "-" + chipSuffix();
  static String mqttUri;
  mqttUri = "mqtt://" + cfgMqttHost + ":" + String(cfgMqttPort);
  Serial.printf("[MQTT] URI: %s\n", mqttUri.c_str());

  esp_mqtt_client_config_t mqttCfg = {};
#if ESP_IDF_VERSION_MAJOR >= 5
  mqttCfg.broker.address.uri = mqttUri.c_str();
  mqttCfg.credentials.client_id = nativeClientId.c_str();
  if (cfgMqttUser.length() > 0) mqttCfg.credentials.username = cfgMqttUser.c_str();
  if (cfgMqttPass.length() > 0) mqttCfg.credentials.authentication.password = cfgMqttPass.c_str();
  mqttCfg.session.keepalive = 30;
  mqttCfg.session.last_will.topic = topicAvailability.c_str();
  mqttCfg.session.last_will.msg = "offline";
  mqttCfg.session.last_will.msg_len = 7;
  mqttCfg.session.last_will.qos = 1;
  mqttCfg.session.last_will.retain = true;
  mqttCfg.buffer.size = 4096;
  mqttCfg.buffer.out_size = 4096;
#else
  mqttCfg.uri = mqttUri.c_str();
  mqttCfg.client_id = nativeClientId.c_str();
  if (cfgMqttUser.length() > 0) mqttCfg.username = cfgMqttUser.c_str();
  if (cfgMqttPass.length() > 0) mqttCfg.password = cfgMqttPass.c_str();
  mqttCfg.keepalive = 30;
  mqttCfg.lwt_topic = topicAvailability.c_str();
  mqttCfg.lwt_msg = "offline";
  mqttCfg.lwt_msg_len = 7;
  mqttCfg.lwt_qos = 1;
  mqttCfg.lwt_retain = true;
  mqttCfg.buffer_size = 4096;
#endif

  mqttClientNative = esp_mqtt_client_init(&mqttCfg);
  if (!mqttClientNative) { Serial.println("[MQTT] ERRO: init retornou NULL."); return; }
  esp_err_t regResult = esp_mqtt_client_register_event(mqttClientNative, MQTT_EVENT_ANY, mqttEventHandler, nullptr);
  if (regResult != ESP_OK) { esp_mqtt_client_destroy(mqttClientNative); mqttClientNative=nullptr; return; }
  esp_err_t startResult = esp_mqtt_client_start(mqttClientNative);
  if (startResult == ESP_OK) { mqttStarted=true; Serial.printf("[MQTT] Cliente iniciado. Client ID: %s\n", nativeClientId.c_str()); }
  else { esp_mqtt_client_destroy(mqttClientNative); mqttClientNative=nullptr; }
#endif
}


// ---------------------------------------------------------------------------
// ENLACE COM OS CONTROLES: ESP-NOW (1) + REDE LOCAL UDP (2)
// MQTT (3) continua nos topicos de texto acima.
// ---------------------------------------------------------------------------
static uint16_t clampU16(float v)
{
  if (!isfinite(v) || v < 0.0f) return 0;
  if (v > 65535.0f) return 65535;
  return (uint16_t)(v + 0.5f);
}

static uint32_t clampU32(float v)
{
  if (!isfinite(v) || v < 0.0f) return 0;
  if (v > 4.0e9f) return 4000000000UL;
  return (uint32_t)(v + 0.5f);
}

void buildLinkStatus(DmkProto::Status &st)
{
  using namespace DmkProto;
  memset(&st, 0, sizeof(st));
  st.session = linkSession;
  st.flags = (pumpOn ? SF_PUMP : 0) | (lightOn ? SF_LIGHT : 0) | (alarmActive ? SF_ALARM : 0) |
             (protectionOk ? SF_PROT_OK : 0) | (cfgProtectionEnabled ? SF_PROT_ON : 0) |
             (WiFi.status() == WL_CONNECTED ? SF_WIFI : 0) | (mqttConnected ? SF_MQTT : 0) |
             (DmkClock::isValid() ? SF_CLOCK : 0);
  st.rssi = (int8_t)(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -127);
  st.channel = DmkLink::channel();
  IPAddress ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP() : IPAddress(0, 0, 0, 0);
  for (int i = 0; i < 4; i++) st.ip[i] = ip[i];
  st.vR10 = clampU16(voltageR * 10.0f);
  st.vS10 = clampU16(voltageS * 10.0f);
  st.vT10 = clampU16(voltageT * 10.0f);
  st.adcR = clampU16(adcVrmsR * 10000.0f);
  st.adcS = clampU16(adcVrmsS * 10000.0f);
  st.adcT = clampU16(adcVrmsT * 10000.0f);
  st.vmin10 = clampU16(cfgVoltageMin * 10.0f);
  st.vmax10 = clampU16(cfgVoltageMax * 10.0f);
  st.current_mA = clampU32(currentA * 1000.0f);
  st.temp10 = (int16_t)constrain(temperatureC * 10.0f, -32000.0f, 32000.0f);
  st.flow_mLmin = clampU32(flowLitersMinute * 1000.0f);
  st.total_cL = clampU32(totalLiters * 100.0f);
  st.rain = (uint8_t)constrain(rainPercent, 0.0f, 100.0f);
  st.otaBusy = (otaInProgress || MqttOta::busy()) ? 1 : 0;
  st.uptime = millis() / 1000UL;
  strncpy(st.fw, FIRMWARE_VERSION, sizeof(st.fw) - 1);
  strncpy(st.reason, protectionReason.c_str(), sizeof(st.reason) - 1);
  strncpy(st.name, cfgDeviceName.c_str(), sizeof(st.name) - 1);
}

void linkReply(const DmkLink::Rx &rx, uint8_t type, const void *payload, uint8_t len)
{
  if (rx.via == DmkLink::VIA_ESPNOW)
    DmkLink::send(type, payload, len, DmkLink::VIA_ESPNOW, rx.mac);
  else if (rx.via == DmkLink::VIA_LAN)
    DmkLink::send(type, payload, len, DmkLink::VIA_LAN, nullptr, rx.ip, rx.port);
}

void linkSendStatusTo(const DmkLink::Rx &rx)
{
  DmkProto::Status st;
  buildLinkStatus(st);
  linkReply(rx, DmkProto::T_STATUS, &st, sizeof(st));
}

void linkOnFrame(const DmkLink::Rx &rx)
{
  using namespace DmkProto;
  if (rx.type == T_PING)
  {
    linkSendStatusTo(rx);
    return;
  }
  if (rx.type != T_CMD || rx.len < sizeof(Cmd))
    return;

  Cmd c;
  memcpy(&c, rx.data, sizeof(c));
  Ack a;
  a.seq = c.seq;
  a.target = c.target;
  a.state = c.state;
  a.result = ACK_OK;
  const char *origin = rx.via == DmkLink::VIA_ESPNOW ? "CONTROLE_ESPNOW" : "CONTROLE_REDE";

  if (c.session != linkSession && c.session != linkPrevSession)
    a.result = ACK_BAD_SESSION; // comando antigo/repetido: recusado
  else if (otaInProgress || MqttOta::busy())
    a.result = ACK_BUSY;
  else if (c.target == TARGET_PUMP)
  {
    if (!PumpHistory::queueCommand(c.state != 0, origin, false, 0.0, 0.0, 0.0f, nullptr))
      a.result = ACK_BUSY;
  }
  else if (c.target == TARGET_LIGHT)
  {
    applyLightOutput(c.state != 0);
    publishLightState();
  }
  else
    a.result = ACK_INVALID;

  Serial.printf("[LINK] CMD %s=%s via %s -> %s\n",
                c.target == TARGET_PUMP ? "BOMBA" : "LUZ", c.state ? "ON" : "OFF",
                DmkLink::viaName(rx.via),
                a.result == ACK_OK ? "OK" : a.result == ACK_BAD_SESSION ? "SESSAO" : a.result == ACK_BUSY ? "OCUPADO" : "INVALIDO");
  linkReply(rx, T_ACK, &a, sizeof(a));
  linkSendStatusTo(rx);
  linkStatusDirty = true;
}

String linkShareConfig()
{
  // Copia Wi-Fi + MQTT para o controle (cifrado com a chave do pareamento).
  if (!cfgPairShare || cfgWifiSsid.length() == 0)
    return "";
  JsonDocument doc;
  doc["s"] = cfgWifiSsid;
  doc["p"] = cfgWifiPass;
  doc["h"] = cfgMqttHost;
  doc["o"] = cfgMqttPort;
  doc["u"] = cfgMqttUser;
  doc["w"] = cfgMqttPass;
  doc["t"] = cfgTopicBase;
  String out;
  serializeJson(doc, out);
  return out;
}

void linkOnPaired(const uint8_t mac[6], const char *name)
{
  char m[18];
  DmkLink::macToText(mac, m);
  Serial.printf("[PAREAR] Controle registrado: %s (%s)\n", name && *name ? name : "sem nome", m);
  linkStatusDirty = true;
}

void newLinkSession()
{
  linkPrevSession = linkSession;
  DmkCrypto::randomBytes((uint8_t *)&linkSession, sizeof(linkSession));
  if (linkSession == 0)
    linkSession = 1;
  linkSessionAt = millis();
  linkStatusDirty = true;
}

void maintainLink()
{
  DmkLink::loop();
  if ((uint32_t)(millis() - linkSessionAt) >= DMK_SESSION_ROTATE_MS)
    newLinkSession();

  uint8_t signature = (pumpOn ? 1 : 0) | (lightOn ? 2 : 0) | (alarmActive ? 4 : 0) |
                      (protectionOk ? 8 : 0) | ((otaInProgress || MqttOta::busy()) ? 16 : 0) |
                      (mqttConnected ? 32 : 0);
  if (signature != lastLinkSignature)
  {
    lastLinkSignature = signature;
    linkStatusDirty = true;
  }

  bool anyController = DmkLink::peerCount() > 0 || DmkLink::pairIsOpen();
  if (!anyController || !DmkLink::hasKey())
  {
    linkStatusDirty = false;
    return;
  }

  uint32_t now = millis();
  bool sendEspNow = linkStatusDirty || (uint32_t)(now - lastEspNowStatusMs) >= DMK_STATUS_ESPNOW_MS;
  bool sendLan = DmkLink::udpReady() && (linkStatusDirty || (uint32_t)(now - lastLanStatusMs) >= DMK_STATUS_LAN_MS);
  if (!sendEspNow && !sendLan)
    return;

  DmkProto::Status st;
  buildLinkStatus(st);
  if (sendEspNow)
  {
    DmkLink::send(DmkProto::T_STATUS, &st, sizeof(st), DmkLink::VIA_ESPNOW, nullptr);
    lastEspNowStatusMs = now;
  }
  if (sendLan)
  {
    DmkLink::send(DmkProto::T_STATUS, &st, sizeof(st), DmkLink::VIA_LAN, nullptr);
    lastLanStatusMs = now;
  }
  linkStatusDirty = false;
}

String buildLinkJson()
{
  String json;
  json.reserve(700);
  char m[18];
  uint8_t own[6];
  WiFi.macAddress(own);
  DmkLink::macToText(own, m);
  json += "{\"espnow\":" + String(DmkLink::espNowReady() ? "true" : "false");
  json += ",\"udp\":" + String(DmkLink::udpReady() ? "true" : "false");
  json += ",\"mac\":\"" + String(m) + "\"";
  json += ",\"channel\":" + String(DmkLink::channel());
  json += ",\"net_id\":\"" + String(DmkLink::netId(), HEX) + "\"";
  json += ",\"pairing\":" + String(DmkLink::pairIsOpen() ? "true" : "false");
  json += ",\"pair_remaining_s\":" + String(DmkLink::pairRemainingS());
  json += ",\"share_config\":" + String(cfgPairShare ? "true" : "false");
  json += ",\"frames_ok\":" + String(DmkLink::framesOk());
  json += ",\"frames_rejected\":" + String(DmkLink::framesRejected());
  json += ",\"controls\":[";
  for (uint8_t i = 0; i < DmkLink::peerCount(); i++)
  {
    uint8_t mac[6];
    String name;
    uint32_t seen = 0;
    if (!DmkLink::peerAt(i, mac, name, seen))
      continue;
    DmkLink::macToText(mac, m);
    if (i)
      json += ",";
    json += "{\"mac\":\"" + String(m) + "\",\"name\":\"" + jsonEscape(name) + "\",\"last_seen_s\":";
    json += seen == 0xFFFFFFFF ? String("null") : String(seen);
    json += "}";
  }
  json += "]}";
  return json;
}

void updateStatusLed()
{
  using namespace DmkStatusLed;
  if (otaInProgress || MqttOta::busy())
    set(LED_OTA);
  else if (DmkLink::pairIsOpen())
    set(LED_FAST);
  else if (configApActive)
    set(LED_AP);
  else if (WiFi.status() == WL_CONNECTED && mqttConnected)
    set(LED_ON);
  else if (WiFi.status() == WL_CONNECTED)
    set(LED_BLIP1);
  else
    set(LED_SLOW);
  DmkStatusLed::loop();
}

// ---------------------------------------------------------------------------
// MEDICAO NAO BLOQUEANTE
// ---------------------------------------------------------------------------
uint32_t readMilliVoltsSafe(int pin)
{
  if (pin < 0) return 0;
#if defined(DMK_BOARD_ESP8266)
  // ESP8266 possui um unico ADC. No ESP-12E/ESP8266MOD ele e lido por A0.
  // A conversao abaixo representa aproximadamente 0..1000 mV no ADC nativo.
  return (uint32_t)((uint32_t)analogRead(pin) * 1000UL / 1023UL);
#else
  return analogReadMilliVolts(pin);
#endif
}

int analogReadSafe(int pin)
{
  if (pin < 0) return 0;
  return analogRead(pin);
}

void startMeasurementWindow()
{
  accR.reset();
  accS.reset();
  accT.reset();
  accCurrent.reset();

  samplingActive = true;
  sampleWindowStartedUs = micros();
  lastSampleUs = 0;
}

void finishMeasurementWindow()
{
  samplingActive = false;

  // analogReadMilliVolts devolve milivolts calibrados no pino ADC.
  adcVrmsR = (float)(accR.rmsAc() / 1000.0);
  adcVrmsS = (float)(accS.rmsAc() / 1000.0);
  adcVrmsT = (float)(accT.rmsAc() / 1000.0);
  currentA = (float)(accCurrent.rmsAc() / 1000.0) * cfgCurrentAmpsPerVolt;

  voltageR = adcVrmsR * cfgCalR;
  voltageS = adcVrmsS * cfgCalS;
  voltageT = adcVrmsT * cfgCalT;

  uint32_t tempMv = readMilliVoltsSafe(PIN_TEMPERATURE);
  temperatureC = (float)tempMv / 10.0f; // LM35 = 10 mV/°C
  int rainRaw = analogReadSafe(PIN_RAIN);
  int rainSpan = (int)cfgRainDryRaw - (int)cfgRainWetRaw;
  if (rainSpan != 0)
  {
    rainPercent = 100.0f * ((float)cfgRainDryRaw - (float)rainRaw) / (float)rainSpan;
    rainPercent = constrain(rainPercent, 0.0f, 100.0f);
  }
  alarmActive = (PIN_ALARM >= 0) ?
      (cfgAlarmActiveLow ? (digitalRead(PIN_ALARM) == LOW) : (digitalRead(PIN_ALARM) == HIGH)) : false;

  if (voltageR < NOISE_FLOOR_V)
    voltageR = 0.0f;
  if (voltageS < NOISE_FLOOR_V)
    voltageS = 0.0f;
  if (voltageT < NOISE_FLOOR_V)
    voltageT = 0.0f;

  measurementReady = true;
  evaluateProtection();

  // Falha de tensao com protecao ativa => desliga a bomba.
  if (cfgProtectionEnabled && !protectionOk && pumpOn)
  {
    applyPumpOutput(false);
    publishPumpState();

    PumpHistory::logAutomatic(false,
                              "PROTECTION",
                              "AUTOMATICO",
                              protectionReason.c_str());

    Serial.printf("[PROTECAO] Bomba desligada: %s\n",
                  protectionReason.c_str());
  }

  Serial.printf(
      "[SENSORES] R=%.1f V | S=%.1f V | T=%.1f V | I=%.2f A | Temp=%.1f C\n",
      voltageR, voltageS, voltageT,
      currentA, temperatureC);

  publishTelemetry();
}

void updateMeasurements()
{
  uint32_t nowMs = millis();

  if (!samplingActive)
  {
    if (!measurementReady ||
        (uint32_t)(nowMs - lastMeasureCycleMs) >= MEASURE_INTERVAL_MS)
    {
      lastMeasureCycleMs = nowMs;
      startMeasurementWindow();
    }
    return;
  }

  uint32_t nowUs = micros();

  if ((uint32_t)(nowUs - sampleWindowStartedUs) >= SAMPLE_WINDOW_US)
  {
    finishMeasurementWindow();
    return;
  }

  if (lastSampleUs == 0 ||
      (uint32_t)(nowUs - lastSampleUs) >= SAMPLE_PERIOD_US)
  {
    lastSampleUs = nowUs;

    // Le os 3 canais rapidamente e volta para o loop.
    uint32_t mvR = readMilliVoltsSafe(PIN_ZMPT_R);
    uint32_t mvS = readMilliVoltsSafe(PIN_ZMPT_S);
    uint32_t mvT = readMilliVoltsSafe(PIN_ZMPT_T);
    uint32_t mvCurrent = readMilliVoltsSafe(PIN_CURRENT);

    accR.add((double)mvR);
    accS.add((double)mvS);
    accT.add((double)mvT);
    accCurrent.add((double)mvCurrent);
  }
}

void updateFlowSensor()
{
  if (PIN_FLOW < 0) { flowLitersMinute = 0.0f; return; }
  uint32_t now = millis();
  if ((uint32_t)(now - lastFlowCalcMs) < 1000)
    return;
  uint32_t elapsed = now - lastFlowCalcMs;
  lastFlowCalcMs = now;

  noInterrupts();
  uint32_t pulses = flowPulseCount;
  interrupts();
  uint32_t delta = pulses - lastFlowPulseSnapshot;
  lastFlowPulseSnapshot = pulses;

  if (cfgFlowPulsesPerLiter > 0.0f && elapsed > 0)
  {
    float liters = (float)delta / cfgFlowPulsesPerLiter;
    totalLiters += liters;
    flowLitersMinute = liters * 60000.0f / (float)elapsed;
  }
}

// ---------------------------------------------------------------------------
// WI-FI / AP
// ---------------------------------------------------------------------------
void startConfigAp(bool manualRequest = false)
{
  if (configApActive)
  {
    // Se o AP ja estava ativo e o usuario fez uma solicitacao manual,
    // apenas marca a origem e renova o temporizador de ociosidade.
    if (manualRequest)
    {
      configApOpenedManually = true;
      configApNoClientSince = millis();
      Serial.println("[AP] AP ja estava ativo. Temporizador renovado pelo botao FLASH/BOOT.");
    }
    return;
  }

  // Mantem STA ativa, se existir, e adiciona o AP.
  WiFi.mode(WIFI_AP_STA);

  String ssid = apSsid();
  bool ok = WiFi.softAP(ssid.c_str(), CONFIG_AP_PASSWORD);

  if (!ok)
  {
    Serial.println("[AP] Falha ao iniciar Access Point.");
    return;
  }

  configApActive = true;
  configApOpenedManually = manualRequest;
  configApLastClientCount = 0;
  configApNoClientSince = millis();

  IPAddress apIp = WiFi.softAPIP();
  dnsServer.stop();
  dnsServer.start(53, "*", apIp);

  Serial.println();
  Serial.println("======================================================");
  Serial.println("[AP] MODO DE CONFIGURACAO ATIVO");
  Serial.printf("[AP] Origem: %s\n", manualRequest ? "FLASH/BOOT 30s" : "SEM WI-FI SALVO");
  Serial.printf("[AP] Rede: %s\n", ssid.c_str());
  Serial.printf("[AP] Senha: %s\n", CONFIG_AP_PASSWORD);
  Serial.printf("[AP] Abra: http://%s\n", apIp.toString().c_str());
  Serial.println("[AP] Sem clientes por 5 minutos -> AP sera encerrado.");
  Serial.println("======================================================");
}

void stopConfigAp(const char *reason)
{
  if (!configApActive)
    return;

  Serial.println();
  Serial.printf("[AP] Encerrando modo de configuracao: %s\n", reason ? reason : "solicitado");

  dnsServer.stop();
  WiFi.softAPdisconnect(true);

  configApActive = false;
  configApOpenedManually = false;
  configApNoClientSince = 0;
  configApLastClientCount = 0;

  // Volta para o modo normal. Se houver credenciais salvas, tenta a STA.
  WiFi.mode(WIFI_STA);
  wifiWasConnected = (WiFi.status() == WL_CONNECTED);

  if (cfgWifiSsid.length() > 0 && WiFi.status() != WL_CONNECTED)
  {
    WiFi.disconnect(false);
    delay(20);
    WiFi.begin(cfgWifiSsid.c_str(), cfgWifiPass.c_str());
    wifiConnectStartedAt = millis();
    lastWifiRetryAt = millis();
    Serial.printf("[WIFI] Voltando a tentar a rede salva: %s\n", cfgWifiSsid.c_str());
  }
  else if (WiFi.status() == WL_CONNECTED)
  {
    Serial.printf("[WIFI] STA continua conectada. IP: %s\n", WiFi.localIP().toString().c_str());
  }
  else
  {
    Serial.println("[WIFI] Nenhuma rede salva. Use FLASH/BOOT por 30 s para reabrir a configuracao.");
  }
}

void maintainConfigAp()
{
  if (!configApActive)
    return;

  uint32_t now = millis();
  uint8_t clients = (uint8_t)WiFi.softAPgetStationNum();

  if (clients != configApLastClientCount)
  {
    Serial.printf("[AP] Clientes conectados: %u\n", (unsigned)clients);
    configApLastClientCount = clients;
  }

  if (clients > 0)
  {
    // Enquanto existir cliente conectado, o AP nao expira.
    configApNoClientSince = 0;
    return;
  }

  if (configApNoClientSince == 0)
    configApNoClientSince = now;

  if ((uint32_t)(now - configApNoClientSince) >= CONFIG_AP_IDLE_TIMEOUT_MS)
  {
    stopConfigAp("5 minutos sem nenhum cliente conectado");
  }
}

void beginWifi()
{
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  if (cfgWifiSsid.length() == 0)
  {
    Serial.println("[WIFI] Nenhuma rede salva. Abrindo AP inicial de configuracao.");
    startConfigAp(false);
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(cfgWifiSsid.c_str(), cfgWifiPass.c_str());

  wifiConnectStartedAt = millis();
  lastWifiRetryAt = millis();

  Serial.printf("[WIFI] Conectando em: %s\n", cfgWifiSsid.c_str());
  Serial.println("[WIFI] Se precisar configurar, apos o boot segure FLASH/BOOT por 30 segundos.");
}

void maintainWifi()
{
  wl_status_t status = WiFi.status();

  if (status == WL_CONNECTED)
  {
    if (!wifiWasConnected)
    {
      wifiWasConnected = true;
      Serial.printf("[WIFI] Conectado. IP: %s | RSSI: %d dBm\n",
                    WiFi.localIP().toString().c_str(),
                    WiFi.RSSI());
    }
    return;
  }

  if (wifiWasConnected)
  {
    wifiWasConnected = false;
    Serial.println("[WIFI] Conexao perdida.");
  }

  uint32_t now = millis();

  // Com credenciais ja salvas, NAO abre AP automaticamente por falha de rede.
  // Isso evita AP aparecendo sozinho e ciclos de abre/fecha. O usuario abre
  // manualmente mantendo FLASH/BOOT pressionado por 30 segundos.
  if (!configApActive &&
      cfgWifiSsid.length() > 0 &&
      (uint32_t)(now - wifiConnectStartedAt) >= WIFI_CONNECT_TIMEOUT_MS &&
      (uint32_t)(now - wifiConnectStartedAt) < WIFI_CONNECT_TIMEOUT_MS + 1000UL)
  {
    Serial.println("[WIFI] Rede salva nao encontrada. AP manual disponivel: segure FLASH/BOOT por 30 s.");
  }

  // Tenta novamente periodicamente, inclusive se o AP estiver aberto.
  if (!wifiScanInProgress &&
      cfgWifiSsid.length() > 0 &&
      (uint32_t)(now - lastWifiRetryAt) >= WIFI_RETRY_MS)
  {
    lastWifiRetryAt = now;
    Serial.println("[WIFI] Tentando reconectar...");
    WiFi.begin(cfgWifiSsid.c_str(), cfgWifiPass.c_str());
  }
}

void checkConfigButton()
{
  const bool pressed = (digitalRead(PIN_CONFIG_BUTTON) == LOW);
  const uint32_t now = millis();

  // Por seguranca, o botao so fica armado depois que for visto solto pelo
  // firmware ao menos uma vez. Isso evita interpretar um nivel LOW do boot
  // como pedido de configuracao.
  if (!configButtonArmed)
  {
    if (!pressed)
    {
      configButtonArmed = true;
      configButtonPressedAt = 0;
      configButtonTriggered = false;
      Serial.println("[BOTAO] FLASH/BOOT pronto. Segure por 30 s para abrir o AP.");
    }
    return;
  }

  if (!pressed)
  {
    if (configButtonPressedAt != 0 && !configButtonTriggered)
    {
      uint32_t held = (uint32_t)(now - configButtonPressedAt);
      if (held >= DMK_PAIR_HOLD_MS)
      {
        Serial.println("[BOTAO] Solto entre 3 e 30 s: abrindo PAREAMENTO de controle.");
        DmkLink::pairOpen(DMK_PAIR_WINDOW_S);
      }
      else
        Serial.println("[BOTAO] FLASH/BOOT solto antes de 3 s. Nada a fazer.");
    }

    configButtonPressedAt = 0;
    configButtonLastLogAt = 0;
    configButtonTriggered = false;
    return;
  }

  if (configButtonPressedAt == 0)
  {
    configButtonPressedAt = now;
    configButtonLastLogAt = now;
    configButtonTriggered = false;
    Serial.println("[BOTAO] FLASH/BOOT pressionado: solte apos 3 s = PAREAR | segure 30 s = AP de configuracao.");
    return;
  }

  const uint32_t heldMs = (uint32_t)(now - configButtonPressedAt);

  // Feedback no serial a cada 5 segundos para mostrar que a leitura esta viva.
  if (!configButtonTriggered &&
      (uint32_t)(now - configButtonLastLogAt) >= 5000UL)
  {
    configButtonLastLogAt = now;
    uint32_t heldSec = heldMs / 1000UL;
    if (heldSec > 30UL) heldSec = 30UL;
    Serial.printf("[BOTAO] FLASH/BOOT: %lu/30 s\n", (unsigned long)heldSec);
  }

  if (!configButtonTriggered && heldMs >= CONFIG_BUTTON_HOLD_MS)
  {
    configButtonTriggered = true;
    Serial.println("[BOTAO] 30 s confirmados. Abrindo AP de configuracao.");
    startConfigAp(true);
  }
}

// ---------------------------------------------------------------------------
// API WEB + ARQUIVOS DA PASTA data/ (LittleFS)
// ---------------------------------------------------------------------------
String jsonEscape(const String &input)
{
  String out;
  out.reserve(input.length() + 16);

  for (size_t i = 0; i < input.length(); i++)
  {
    char c = input[i];
    switch (c)
    {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if ((uint8_t)c >= 0x20)
        out += c;
      break;
    }
  }

  return out;
}

String boolJson(bool value)
{
  return value ? "true" : "false";
}

String buildStatusJson()
{
  String json;
  json.reserve(900);

  json += "{";
  json += "\"firmware_name\":\"" + jsonEscape(String(FIRMWARE_NAME)) + "\",";
  json += "\"version\":\"" + jsonEscape(String(FIRMWARE_VERSION)) + "\",";
  json += "\"build\":\"" + jsonEscape(String(BUILD_DATE)) + "\",";
  json += "\"device\":\"" + jsonEscape(cfgDeviceName) + "\",";
  json += "\"wifi\":" + boolJson(WiFi.status() == WL_CONNECTED) + ",";
  json += "\"mqtt\":" + boolJson(mqttStarted && mqttConnected) + ",";
  json += "\"config_mode\":" + boolJson(configApActive) + ",";
  json += "\"filesystem\":" + boolJson(littleFsReady) + ",";
  json += "\"ip\":\"" + jsonEscape(WiFi.localIP().toString()) + "\",";
  json += "\"ap_ip\":\"" + jsonEscape(WiFi.softAPIP().toString()) + "\",";
  json += "\"ap_ssid\":\"" + jsonEscape(apSsid()) + "\",";
  json += "\"rssi\":" + String((WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -127) + ",";
  json += "\"vR\":" + String(voltageR, 2) + ",";
  json += "\"vS\":" + String(voltageS, 2) + ",";
  json += "\"vT\":" + String(voltageT, 2) + ",";
  json += "\"adcVrmsR\":" + String(adcVrmsR, 4) + ",";
  json += "\"adcVrmsS\":" + String(adcVrmsS, 4) + ",";
  json += "\"adcVrmsT\":" + String(adcVrmsT, 4) + ",";
  json += "\"current_a\":" + String(currentA, 3) + ",";
  json += "\"temperature_c\":" + String(temperatureC, 2) + ",";
  json += "\"flow_l_min\":" + String(flowLitersMinute, 3) + ",";
  json += "\"total_l\":" + String(totalLiters, 3) + ",";
  json += "\"rain_pct\":" + String(rainPercent, 1) + ",";
  json += "\"alarm\":" + boolJson(alarmActive) + ",";
  json += "\"light\":" + boolJson(lightOn) + ",";
  json += "\"pump\":" + boolJson(pumpOn) + ",";
  json += "\"protection_enabled\":" + boolJson(cfgProtectionEnabled) + ",";
  json += "\"protection_ok\":" + boolJson(protectionOk) + ",";
  json += "\"protection_reason\":\"" + jsonEscape(protectionReason) + "\",";
  json += "\"clock_valid\":" + boolJson(DmkClock::isValid()) + ",";
  json += "\"clock_local\":\"" + jsonEscape(DmkClock::localDateTime()) + "\",";
  json += "\"clock_source\":\"" + jsonEscape(String(DmkClock::source())) + "\",";
  json += "\"uptime_s\":" + String(millis() / 1000UL) + ",";
  json += "\"target\":\"" + String(DMK_OTA_TARGET) + "\",";
  json += "\"default_password\":" + boolJson(cfgOtaPassword == DEFAULT_OTA_PASSWORD) + ",";
  json += "\"controls\":" + String(DmkLink::peerCount()) + ",";
  json += "\"pairing\":" + boolJson(DmkLink::pairIsOpen()) + ",";
  json += "\"espnow_channel\":" + String(DmkLink::channel()) + ",";
  json += "\"led\":\"" + String(DmkStatusLed::describe(DmkStatusLed::current())) + "\"";
  json += "}";

  return json;
}

String buildConfigJson()
{
  String json;
  json.reserve(1050);

  json += "{";
  json += "\"device_name\":\"" + jsonEscape(cfgDeviceName) + "\",";
  json += "\"wifi_ssid\":\"" + jsonEscape(cfgWifiSsid) + "\",";
  json += "\"mqtt_host\":\"" + jsonEscape(cfgMqttHost) + "\",";
  json += "\"mqtt_port\":" + String(cfgMqttPort) + ",";
  json += "\"mqtt_user\":\"" + jsonEscape(cfgMqttUser) + "\",";
  json += "\"topic\":\"" + jsonEscape(cfgTopicBase) + "\",";
  json += "\"relay_low\":" + boolJson(cfgRelayActiveLow) + ",";
  json += "\"light_low\":" + boolJson(cfgLightRelayActiveLow) + ",";
  json += "\"alarm_low\":" + boolJson(cfgAlarmActiveLow) + ",";
  json += "\"cal_r\":" + String(cfgCalR, 3) + ",";
  json += "\"cal_s\":" + String(cfgCalS, 3) + ",";
  json += "\"cal_t\":" + String(cfgCalT, 3) + ",";
  json += "\"protection\":" + boolJson(cfgProtectionEnabled) + ",";
  json += "\"vmin\":" + String(cfgVoltageMin, 1) + ",";
  json += "\"vmax\":" + String(cfgVoltageMax, 1) + ",";
  json += "\"current_cal\":" + String(cfgCurrentAmpsPerVolt, 3) + ",";
  json += "\"flow_cal\":" + String(cfgFlowPulsesPerLiter, 1) + ",";
  json += "\"rain_dry\":" + String(cfgRainDryRaw) + ",";
  json += "\"rain_wet\":" + String(cfgRainWetRaw) + ",";
  json += "\"pair_share\":" + boolJson(cfgPairShare) + ",";
  json += "\"ota_user\":\"" + jsonEscape(String(OTA_USER)) + "\"";
  json += "}";

  return json;
}

String contentTypeForPath(const String &path)
{
  if (path.endsWith(".html"))
    return "text/html; charset=utf-8";
  if (path.endsWith(".css"))
    return "text/css; charset=utf-8";
  if (path.endsWith(".js"))
    return "application/javascript; charset=utf-8";
  if (path.endsWith(".json"))
    return "application/json; charset=utf-8";
  if (path.endsWith(".svg"))
    return "image/svg+xml";
  if (path.endsWith(".ico"))
    return "image/x-icon";
  if (path.endsWith(".woff2"))
    return "font/woff2";
  return "application/octet-stream";
}

// Paginas do painel ficam DENTRO do firmware (gzip), geradas de data/serve.
// A OTA atualiza firmware e paginas juntos; nao precisa gravar "uploadfs".
bool serveDataFile(const String &path, bool noCache = false)
{
  const DmkWebAsset *asset = dmkFindWebAsset(path);
  if (!asset)
  {
    server.send(404, "text/plain; charset=utf-8", "Arquivo do painel nao encontrado.");
    return false;
  }
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", noCache ? "no-store, no-cache, must-revalidate" : "public, max-age=86400");
  server.send_P(200, asset->mime, (PGM_P)asset->data, asset->len);
  return true;
}

void sendJsonMessage(int code, bool ok, const String &message)
{
  String json = "{\"ok\":" + boolJson(ok) +
                ",\"message\":\"" + jsonEscape(message) + "\"}";
  server.send(code, "application/json; charset=utf-8", json);
}

void handleRoot()
{
  serveDataFile("/index.html", true);
}

void handleWifiScan()
{
  if (!configApActive)
  {
    sendJsonMessage(403, false, "Ative o modo de configuracao segurando FLASH/BOOT por 30 segundos.");
    return;
  }

  int16_t result = WiFi.scanComplete();

  if (result == WIFI_SCAN_RUNNING)
  {
    wifiScanInProgress = true;
    server.send(202, "application/json; charset=utf-8", "{\"ok\":true,\"scanning\":true}");
    return;
  }

  if (result == WIFI_SCAN_FAILED)
  {
    WiFi.scanDelete();
    int16_t started = WiFi.scanNetworks(true, true);
    wifiScanInProgress = (started == WIFI_SCAN_RUNNING);

    if (!wifiScanInProgress)
    {
      sendJsonMessage(500, false, "Nao foi possivel iniciar a busca de redes Wi-Fi.");
      return;
    }

    server.send(202, "application/json; charset=utf-8", "{\"ok\":true,\"scanning\":true}");
    return;
  }

  wifiScanInProgress = false;

  String json;
  json.reserve(250 + (result > 0 ? result * 95 : 0));
  json += "{\"ok\":true,\"scanning\":false,\"networks\":[";

  for (int i = 0; i < result; i++)
  {
    if (i > 0)
      json += ",";
    json += "{";
    json += "\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",";
    json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
#if defined(DMK_BOARD_ESP8266)
    json += "\"secure\":" + boolJson(WiFi.encryptionType(i) != ENC_TYPE_NONE);
#else
    json += "\"secure\":" + boolJson(WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
#endif
    json += "}";
  }

  json += "]}";
  WiFi.scanDelete();
  server.send(200, "application/json; charset=utf-8", json);
}

// Rotas que alteram algo exigem login (admin + senha do painel), exceto para
// quem esta conectado no AP de configuracao (precisa do botao fisico).
bool requestFromConfigAp()
{
  return configApActive && server.client().localIP() == WiFi.softAPIP();
}

bool requireWebAuth()
{
  if (requestFromConfigAp())
    return true;
  if (server.authenticate(DMK_WEB_USER, cfgOtaPassword.c_str()))
    return true;
  server.requestAuthentication(BASIC_AUTH, "DMK Smart");
  return false;
}

bool requireOtaAuth()
{
  if(MqttOta::busy()){server.send(409,"text/plain","OTA MQTT em andamento");return false;}
  return requireWebAuth();
}


void cancelFirmwareUpdate()
{
#if defined(DMK_BOARD_ESP8266)
  // ESP8266 nao oferece Update.abort().
  if (Update.isRunning())
    Update.end(false);
#else
  Update.abort();
#endif
}


void setupMqttOtaRoutes(){
  server.on("/ota-mqtt",HTTP_GET,[](){serveDataFile("/ota-mqtt.html",true);});
  server.on("/api/ota-mqtt/status",HTTP_GET,[](){server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",MqttOta::statusJson());});
  server.on("/api/ota-mqtt/config",HTTP_GET,[](){if(!requireWebAuth())return;server.send(200,"application/json",RemoteOta::configJson());});
  server.on("/api/ota-mqtt/command",HTTP_POST,[](){
    if(!requireWebAuth())return;
    String json,error;bool ok=MqttOta::requestLocal(server.arg("plain"),error);
    JsonDocument ack;ack["ok"]=ok;ack["error"]=error;serializeJson(ack,json);server.send(ok?202:400,"application/json",json);
  });
}
void setupWebServer()
{
  setupMqttOtaRoutes();
  server.on("/", HTTP_GET, handleRoot);
  server.on("/index.html", HTTP_GET, handleRoot);

  server.on("/api/status", HTTP_GET, []()
            {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json; charset=utf-8", buildStatusJson()); });

  server.on("/api/config", HTTP_GET, []()
            {
    if (!configApActive) {
      sendJsonMessage(403, false, "Configuracao disponivel somente no modo AP.");
      return;
    }
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json; charset=utf-8", buildConfigJson()); });

  server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);

  // -----------------------------------------------------------------------
  // RELOGIO DO ESP
  // -----------------------------------------------------------------------
  server.on("/api/clock", HTTP_GET, []()
            {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200,
                "application/json; charset=utf-8",
                DmkClock::statusJson(WiFi.status() == WL_CONNECTED)); });

  // Ajuste manual da hora LOCAL.
  // Campos: year, month, day, hour, minute, second(opcional)
  server.on("/api/clock/manual", HTTP_POST, []()
            {
    if (!requireWebAuth()) return;
    int year = server.arg("year").toInt();
    int month = server.arg("month").toInt();
    int day = server.arg("day").toInt();
    int hour = server.arg("hour").toInt();
    int minute = server.arg("minute").toInt();
    int second = server.hasArg("second") ? server.arg("second").toInt() : 0;

    if (!DmkClock::setManual(year, month, day, hour, minute, second)) {
      sendJsonMessage(400, false, "Data/hora invalida.");
      return;
    }

    sendJsonMessage(200, true, "Relogio do ESP ajustado."); });

  // -----------------------------------------------------------------------
  // AUTOMACOES DE HORARIO
  // -----------------------------------------------------------------------
  server.on("/api/automations", HTTP_GET, []()
            {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200,
                "application/json; charset=utf-8",
                AutomationManager::toJson()); });

  server.on("/api/automations/save", HTTP_POST, []()
            {
    if (!requireWebAuth()) return;
    uint32_t id = (uint32_t)server.arg("id").toInt();

    String name = server.arg("name");
    name.trim();
    if (name.length() == 0)
      name = "Automacao";

    String targetText = server.arg("target");
    targetText.trim();
    targetText.toUpperCase();

    uint8_t target = 0;
    if (targetText == "PUMP" || targetText == "BOMBA")
      target = AutomationManager::TARGET_PUMP;
    else if (targetText == "LIGHT" || targetText == "LUZ")
      target = AutomationManager::TARGET_LIGHT;

    String state = server.arg("state");
    state.trim();
    state.toUpperCase();

    if (target == 0 || (state != "ON" && state != "OFF")) {
      sendJsonMessage(400, false, "Dispositivo ou estado invalido.");
      return;
    }

    int hour = server.arg("hour").toInt();
    int minute = server.arg("minute").toInt();
    int daysMask = server.arg("days_mask").toInt();

    if (hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 ||
        (daysMask & AutomationManager::EVERY_DAY) == 0) {
      sendJsonMessage(400, false, "Horario ou dias invalidos.");
      return;
    }

    bool enabled = !server.hasArg("enabled") ||
                   server.arg("enabled") == "1" ||
                   server.arg("enabled") == "true" ||
                   server.arg("enabled") == "ON";

    uint32_t savedId = 0;
    if (!AutomationManager::save(
            id,
            name.c_str(),
            target,
            state == "ON",
            (uint8_t)hour,
            (uint8_t)minute,
            (uint8_t)daysMask,
            enabled,
            &savedId)) {
      sendJsonMessage(500, false, "Nao foi possivel salvar a automacao.");
      return;
    }

    String response = "{\"ok\":true,\"id\":" + String(savedId) +
                      ",\"message\":\"Automacao salva.\"}";
    server.send(200, "application/json; charset=utf-8", response); });

  server.on("/api/automations/delete", HTTP_POST, []()
            {
    if (!requireWebAuth()) return;
    uint32_t id = (uint32_t)server.arg("id").toInt();
    if (id == 0 || !AutomationManager::remove(id)) {
      sendJsonMessage(404, false, "Automacao nao encontrada.");
      return;
    }
    sendJsonMessage(200, true, "Automacao excluida."); });

  server.on("/api/automations/enable", HTTP_POST, []()
            {
    if (!requireWebAuth()) return;
    uint32_t id = (uint32_t)server.arg("id").toInt();
    bool enabled = server.arg("enabled") == "1" ||
                   server.arg("enabled") == "true" ||
                   server.arg("enabled") == "ON";

    if (id == 0 || !AutomationManager::setEnabled(id, enabled)) {
      sendJsonMessage(404, false, "Automacao nao encontrada.");
      return;
    }
    sendJsonMessage(200, true, enabled ? "Automacao ativada." : "Automacao desativada."); });

  // O aplicativo foi unificado em /index.html.
  // URLs antigas redirecionam para a tela principal.
  server.on("/automacoes.html", HTTP_GET, []()
            {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", ""); });

  server.on("/historico.html", HTTP_GET, []()
            {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", ""); });

  server.on("/historico-mqtt.html", HTTP_GET, []()
            {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", ""); });

  server.on("/APPDMK.html", HTTP_GET, handleRoot);

  server.on("/api/pump", HTTP_POST, []()
            {
    if (!requireWebAuth()) return;
    String state = server.arg("state");
    state.trim();
    state.toUpperCase();

    if (state != "ON" && state != "OFF") {
      sendJsonMessage(400, false, "Estado da bomba invalido.");
      return;
    }

    // Latitude/longitude sao OPCIONAIS.
    // Ausente, vazio ou invalido => 0.0 / 0.0 e location_valid=false.
    String latArg = server.arg("lat");
    String lonArg = server.hasArg("lon") ? server.arg("lon") : server.arg("lng");
    String accArg = server.arg("accuracy_m");
    latArg.trim();
    lonArg.trim();
    accArg.trim();

    bool hasLocation = latArg.length() > 0 && lonArg.length() > 0;
    double latitude = hasLocation ? strtod(latArg.c_str(), nullptr) : 0.0;
    double longitude = hasLocation ? strtod(lonArg.c_str(), nullptr) : 0.0;
    float accuracyM = accArg.length() > 0 ? accArg.toFloat() : 0.0f;

    if (!PumpHistory::queueCommand(state == "ON",
                                   "LOCAL_WEB",
                                   hasLocation,
                                   latitude,
                                   longitude,
                                   accuracyM,
                                   nullptr)) {
      sendJsonMessage(500, false, "Nao foi possivel enfileirar o comando da bomba.");
      return;
    }

    sendJsonMessage(202, true, state == "ON" ? "Comando para ligar a bomba recebido."
                                              : "Comando para desligar a bomba recebido."); });

  // Consulta os ultimos eventos de ligar/desligar da bomba.
  server.on("/api/pump/history", HTTP_GET, []()
            {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json; charset=utf-8", PumpHistory::toJson(50)); });

  // Limpa o historico manualmente, caso seja necessario.
  server.on("/api/pump/history/clear", HTTP_POST, []()
            {
    if (!requireWebAuth()) return;
    if (!PumpHistory::clear()) {
      sendJsonMessage(500, false, "Nao foi possivel limpar o historico da bomba.");
      return;
    }
    sendJsonMessage(200, true, "Historico da bomba apagado."); });

  server.on("/api/light", HTTP_POST, []()
            {
    if (!requireWebAuth()) return;
    String state = server.arg("state");
    state.toUpperCase();
    if (state != "ON" && state != "OFF") {
      sendJsonMessage(400, false, "Estado da luz invalido.");
      return;
    }
    applyLightOutput(state == "ON");
    publishLightState();
    sendJsonMessage(200, true, state == "ON" ? "Luz ligada." : "Luz desligada."); });

  server.on("/save", HTTP_POST, saveConfigFromWeb);

  server.on("/factory", HTTP_POST, []()
            {
    if (!configApActive) {
      sendJsonMessage(403, false, "Ative o AP segurando FLASH/BOOT por 30 segundos.");
      return;
    }

    if (pumpOn)
    {
      applyPumpOutput(false);
      publishPumpState();
      PumpHistory::logAutomatic(false, "FACTORY_RESET", "AUTOMATICO", "RESTAURACAO_FABRICA");
    }
    else
    {
      applyPumpOutput(false);
    }
    applyLightOutput(false);

    bool resetOk = true;

    if (prefs.begin("energycfg", false)) {
      resetOk = prefs.clear() && resetOk;
      prefs.end();
    } else resetOk = false;

    if (prefs.begin("dmkauto", false)) {
      resetOk = prefs.clear() && resetOk;
      prefs.end();
    } else resetOk = false;

    if (prefs.begin("dmkota", false)) {
      resetOk = prefs.clear() && resetOk;
      prefs.end();
    } else resetOk = false;

    if (prefs.begin("dmklink", false)) {
      resetOk = prefs.clear() && resetOk;
      prefs.end();
    } else resetOk = false;

    // Historico e dado operacional; em restauracao de fabrica tambem e zerado.
    if (!PumpHistory::clear()) resetOk = false;

    if (!resetOk) {
      sendJsonMessage(500, false, "Restauracao incompleta. Algumas areas da memoria nao puderam ser apagadas.");
      return;
    }

    sendJsonMessage(200, true, "Restauracao de fabrica concluida. Wi-Fi, MQTT, sensores, automacoes, OTA, pareamentos e historico foram apagados.");
    scheduledRestartAt = millis() + 1500; });

  // -----------------------------------------------------------------------
  // OTA REMOTA - servidor autorizado
  // -----------------------------------------------------------------------
  server.on("/api/ota/remote/config", HTTP_GET, []()
            {
    if (!requireOtaAuth()) return;
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json; charset=utf-8", RemoteOta::configJson()); });

  server.on("/api/ota/remote/config", HTTP_POST, []()
            {
    if (!requireOtaAuth()) return;

    String error;
    if (server.hasArg("localHttp") && !RemoteOta::allowHttpLocal(webArgTrue("localHttp"), error)) {
      sendJsonMessage(400, false, error);
      return;
    }
    if (server.hasArg("autoInstall") && !RemoteOta::setAutoInstall(webArgTrue("autoInstall"), error)) {
      sendJsonMessage(400, false, error);
      return;
    }
    if (!RemoteOta::saveConfig(
            server.arg("repo"),
            server.arg("manifestUrl"),
            server.arg("token"),
            server.arg("caCert"),
            error)) {
      sendJsonMessage(400, false, error);
      return;
    }

    sendJsonMessage(200, true, "Configuracao de atualizacao salva."); });

  server.on("/api/ota/remote/check", HTTP_POST, []()
            {
    if (!requireOtaAuth()) return;

    if (WiFi.status() != WL_CONNECTED) {
      sendJsonMessage(503, false, "Wi-Fi sem internet para consultar o servidor OTA.");
      return;
    }

    RemoteOta::Manifest manifest;
    String error;

    if (!RemoteOta::check(manifest, error)) {
      sendJsonMessage(502, false, error);
      return;
    }

    int cmp = RemoteOta::compareWithInstalled(manifest.version);

    String json;
    json.reserve(360);
    json += "{\"ok\":true";
    json += ",\"device\":\"" + jsonEscape(manifest.device) + "\"";
    json += ",\"version\":\"" + jsonEscape(manifest.version) + "\"";
    json += ",\"size\":" + String((unsigned long)manifest.size);
    json += ",\"sha256\":\"" + jsonEscape(manifest.sha256) + "\"";
    json += ",\"available\":";
    json += (cmp > 0) ? "true" : "false";
    json += ",\"same_version\":";
    json += (cmp == 0) ? "true" : "false";
    json += "}";

    server.send(200, "application/json; charset=utf-8", json); });

  server.on("/api/ota/remote/install", HTTP_POST, []()
            {
    if (!requireOtaAuth()) return;

    if (WiFi.status() != WL_CONNECTED) {
      sendJsonMessage(503, false, "Wi-Fi sem internet para baixar firmware remoto.");
      return;
    }

    // Instalacao em segundo plano: o painel acompanha por /api/ota-mqtt/status.
    String req = String("{\"cmd\":\"install\",\"force\":") +
                 ((server.arg("force") == "1" || server.arg("force") == "true") ? "true" : "false") + "}";
    String error;
    if (!MqttOta::requestLocal(req, error)) {
      sendJsonMessage(409, false, error);
      return;
    }
    sendJsonMessage(202, true, "Atualizacao iniciada. A bomba foi desligada por seguranca."); });

  // -----------------------------------------------------------------------
  // CONTROLES REMOTOS (ESP-NOW / REDE LOCAL)
  // -----------------------------------------------------------------------
  server.on("/api/link", HTTP_GET, []()
            {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json; charset=utf-8", buildLinkJson()); });

  server.on("/api/link/pair", HTTP_POST, []()
            {
    if (!requireWebAuth()) return;
    if (server.arg("state") == "OFF") {
      DmkLink::pairClose();
      sendJsonMessage(200, true, "Pareamento encerrado.");
      return;
    }
    DmkLink::pairOpen(DMK_PAIR_WINDOW_S);
    sendJsonMessage(200, true, "Pareamento aberto por 2 minutos. No controle, segure BOMBA + LUZ por 3 segundos."); });

  server.on("/api/link/remove", HTTP_POST, []()
            {
    if (!requireWebAuth()) return;
    uint8_t mac[6];
    if (!DmkLink::textToMac(server.arg("mac"), mac) || !DmkLink::removePeer(mac)) {
      sendJsonMessage(404, false, "Controle nao encontrado.");
      return;
    }
    sendJsonMessage(200, true, "Controle removido da lista. Para bloquear de vez, use 'Trocar chave'."); });

  server.on("/api/link/rekey", HTTP_POST, []()
            {
    if (!requireWebAuth()) return;
    if (server.arg("confirm") != "TROCAR") {
      sendJsonMessage(400, false, "Confirme com confirm=TROCAR.");
      return;
    }
    DmkLink::regenerateKey();
    newLinkSession();
    sendJsonMessage(200, true, "Chave trocada. Todos os controles precisam parear novamente."); });

  server.on("/api/link/share", HTTP_POST, []()
            {
    if (!requireWebAuth()) return;
    bool enabled = webArgTrue("enabled");
    if (!prefs.begin("energycfg", false)) {
      sendJsonMessage(500, false, "Memoria indisponivel.");
      return;
    }
    prefs.putBool("pairShare", enabled);
    bool ok = prefs.getBool("pairShare", !enabled) == enabled;
    prefs.end();
    if (!ok) {
      sendJsonMessage(500, false, "Falha ao gravar.");
      return;
    }
    cfgPairShare = enabled;
    sendJsonMessage(200, true, enabled ? "O controle recebera Wi-Fi e MQTT ao parear." : "O controle NAO recebera Wi-Fi/MQTT ao parear."); });

  server.on("/update", HTTP_GET, []()
            {
    server.sendHeader("Location", "/#ota", true);
    server.send(302, "text/plain", ""); });

  server.on("/update", HTTP_POST, []()
            {
    if (!requireOtaAuth()) return;

    bool ok = otaUploadOk && !Update.hasError();
    otaInProgress = false;

    if (ok) {
      sendJsonMessage(200, true, "Atualizacao concluida. O ESP32 vai reiniciar.");
      scheduledRestartAt = millis() + 1200;
    } else {
      sendJsonMessage(500, false, "Falha na atualizacao. O firmware anterior foi mantido.");
    } }, []()
            {
    if(MqttOta::busy()) return;
    if (!requestFromConfigAp() && !server.authenticate(DMK_WEB_USER, cfgOtaPassword.c_str())) return;

    HTTPUpload &upload = server.upload();

    if (upload.status == UPLOAD_FILE_START) {
      if (pumpOn)
      {
        applyPumpOutput(false);
        publishPumpState();
        PumpHistory::logAutomatic(false, "OTA", "AUTOMATICO", "INICIO_ATUALIZACAO");
      }
      else
      {
        applyPumpOutput(false);
      }
      applyLightOutput(false);
      otaUploadOk = false;
      otaInProgress = true;

      #if defined(DMK_BOARD_ESP8266)
      // ESP8266 core nao define UPDATE_SIZE_UNKNOWN.
      // Reserva todo o espaco livre valido para o novo sketch.
      const size_t otaMaxSketchSize =
          (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;

      if (!Update.begin(otaMaxSketchSize)) {
#else
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
#endif
        Update.printError(Serial);
      }
    }
    else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    }
    else if (upload.status == UPLOAD_FILE_END) {
      otaUploadOk = Update.end(true);
      if (!otaUploadOk) Update.printError(Serial);
    }
    else if (upload.status == UPLOAD_FILE_ABORTED) {
      cancelFirmwareUpdate();
      otaInProgress = false;
    } });

  // Rotas de deteccao de captive portal.
  server.on("/generate_204", HTTP_GET, handleRoot);
  server.on("/hotspot-detect.html", HTTP_GET, handleRoot);
  server.on("/ncsi.txt", HTTP_GET, handleRoot);
  server.on("/connecttest.txt", HTTP_GET, handleRoot);
  server.on("/fwlink", HTTP_GET, handleRoot);

  server.onNotFound([]()
                    {
    String uri = server.uri();

    // Arquivos estaticos da pasta data/.
    if (uri.startsWith("/assets/") || uri.startsWith("/vendor/")) {
      serveDataFile(uri, true);
      return;
    }

    // Em modo AP, qualquer endereco deve voltar para o captive portal.
    if (configApActive) {
      server.sendHeader("Location", "/", true);
      server.send(302, "text/plain", "");
      return;
    }

    server.send(404, "text/plain; charset=utf-8", "Nao encontrado."); });

  webServerConfigured = true;
  Serial.println("[WEB] Rotas HTTP configuradas. Painel embutido no firmware.");
}

void startWebServerIfReady()
{
  if (!webServerConfigured || webServerStarted)
    return;

  if (WiFi.status() != WL_CONNECTED && !configApActive)
    return;

  server.begin();
  webServerStarted = true;

  if (configApActive)
  {
    Serial.printf("[WEB] Servidor HTTP ativo no AP: http://%s\n",
                  WiFi.softAPIP().toString().c_str());
  }
  else
  {
    Serial.printf("[WEB] Servidor HTTP ativo: http://%s\n",
                  WiFi.localIP().toString().c_str());
  }
}

// ---------------------------------------------------------------------------
// SETUP / LOOP
// ---------------------------------------------------------------------------
bool dmkPrepareMqttOta(String &error){
  if(otaInProgress||Update.isRunning()){error="Outra OTA em andamento";return false;}
  applyPumpOutput(false);applyLightOutput(false);publishPumpState();publishLightState();
  PumpHistory::logAutomatic(false,"OTA_MQTT","AUTOMATICO","INICIO_ATUALIZACAO");otaInProgress=true;return true;
}
void dmkEndMqttOta(bool success){otaInProgress=success;}
bool dmkCanAutoUpdate(){return !pumpOn && !otaInProgress;}

void setup()
{
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("======================================================");
  Serial.println(" ESP32 - MONITOR TRIFASICO + BOMBA + MQTT");
  Serial.printf(" FIRMWARE: %s v%s | build %s\n", FIRMWARE_NAME, FIRMWARE_VERSION, BUILD_DATE);
  Serial.println("======================================================");

#if defined(DMK_BOARD_ESP8266)
  Serial.println("[FS] Montando LittleFS para configuracoes ESP8266...");
  littleFsReady = LittleFS.begin();
  loadConfig();
#else
  loadConfig();
  Serial.println("[FS] Montando LittleFS (historico da bomba)...");
  // Tabela 1.1 usa o rotulo "spiffs"; a 1.0.0 usava "littlefs".
  littleFsReady = LittleFS.begin(true) || LittleFS.begin(true, "/littlefs", 10, "littlefs");
#endif
  if (littleFsReady)
  {
    Serial.println("[FS] LittleFS pronto.");
  }
  else
  {
    Serial.println("[FS] ERRO: LittleFS nao montado. Painel e controle funcionam; historico fica indisponivel.");
  }

  // O mesmo LittleFS guarda a interface da pasta data/ e o historico da bomba.
  PumpHistory::begin(littleFsReady);

  // PRIMEIRO coloca atuadores em estado seguro. So depois iniciamos relogio,
  // automacoes, rede ou MQTT, para nada conseguir religar a bomba no meio do boot.
  Serial.println("[BOOT] 2/6 - Configurando GPIO em estado seguro...");
  initializeActuatorOutputsSafe();
  pinMode(PIN_CONFIG_BUTTON, INPUT_PULLUP);
  DmkStatusLed::begin(PIN_STATUS_LED, STATUS_LED_ACTIVE_LOW, STATUS_LED_RGB);
  DmkStatusLed::set(DmkStatusLed::LED_SLOW);

  // Relogio interno. Ajusta por NTP quando a internet estiver disponivel.
  // Se reiniciar sem internet, as automacoes ficam pausadas ate a hora ser
  // sincronizada ou ajustada manualmente pela web.
  DmkClock::begin("BRT3");

  // Automacoes persistentes de BOMBA/LUZ. Os atuadores ja estao OFF neste ponto.
  AutomationManager::begin(scheduledPumpCommand, scheduledLightCommand);

  // OTA remota segura por HTTPS + CA + manifesto SHA-256.
  if (PIN_FLOW >= 0) {
    pinMode(PIN_FLOW, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_FLOW), onFlowPulse, RISING);
  }
  if (PIN_ALARM >= 0) pinMode(PIN_ALARM, INPUT_PULLUP);

  Serial.println("[BOOT] 3/6 - Configurando ADC...");
#if !defined(DMK_BOARD_ESP8266)
  analogReadResolution(12);
  if (PIN_ZMPT_R >= 0) analogSetPinAttenuation(PIN_ZMPT_R, ADC_11db);
  if (PIN_ZMPT_S >= 0) analogSetPinAttenuation(PIN_ZMPT_S, ADC_11db);
  if (PIN_ZMPT_T >= 0) analogSetPinAttenuation(PIN_ZMPT_T, ADC_11db);
  if (PIN_CURRENT >= 0) analogSetPinAttenuation(PIN_CURRENT, ADC_11db);
  if (PIN_TEMPERATURE >= 0) analogSetPinAttenuation(PIN_TEMPERATURE, ADC_11db);
  if (PIN_RAIN >= 0) analogSetPinAttenuation(PIN_RAIN, ADC_11db);
#endif

  // IMPORTANTE:
  // Inicializa a pilha Wi-Fi ANTES de iniciar qualquer servidor TCP.
  Serial.println("[BOOT] 4/6 - Inicializando Wi-Fi...");
  beginWifi();

  // Enlace com os controles: ESP-NOW + UDP local, cifrado e autenticado.
  newLinkSession();
  DmkLink::serveSetup(linkShareConfig, linkOnPaired);
  DmkLink::begin(cfgDeviceName.c_str(), FIRMWARE_VERSION, linkOnFrame);
  if (DmkLink::peerCount() == 0)
  {
    // Primeira instalacao: facilita parear sem apertar nada no SERVE.
    DmkLink::pairOpen(DMK_PAIR_FIRST_BOOT_S);
  }

  // Apenas registra as rotas. server.begin() ocorre no loop quando a
  // interface de rede estiver realmente ativa.
  Serial.println("[BOOT] 5/6 - Configurando servidor web...");
  setupWebServer();

  // Configura MQTT, mas ainda não cria a tarefa.
  setupMqtt();

  lastMeasureCycleMs = 0;
  lastFlowCalcMs = millis();

  Serial.println("[BOOT] Inicializacao principal concluida.");
  MqttOta::begin();
}

void loop()
{
  MqttOta::loop(cfgMqttHost,cfgMqttPort,cfgMqttUser,cfgMqttPass,cfgOtaPassword);

  // Mantem Wi-Fi e modo de configuracao.
  maintainWifi();
  checkConfigButton();
  maintainConfigAp();

  // Servidor só é iniciado depois que Wi-Fi/AP estiver pronto.
  startWebServerIfReady();

  if (webServerStarted)
  {
    server.handleClient();
  }

  if (configApActive)
  {
    dnsServer.processNextRequest();
  }

  // Controles: ESP-NOW (prioridade) e rede local.
  maintainLink();

  // MQTT só inicia quando a STA tiver recebido IP.
  startMqttIfReady();
#if defined(DMK_BOARD_ESP8266)
  if (mqttClient8266.connected()) {
    mqttClient8266.loop();
    mqttConnected = true;
  } else {
    mqttConnected = false;
  }
#endif

  // Medicao e comandos sem delays longos.
  if (!otaInProgress)
    updateMeasurements();
  updateFlowSensor();

  // Relogio e automacoes nao bloqueantes.
  DmkClock::update(WiFi.status() == WL_CONNECTED);
  if(!otaInProgress&&!MqttOta::busy())AutomationManager::update();

  // Mantem o relogio/agenda atualizados no aplicativo remoto.
  if (mqttConnected &&
      (uint32_t)(millis() - lastScheduleStatePublishMs) >= 30000UL)
  {
    publishAutomationStateMqtt();
  }

  // Processa inclusive comandos criados por uma automacao de horario.
  if(!otaInProgress&&!MqttOta::busy())processPumpCommand();

  // Publicacoes iniciais apos cada conexao/reconexao MQTT.
  if (mqttJustConnected)
  {
    mqttJustConnected = false;

    if (mqttCanPublish())
    {
      dmkMqttPublish(topicAvailability.c_str(), "online", 0, 1, true);

      publishPumpState();
      publishLightState();
      publishTelemetry();
    }
  }

  // Reinicio agendado apos salvar/factory reset.
  if (scheduledRestartAt != 0 &&
      (int32_t)(millis() - scheduledRestartAt) >= 0)
  {
    if (pumpOn)
    {
      applyPumpOutput(false);
      publishPumpState();
      PumpHistory::logAutomatic(false, "RESTART", "AUTOMATICO", "REINICIO_AGENDADO");
    }
    else
    {
      applyPumpOutput(false);
    }
    applyLightOutput(false);
    delay(50);
    ESP.restart();
  }

  updateStatusLed();

  // Pequeno yield para o RTOS/Wi-Fi.
  delay(1);
}
