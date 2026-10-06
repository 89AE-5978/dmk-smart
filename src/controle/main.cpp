/*
  ============================================================================
  DMK SMART - CONTROLE REMOTO (BOMBA + LUZ + LCD + 3 BOTOES + LEDs)
  Fala com o SERVE por: 1) ESP-NOW  2) Rede local (UDP)  3) MQTT
  Escolha automatica: usa o primeiro caminho que responder.
  ============================================================================
*/

#include <Arduino.h>

#if defined(DMK_BOARD_ESP8266)
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <Wire.h>
#include <Updater.h>
#include <LittleFS.h>
#include <PubSubClient.h>
#include "DmkPrefs.h"
using DmkWebServer = ESP8266WebServer;
#else
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Wire.h>
#include <Update.h>
#include <LittleFS.h>
#include <esp_idf_version.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "mqtt_client.h"
using DmkWebServer = WebServer;
#endif

#include "firmware_info.h"
#include "MqttOta.h"
#include "RemoteOta.h"
#include <ArduinoJson.h>
#include "PinsConfig.h"
#include "ProjectConfig.h"
#include "UserSettings.h"
#include "DmkConfig.h"
#include "DmkProto.h"
#include "DmkLink.h"
#include "DmkStatusLed.h"
#include "DmkWebAssets.h"

// ---------------------------------------------------------------------------
// OBJETOS
// ---------------------------------------------------------------------------
Preferences prefs;
DmkWebServer server(80);
DNSServer dnsServer;

#if defined(DMK_BOARD_ESP8266)
WiFiClient mqttNetworkClient;
PubSubClient mqttClient(mqttNetworkClient);
uint32_t lastMqttReconnectMs = 0;
#else
esp_mqtt_client_handle_t mqttClient = nullptr;
#endif

// ---------------------------------------------------------------------------
// CONFIGURACAO
// ---------------------------------------------------------------------------
String cfgWifiSsid = "";
String cfgWifiPass = "";

String cfgMqttHost = DEFAULT_MQTT_HOST;
uint16_t cfgMqttPort = DEFAULT_MQTT_PORT;
String cfgTopicBase = DEFAULT_MQTT_TOPIC;
String cfgMqttUsername = DEFAULT_MQTT_USERNAME;
String cfgMqttPassword = DEFAULT_MQTT_PASSWORD;
String cfgMqttClientId = DEFAULT_MQTT_CLIENT_ID;
String cfgOtaPassword = DEFAULT_OTA_PASSWORD;

String deviceName = USER_DEVICE_NAME;

// ---------------------------------------------------------------------------
// MQTT TOPICOS
// ---------------------------------------------------------------------------
String topicTelemetry;
String topicPumpState;
String topicAvailability;
String topicPumpSet;
String topicLightState;
String topicLightSet;

// ---------------------------------------------------------------------------
// ESTADOS
// ---------------------------------------------------------------------------
bool configApActive = false;
bool webStarted = false;
uint32_t configApLastClientMs = 0;
uint8_t configApLastStationCount = 0;

bool mqttStarted = false;
volatile bool mqttConnected = false;
volatile bool mainEspOnline = false;

bool pumpStateKnown = false;
bool pumpOn = false;
bool lightOn = false;
bool lightStateKnown = false;

bool commandPending = false;
bool pendingTargetOn = false;
uint32_t commandSentMs = 0;
bool lightCommandPending = false;
bool pendingLightTargetOn = false;
uint32_t lightCommandSentMs = 0;

float voltageR = 0.0f;
float voltageS = 0.0f;
float voltageT = 0.0f;
float voltageMin = 180.0f;
float voltageMax = 250.0f;
float currentA = 0.0f;
float temperatureC = 0.0f;
float flowLitersMinute = 0.0f;
float totalLiters = 0.0f;
float rainPercent = 0.0f;
bool alarmActive = false;
char mainFirmwareVersion[16] = "--";
bool otaUploadOk = false;

int mainEspRssi = -127;
uint32_t lastTelemetryMs = 0;

// ---------------------------------------------------------------------------
// ENLACE COM O SERVE (ESP-NOW -> REDE LOCAL -> MQTT)
// ---------------------------------------------------------------------------
uint32_t lastEspNowStatusMs = 0;
uint32_t lastLanStatusMs = 0;
uint32_t lastMqttTelemetryMs = 0;
uint32_t serveSession = 0;
IPAddress serveIp(0, 0, 0, 0);
uint8_t serveMacAddr[6] = {0};
bool servePaired = false;
uint8_t sweepChannel = 0;
uint32_t lastPingMs = 0;
uint32_t restartAtMs = 0;
String linkMessage = "";
bool mainOtaBusy = false;

struct PendingCommand
{
  bool active = false;
  uint8_t target = 0;
  bool on = false;
  uint16_t seq = 0;
  int8_t stage = -1; // 0 ESP-NOW, 1 rede local, 2 MQTT
  uint32_t stageAt = 0;
  bool acked = false;
  uint8_t ackVia = 0;
} pendingCmd;
uint16_t commandSeq = 0;
uint8_t lastCommandVia = 0;

uint32_t lastWifiAttemptMs = 0;
uint32_t lastLcdUpdateMs = 0;
uint8_t lcdPage = 0;
static const uint8_t LCD_PAGE_COUNT = 6;
String lastLcdLines[4];
uint32_t lastMenuActionMs = 0;
bool lcdDisplayEnabled = true;
uint8_t lcdColumns = LCD_DEFAULT_COLUMNS;
uint8_t lcdRows = LCD_DEFAULT_ROWS;
bool lcdCustomMode = LCD_DEFAULT_CUSTOM_MODE;
String lcdCustomLines[4] = {LCD_DEFAULT_LINE1, LCD_DEFAULT_LINE2, LCD_DEFAULT_LINE3, LCD_DEFAULT_LINE4};
bool lcdRemoteMode = false;
String lcdRemoteKey = "";
String lcdRemoteLabel = "";
String lcdRemoteUnit = "";
String lastTelemetryPayload = "{}";
bool wifiScanActive = false;
bool menuLongActionDone = false;
bool menuApActionDone = false;
uint32_t menuPressedAt = 0;

struct LcdStateEvent
{
  uint8_t device;
  bool on;
}; // 1=bomba, 2=luz

#if defined(DMK_BOARD_ESP8266)
static LcdStateEvent lcdEventBuffer[8];
static uint8_t lcdEventHead = 0;
static uint8_t lcdEventTail = 0;
static uint8_t lcdEventCount = 0;

void lcdQueueInit()
{
  lcdEventHead = lcdEventTail = lcdEventCount = 0;
}

void lcdQueueReset()
{
  lcdEventHead = lcdEventTail = lcdEventCount = 0;
}

bool lcdQueueSend(const LcdStateEvent &event)
{
  if (lcdEventCount >= 8)
    return false;
  lcdEventBuffer[lcdEventTail] = event;
  lcdEventTail = (uint8_t)((lcdEventTail + 1) % 8);
  lcdEventCount++;
  return true;
}

bool lcdQueueReceive(LcdStateEvent &event)
{
  if (lcdEventCount == 0)
    return false;
  event = lcdEventBuffer[lcdEventHead];
  lcdEventHead = (uint8_t)((lcdEventHead + 1) % 8);
  lcdEventCount--;
  return true;
}
#else
QueueHandle_t lcdEventQueue = nullptr;

void lcdQueueInit()
{
  lcdEventQueue = xQueueCreate(8, sizeof(LcdStateEvent));
}

void lcdQueueReset()
{
  if (lcdEventQueue != nullptr)
    xQueueReset(lcdEventQueue);
}

bool lcdQueueSend(const LcdStateEvent &event)
{
  return lcdEventQueue != nullptr &&
         xQueueSend(lcdEventQueue, &event, 0) == pdTRUE;
}

bool lcdQueueReceive(LcdStateEvent &event)
{
  return lcdEventQueue != nullptr &&
         xQueueReceive(lcdEventQueue, &event, 0) == pdTRUE;
}
#endif

LcdStateEvent activeLcdEvent = {};
uint32_t activeLcdEventAt = 0;

// ---------------------------------------------------------------------------
// BOTAO
// ---------------------------------------------------------------------------
struct DebouncedButton
{
  uint8_t pin;
  bool lastReading = HIGH;
  bool stableState = HIGH;
  uint32_t changedAt = 0;
  explicit DebouncedButton(uint8_t gpio) : pin(gpio) {}
};
DebouncedButton pumpButton(PIN_PUMP_BUTTON);
DebouncedButton lightButton(PIN_LIGHT_BUTTON);
DebouncedButton menuButton(PIN_MENU_BUTTON);

// ---------------------------------------------------------------------------
// LCD PCF8574 - DRIVER INTERNO
// ---------------------------------------------------------------------------
// Mapeamento comum dos backpacks I2C:
// P0=RS, P1=RW, P2=EN, P3=BACKLIGHT, P4..P7=D4..D7

uint8_t lcdAddress = 0;
bool lcdReady = false;

static const uint8_t LCD_RS = 0x01;
static const uint8_t LCD_EN = 0x04;
static const uint8_t LCD_BL = 0x08;

void lcdExpanderWrite(uint8_t data)
{
  if (!lcdAddress)
    return;

  // O backlight acompanha o estado logico do LCD.
  // Assim o toque longo realmente apaga a tela, inclusive a iluminacao.
  uint8_t backlight = lcdDisplayEnabled ? LCD_BL : 0;

  Wire.beginTransmission(lcdAddress);
  Wire.write(data | backlight);
  Wire.endTransmission();
}

void lcdPulseEnable(uint8_t data)
{
  lcdExpanderWrite(data | LCD_EN);
  delayMicroseconds(1);
  lcdExpanderWrite(data & ~LCD_EN);
  delayMicroseconds(50);
}

void lcdWrite4bits(uint8_t value)
{
  lcdExpanderWrite(value);
  lcdPulseEnable(value);
}

void lcdSend(uint8_t value, uint8_t mode)
{
  uint8_t highNibble = value & 0xF0;
  uint8_t lowNibble = (value << 4) & 0xF0;

  lcdWrite4bits(highNibble | mode);
  lcdWrite4bits(lowNibble | mode);
}

void lcdCommand(uint8_t value)
{
  lcdSend(value, 0);
}

void lcdWriteChar(char c)
{
  lcdSend((uint8_t)c, LCD_RS);
}

uint8_t findLcdAddress()
{
  const uint8_t candidates[] = {0x27, 0x3F};

  for (uint8_t addr : candidates)
  {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0)
    {
      return addr;
    }
  }

  // Se nao achou nos dois enderecos mais comuns, faz scan geral.
  for (uint8_t addr = 1; addr < 127; addr++)
  {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0)
    {
      return addr;
    }
  }

  return 0;
}

void lcdInit()
{
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  delay(80);

  lcdAddress = findLcdAddress();

  if (!lcdAddress)
  {
    Serial.println("[LCD] Nenhum dispositivo I2C encontrado.");
    lcdReady = false;
    return;
  }

  Serial.printf("[LCD] Dispositivo encontrado em 0x%02X\n", lcdAddress);

  delay(50);

  // Sequencia de inicializacao HD44780 em modo 4 bits.
  lcdWrite4bits(0x30);
  delayMicroseconds(4500);

  lcdWrite4bits(0x30);
  delayMicroseconds(4500);

  lcdWrite4bits(0x30);
  delayMicroseconds(150);

  lcdWrite4bits(0x20);

  lcdCommand(0x28); // 4 bits, 2 linhas logicas, fonte 5x8
  lcdCommand(0x08); // display off
  lcdCommand(0x01); // clear
  delayMicroseconds(2000);

  lcdCommand(0x06); // entry mode
  lcdCommand(0x0C); // display on, cursor off

  lcdReady = true;
  Serial.printf("[LCD] LCD %ux%u inicializado.\n", lcdColumns, lcdRows);
}

void lcdSetCursor(uint8_t col, uint8_t row)
{
  if (!lcdReady)
    return;

  if (lcdRows < 1)
    lcdRows = 1;
  if (row >= lcdRows)
    row = lcdRows - 1;
  if (row > 3)
    row = 3;
  if (col >= lcdColumns)
    col = lcdColumns ? lcdColumns - 1 : 0;

  // HD44780:
  // 1/2 linhas usam 0x00 e 0x40.
  // 4 linhas normalmente intercalam as duas memórias internas.
  uint8_t offset = 0x00;
  if (row == 1)
    offset = 0x40;
  else if (row == 2)
    offset = (lcdColumns == 16) ? 0x10 : lcdColumns;
  else if (row == 3)
    offset = (lcdColumns == 16) ? 0x50 : (uint8_t)(0x40 + lcdColumns);

  lcdCommand(0x80 | (col + offset));
}

void lcdClear()
{
  if (!lcdReady)
    return;
  lcdCommand(0x01);
  delayMicroseconds(2000);
}

void lcdSetEnabled(bool enabled)
{
  if (lcdDisplayEnabled == enabled)
    return;

  lcdDisplayEnabled = enabled;
  if (!lcdReady)
    return;

  if (enabled)
  {
    lcdCommand(0x0C); // display on, cursor off

    // Sempre volta para a tela padrao das fases ao religar.
    lcdPage = 0;
    activeLcdEvent = {};
    lcdQueueReset();

    for (uint8_t row = 0; row < 4; row++)
      lastLcdLines[row] = "";
    lastLcdUpdateMs = 0;
    Serial.println("[LCD] Display LIGADO por toque longo.");
  }
  else
  {
    // Limpa o cache antes de apagar para forcar redesenho ao religar.
    for (uint8_t row = 0; row < 4; row++)
      lastLcdLines[row] = "";
    lcdCommand(0x08); // display off; lcdExpanderWrite tambem apaga o backlight
    Serial.println("[LCD] Display DESLIGADO por toque longo.");
  }
}

void lcdPrintRaw(const String &text)
{
  if (!lcdReady)
    return;

  for (size_t i = 0; i < text.length(); i++)
  {
    lcdWriteChar(text[i]);
  }
}

String fitLcd(String text)
{
  if (text.length() > lcdColumns)
  {
    text.remove(lcdColumns);
  }

  while (text.length() < lcdColumns)
  {
    text += ' ';
  }

  return text;
}

void lcdPrintLine(uint8_t row, const String &text)
{
  if (!lcdReady)
    return;

  lcdSetCursor(0, row);
  lcdPrintRaw(fitLcd(text));
}

void lcdPrintLineIfChanged(uint8_t row, const String &text)
{
  if (row >= lcdRows || row >= 4)
    return;

  String fitted = fitLcd(text);
  if (lastLcdLines[row] == fitted)
    return;

  lastLcdLines[row] = fitted;
  lcdPrintLine(row, fitted);
}

// ---------------------------------------------------------------------------
// HELPERS
// ---------------------------------------------------------------------------
String chipSuffix()
{
  char buf[7];

#if defined(DMK_BOARD_ESP8266)
  uint32_t id = ESP.getChipId();
  snprintf(buf, sizeof(buf), "%06lX", (unsigned long)(id & 0xFFFFFFUL));
#else
  uint64_t id = ESP.getEfuseMac();
  snprintf(buf, sizeof(buf), "%06lX", (unsigned long)(id & 0xFFFFFFULL));
#endif

  return String(buf);
}

uint32_t dmkCpuMhz()
{
#if defined(DMK_BOARD_ESP8266)
  return ESP.getCpuFreqMHz();
#else
  return ESP.getCpuFreqMHz();
#endif
}

uint32_t dmkHeapTotal()
{
#if defined(DMK_BOARD_ESP8266)
  // O core ESP8266 nao fornece heap total pelo mesmo metodo do ESP32.
  return ESP.getFreeHeap();
#else
  return ESP.getHeapSize();
#endif
}

uint32_t dmkHeapMin()
{
#if defined(DMK_BOARD_ESP8266)
  return ESP.getFreeHeap();
#else
  return ESP.getMinFreeHeap();
#endif
}

uint32_t dmkPsramSize()
{
#if defined(DMK_BOARD_ESP8266)
  return 0;
#else
  return ESP.getPsramSize();
#endif
}

uint32_t dmkPsramFree()
{
#if defined(DMK_BOARD_ESP8266)
  return 0;
#else
  return ESP.getFreePsram();
#endif
}

bool dmkFilesystemBegin()
{
#if defined(DMK_BOARD_ESP8266)
  return LittleFS.begin();
#else
  // Tabela 1.1 usa o rotulo "spiffs"; a 1.0.0 usava "littlefs".
  return LittleFS.begin(true) || LittleFS.begin(true, "/littlefs", 10, "littlefs");
#endif
}

bool dmkWifiOpenEncryption(int index)
{
#if defined(DMK_BOARD_ESP8266)
  return WiFi.encryptionType(index) == ENC_TYPE_NONE;
#else
  return WiFi.encryptionType(index) == WIFI_AUTH_OPEN;
#endif
}

bool dmkUpdateBegin()
{
#if defined(DMK_BOARD_ESP8266)
  const size_t freeSpace = ESP.getFreeSketchSpace();
  if (freeSpace <= 0x1000)
    return false;
  const size_t maxSketchSize = (freeSpace - 0x1000) & 0xFFFFF000;
  return Update.begin(maxSketchSize);
#else
  return Update.begin(UPDATE_SIZE_UNKNOWN);
#endif
}

void dmkUpdateAbort()
{
#if defined(DMK_BOARD_ESP8266)
  Update.end(false);
#else
  Update.abort();
#endif
}

String htmlEscape(const String &s)
{
  String out = s;

  out.replace("&", "&amp;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  out.replace("\"", "&quot;");
  out.replace("'", "&#39;");

  return out;
}

String jsonEscape(const String &s)
{
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++)
  {
    char c = s[i];
    if (c == '"' || c == '\\')
    {
      out += '\\';
      out += c;
    }
    else if ((uint8_t)c < 0x20)
      out += ' ';
    else
      out += c;
  }
  return out;
}

bool telemetryFresh()
{
  return lastTelemetryMs != 0 &&
         (uint32_t)(millis() - lastTelemetryMs) < TELEMETRY_TIMEOUT_MS;
}

static bool freshSince(uint32_t at)
{
  return at != 0 && (uint32_t)(millis() - at) < DMK_LINK_FRESH_MS;
}

// Melhor caminho ativo neste momento.
uint8_t activeVia()
{
  if (freshSince(lastEspNowStatusMs))
    return DmkLink::VIA_ESPNOW;
  if (freshSince(lastLanStatusMs) && WiFi.status() == WL_CONNECTED)
    return DmkLink::VIA_LAN;
  if (mqttConnected && freshSince(lastMqttTelemetryMs))
    return DmkLink::VIA_MQTT;
  return DmkLink::VIA_NONE;
}

bool mainCommunicationOk()
{
  // Qualquer caminho com estado recente do SERVE serve para comandar.
  return activeVia() != DmkLink::VIA_NONE && telemetryFresh();
}

bool canControlPump()
{
  return mainCommunicationOk() && pumpStateKnown && !commandPending;
}

bool canControlLight()
{
  return mainCommunicationOk() && lightStateKnown && !lightCommandPending;
}

void buildTopics()
{
  String base = cfgTopicBase;

  topicTelemetry = base + "/telemetry";
  topicPumpState = base + "/pump/state";
  topicAvailability = base + "/availability";
  topicPumpSet = base + "/pump/set";
  topicLightState = base + "/light/state";
  topicLightSet = base + "/light/set";
}

// ---------------------------------------------------------------------------
// PREFERENCES
// ---------------------------------------------------------------------------
void loadConfig()
{
  Serial.println("[CFG] Carregando configuracao...");

  if (!prefs.begin(PREFERENCES_NAMESPACE, false))
  {
    Serial.println("[CFG] Falha ao abrir Preferences. Usando padrao.");
    return;
  }

  cfgWifiSsid = prefs.isKey("ssid") ? prefs.getString("ssid") : DEFAULT_WIFI_SSID;
  cfgWifiPass = prefs.isKey("pass") ? prefs.getString("pass") : DEFAULT_WIFI_PASSWORD;
  cfgMqttHost = prefs.isKey("mqttHost") ? prefs.getString("mqttHost") : DEFAULT_MQTT_HOST;
  cfgMqttPort = prefs.isKey("mqttPort") ? prefs.getUShort("mqttPort") : DEFAULT_MQTT_PORT;
  cfgTopicBase = prefs.isKey("topic") ? prefs.getString("topic") : DEFAULT_MQTT_TOPIC;
  cfgMqttUsername = prefs.isKey("mqttUser") ? prefs.getString("mqttUser") : DEFAULT_MQTT_USERNAME;
  cfgMqttPassword = prefs.isKey("mqttPass") ? prefs.getString("mqttPass") : DEFAULT_MQTT_PASSWORD;
  cfgMqttClientId = prefs.isKey("mqttClient") ? prefs.getString("mqttClient") : DEFAULT_MQTT_CLIENT_ID;
  cfgOtaPassword = prefs.isKey("otaPass") ? prefs.getString("otaPass") : DEFAULT_OTA_PASSWORD;
  lcdDisplayEnabled = prefs.getBool("lcdOn", true);
  lcdColumns = prefs.getUChar("lcdCols", LCD_DEFAULT_COLUMNS);
  lcdRows = prefs.getUChar("lcdRows", LCD_DEFAULT_ROWS);
  lcdCustomMode = prefs.getBool("lcdCustom", LCD_DEFAULT_CUSTOM_MODE);
  lcdRemoteMode = prefs.getBool("lcdRemote", false);
  lcdRemoteKey = prefs.getString("lcdKey", "");
  lcdRemoteLabel = prefs.getString("lcdLabel", "");
  lcdRemoteUnit = prefs.getString("lcdUnit", "");
  for (uint8_t i = 0; i < 4; i++)
  {
    String key = String("lcdL") + i;
    if (prefs.isKey(key.c_str()))
      lcdCustomLines[i] = prefs.getString(key.c_str());
  }

  prefs.end();

  cfgWifiSsid.trim();
  cfgMqttHost.trim();
  cfgTopicBase.trim();
  cfgMqttUsername.trim();
  cfgMqttClientId.trim();
  cfgOtaPassword.trim();

  if (cfgTopicBase.length() == 0)
  {
    cfgTopicBase = DEFAULT_MQTT_TOPIC;
  }
  if (cfgOtaPassword.length() < 8)
    cfgOtaPassword = DEFAULT_OTA_PASSWORD;
  if (cfgMqttHost.length() == 0)
    cfgMqttHost = DEFAULT_MQTT_HOST;
  if (cfgMqttPort == 0)
    cfgMqttPort = DEFAULT_MQTT_PORT;
  if (lcdColumns < 8 || lcdColumns > 40)
    lcdColumns = LCD_DEFAULT_COLUMNS;
  if (lcdRows < 1 || lcdRows > 4)
    lcdRows = LCD_DEFAULT_ROWS;

  while (cfgTopicBase.endsWith("/"))
  {
    cfgTopicBase.remove(cfgTopicBase.length() - 1);
  }

  Serial.printf("[CFG] MQTT: %s:%u\n", cfgMqttHost.c_str(), cfgMqttPort);
  Serial.printf("[CFG] Topico: %s\n", cfgTopicBase.c_str());
  Serial.printf("[CFG] Usuario MQTT: %s\n", cfgMqttUsername.length() ? cfgMqttUsername.c_str() : "(sem autenticacao)");
  Serial.printf("[CFG] Client ID: %s\n", cfgMqttClientId.length() ? cfgMqttClientId.c_str() : "(automatico)");
}

bool saveConfig(
    const String &ssid,
    const String &pass,
    const String &mqttHost,
    uint16_t mqttPort,
    const String &topic,
    const String &mqttUsername,
    const String &mqttPassword,
    const String &mqttClientId,
    const String &otaPassword)
{
  String savedHost = mqttHost.length() ? mqttHost : DEFAULT_MQTT_HOST;
  uint16_t savedPort = mqttPort ? mqttPort : DEFAULT_MQTT_PORT;
  String savedTopic = topic.length() ? topic : DEFAULT_MQTT_TOPIC;
  String savedOta = otaPassword.length() >= 8 ? otaPassword : cfgOtaPassword;

  if (!prefs.begin(PREFERENCES_NAMESPACE, false))
  {
    Serial.println("[CFG] ERRO: nao foi possivel abrir a memoria para gravacao.");
    return false;
  }

  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putString("mqttHost", savedHost);
  prefs.putUShort("mqttPort", savedPort);
  prefs.putString("topic", savedTopic);
  prefs.putString("mqttUser", mqttUsername);
  prefs.putString("mqttPass", mqttPassword);
  prefs.putString("mqttClient", mqttClientId);
  if (otaPassword.length() >= 8)
    prefs.putString("otaPass", savedOta);

  // Confirma na propria memoria antes de informar sucesso ao painel.
  bool ok =
      prefs.getString("ssid", "") == ssid &&
      prefs.getString("pass", "") == pass &&
      prefs.getString("mqttHost", "") == savedHost &&
      prefs.getUShort("mqttPort", 0) == savedPort &&
      prefs.getString("topic", "") == savedTopic &&
      prefs.getString("mqttUser", "") == mqttUsername &&
      prefs.getString("mqttPass", "") == mqttPassword &&
      prefs.getString("mqttClient", "") == mqttClientId;

  if (otaPassword.length() >= 8)
    ok = ok && prefs.getString("otaPass", "") == savedOta;

  prefs.end();

  if (!ok)
  {
    Serial.println("[CFG] ERRO: verificacao da memoria falhou. Configuracao nao confirmada.");
    return false;
  }

  // Mantem RAM e memoria sincronizadas ate o reinicio.
  cfgWifiSsid = ssid;
  cfgWifiPass = pass;
  cfgMqttHost = savedHost;
  cfgMqttPort = savedPort;
  cfgTopicBase = savedTopic;
  cfgMqttUsername = mqttUsername;
  cfgMqttPassword = mqttPassword;
  cfgMqttClientId = mqttClientId;
  cfgOtaPassword = savedOta;

  Serial.println("[CFG] Configuracao gravada e verificada com sucesso.");
  return true;
}

bool saveLcdConfig(bool enabled, uint8_t columns, uint8_t rows, bool customMode, String lines[4])
{
  if (columns < 8 || columns > 40)
    columns = LCD_DEFAULT_COLUMNS;
  if (rows < 1 || rows > 4)
    rows = LCD_DEFAULT_ROWS;

  for (uint8_t i = 0; i < 4; i++)
  {
    if (lines[i].length() > 96)
      lines[i].remove(96);
  }

  if (!prefs.begin(PREFERENCES_NAMESPACE, false))
  {
    Serial.println("[CFG] ERRO: memoria indisponivel para salvar LCD.");
    return false;
  }

  prefs.putBool("lcdOn", enabled);
  prefs.putUChar("lcdCols", columns);
  prefs.putUChar("lcdRows", rows);
  prefs.putBool("lcdCustom", customMode);
  for (uint8_t i = 0; i < 4; i++)
    prefs.putString((String("lcdL") + i).c_str(), lines[i]);

  bool ok =
      prefs.getBool("lcdOn", !enabled) == enabled &&
      prefs.getUChar("lcdCols", 0) == columns &&
      prefs.getUChar("lcdRows", 0) == rows &&
      prefs.getBool("lcdCustom", !customMode) == customMode;
  for (uint8_t i = 0; i < 4; i++)
    ok = ok && prefs.getString((String("lcdL") + i).c_str(), "") == lines[i];

  prefs.end();

  if (!ok)
  {
    Serial.println("[CFG] ERRO: verificacao da configuracao LCD falhou.");
    return false;
  }

  lcdColumns = columns;
  lcdRows = rows;
  lcdCustomMode = customMode;

  for (uint8_t i = 0; i < 4; i++)
    lcdCustomLines[i] = lines[i];

  for (uint8_t i = 0; i < 4; i++)
    lastLcdLines[i] = "";

  lcdSetEnabled(enabled);
  lastLcdUpdateMs = 0;
  Serial.println("[CFG] LCD gravado e verificado com sucesso.");
  return true;
}

bool saveLcdRemoteConfig(bool remoteMode, const String &key, const String &label, const String &unit)
{
  if (!prefs.begin(PREFERENCES_NAMESPACE, false))
  {
    Serial.println("[CFG] ERRO: memoria indisponivel para salvar sensor remoto do LCD.");
    return false;
  }

  prefs.putBool("lcdRemote", remoteMode);
  prefs.putString("lcdKey", key);
  prefs.putString("lcdLabel", label);
  prefs.putString("lcdUnit", unit);

  bool ok =
      prefs.getBool("lcdRemote", !remoteMode) == remoteMode &&
      prefs.getString("lcdKey", "") == key &&
      prefs.getString("lcdLabel", "") == label &&
      prefs.getString("lcdUnit", "") == unit;
  prefs.end();

  if (!ok)
  {
    Serial.println("[CFG] ERRO: verificacao do sensor remoto do LCD falhou.");
    return false;
  }

  lcdRemoteMode = remoteMode;
  lcdRemoteKey = key;
  lcdRemoteLabel = label;
  lcdRemoteUnit = unit;
  lastLcdUpdateMs = 0;
  Serial.println("[CFG] Sensor remoto do LCD gravado e verificado.");
  return true;
}

// ---------------------------------------------------------------------------
// WIFI / AP
// ---------------------------------------------------------------------------
void stopConfigAp(const char *reason)
{
  if (!configApActive)
    return;

  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  configApActive = false;
  configApLastClientMs = 0;
  configApLastStationCount = 0;

  if (WiFi.status() == WL_CONNECTED)
    WiFi.mode(WIFI_STA);

  Serial.printf("[AP] Fechado%s%s.\n", reason ? ": " : "", reason ? reason : "");
}

void startConfigAp()
{
  if (configApActive)
    return;

  String apName = String(CONFIG_AP_PREFIX) + chipSuffix();

  // AP+STA mantem a tentativa de conexao normal enquanto libera a configuracao.
  WiFi.mode(WIFI_AP_STA);

  if (WiFi.softAP(apName.c_str(), CONFIG_AP_PASSWORD))
  {
    configApActive = true;
    configApLastClientMs = millis();
    configApLastStationCount = 0;

    IPAddress ip = WiFi.softAPIP();
    dnsServer.start(53, "*", ip);

    Serial.println();
    Serial.printf("[AP] SSID: %s\n", apName.c_str());
    Serial.printf("[AP] Senha: %s\n", CONFIG_AP_PASSWORD);
    Serial.printf("[AP] Configuracao: http://%s\n", ip.toString().c_str());
    Serial.printf("[AP] Fechamento automatico: %lu s sem cliente.\n", (unsigned long)(CONFIG_AP_IDLE_TIMEOUT_MS / 1000UL));
  }
  else
  {
    Serial.println("[AP] ERRO: nao foi possivel iniciar o ponto de acesso.");
  }
}

void maintainConfigAp()
{
  if (!configApActive)
    return;

  uint32_t now = millis();
  uint8_t stations = (uint8_t)WiFi.softAPgetStationNum();

  if (stations > 0)
  {
    // Enquanto houver alguem conectado, o temporizador fica renovado.
    configApLastClientMs = now;
    if (stations != configApLastStationCount)
      Serial.printf("[AP] Clientes conectados: %u\n", stations);
  }
  else if ((uint32_t)(now - configApLastClientMs) >= CONFIG_AP_IDLE_TIMEOUT_MS)
  {
    stopConfigAp("5 minutos sem cliente conectado");
    return;
  }

  configApLastStationCount = stations;
}

void beginWifi()
{
  WiFi.mode(WIFI_STA);
  // Reconexao manual: a busca automatica do driver troca de canal e
  // atrapalharia o ESP-NOW. Ver maintainWifi().
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);

  if (cfgWifiSsid.length() == 0)
  {
    Serial.println("[WIFI] Nenhuma rede salva. Segure MENU/RESET por 30 s para abrir o AP.");
    return;
  }

  if (WIFI_USE_STATIC_IP)
  {
    IPAddress ip, gateway, subnet, dns;
    if (ip.fromString(WIFI_STATIC_IP) && gateway.fromString(WIFI_GATEWAY) && subnet.fromString(WIFI_SUBNET) && dns.fromString(WIFI_DNS))
      WiFi.config(ip, gateway, subnet, dns);
  }

  Serial.printf("[WIFI] Conectando em: %s\n", cfgWifiSsid.c_str());
  WiFi.begin(cfgWifiSsid.c_str(), cfgWifiPass.c_str());
  lastWifiAttemptMs = millis();
}

void maintainWifi()
{
  if (wifiScanActive)
    return;
  static bool announcedConnected = false;

  if (WiFi.status() == WL_CONNECTED)
  {
    if (!announcedConnected)
    {
      announcedConnected = true;
      Serial.printf("[WIFI] Conectado. IP: %s | RSSI: %d dBm\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }
    return;
  }

  announcedConnected = false;

  // O AP NAO abre mais sozinho por falha do Wi-Fi.
  // Ele so e aberto pelo botao segurado por 30 s (ou comando manual da API).
  // Com ESP-NOW funcionando o Wi-Fi e so reserva: tenta com menos frequencia.
  uint32_t retryMs = freshSince(lastEspNowStatusMs) ? 120000UL : WIFI_RETRY_MS;
  if (DmkLink::pairing())
    return;
  if (cfgWifiSsid.length() > 0 &&
      (uint32_t)(millis() - lastWifiAttemptMs) >= retryMs)
  {
    lastWifiAttemptMs = millis();
    Serial.println("[WIFI] Nova tentativa em segundo plano...");
    WiFi.disconnect();
    WiFi.begin(cfgWifiSsid.c_str(), cfgWifiPass.c_str());
  }
}

// Rotas que alteram algo exigem login (admin + senha do painel), exceto
// para quem esta no AP de configuracao (aberto so pelo botao fisico).
bool requireWebAuth()
{
  if (configApActive && server.client().localIP() == WiFi.softAPIP())
    return true;
  if (server.authenticate(DMK_WEB_USER, cfgOtaPassword.c_str()))
    return true;
  server.requestAuthentication(BASIC_AUTH, "DMK Smart");
  return false;
}

bool requireOtaAuth()
{
  if(MqttOta::busy()){server.send(409,"text/plain","OTA em andamento");return false;}
  return requireWebAuth();
}

bool mqttPublishPumpCommand(bool turnOn);
bool mqttPublishLightCommand(bool turnOn);
bool sendControlCommand(uint8_t target, bool turnOn);
void startPairing(uint32_t seconds);

bool requestFromAuthorizedUpload()
{
  if (configApActive && server.client().localIP() == WiFi.softAPIP())
    return true;
  return server.authenticate(DMK_WEB_USER, cfgOtaPassword.c_str());
}

// Paginas do painel embutidas no firmware (gzip), geradas de data/controle.
bool sendWebAsset(const String &path, bool noCache = true)
{
  const DmkWebAsset *asset = dmkFindWebAsset(path);
  if (!asset)
    return false;
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", noCache ? "no-store" : "public, max-age=86400");
  server.send_P(200, asset->mime, (PGM_P)asset->data, asset->len);
  return true;
}

void sendIndexPage()
{
  if (!sendWebAsset("/index.html"))
    server.send(500, "text/plain; charset=utf-8", "Painel nao embutido no firmware.");
}

void sendAppConfig()
{
  // Personalizacao opcional: um app-config.js gravado no LittleFS tem prioridade.
  if (LittleFS.exists("/app-config.js"))
  {
    File file = LittleFS.open("/app-config.js", "r");
    if (file)
    {
      server.streamFile(file, "application/javascript; charset=utf-8");
      file.close();
      return;
    }
  }
  if (!sendWebAsset("/app-config.js"))
    server.send(404, "text/plain", "app-config.js nao encontrado");
}

String statusJson()
{
  String j;
  j.reserve(1300);
  j += '{';
  j += "\"version\":\"" + String(FIRMWARE_VERSION) + "\",";
  j += "\"device_name\":\"" + jsonEscape(deviceName) + "\",";
  j += "\"chip_id\":\"" + chipSuffix() + "\",";
  j += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  j += "\"wifi_ssid\":\"" + jsonEscape(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : cfgWifiSsid) + "\",";
  j += "\"wifi_rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -127) + ',';
  j += "\"ap_active\":" + String(configApActive ? "true" : "false") + ',';
  j += "\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\",";
  j += "\"ap_clients\":" + String(configApActive ? WiFi.softAPgetStationNum() : 0) + ',';
  uint32_t apIdleElapsed = configApActive ? (uint32_t)(millis() - configApLastClientMs) : 0;
  uint32_t apRemaining = (configApActive && apIdleElapsed < CONFIG_AP_IDLE_TIMEOUT_MS)
                             ? (CONFIG_AP_IDLE_TIMEOUT_MS - apIdleElapsed) / 1000UL
                             : 0;
  j += "\"ap_idle_remaining_s\":" + String(apRemaining) + ',';
  j += "\"mqtt_host\":\"" + jsonEscape(cfgMqttHost) + "\",";
  j += "\"mqtt_port\":" + String(cfgMqttPort) + ',';
  j += "\"mqtt_topic\":\"" + jsonEscape(cfgTopicBase) + "\",";
  j += "\"mqtt_username\":\"" + jsonEscape(cfgMqttUsername) + "\",";
  j += "\"mqtt_has_password\":" + String(cfgMqttPassword.length() ? "true" : "false") + ',';
  j += "\"mqtt_client_id\":\"" + jsonEscape(cfgMqttClientId) + "\",";
  j += "\"lcd_columns\":" + String(lcdColumns) + ',';
  j += "\"lcd_rows\":" + String(lcdRows) + ',';
  j += "\"uptime_s\":" + String(millis() / 1000UL) + ',';
  j += "\"mac\":\"" + WiFi.macAddress() + "\",";
  j += "\"cpu_mhz\":" + String(dmkCpuMhz()) + ',';
  j += "\"heap_free\":" + String(ESP.getFreeHeap()) + ',';
  j += "\"heap_total\":" + String(dmkHeapTotal()) + ',';
  j += "\"heap_min\":" + String(dmkHeapMin()) + ',';
  j += "\"flash_size\":" + String(ESP.getFlashChipSize()) + ',';
  j += "\"psram_size\":" + String(dmkPsramSize()) + ',';
  j += "\"psram_free\":" + String(dmkPsramFree()) + ',';
  j += "\"main_version\":\"" + jsonEscape(mainFirmwareVersion) + "\",";
  j += "\"wifi\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ',';
  j += "\"mqtt\":" + String(mqttConnected ? "true" : "false") + ',';
  j += "\"main_online\":" + String(mainEspOnline ? "true" : "false") + ',';
  j += "\"telemetry_fresh\":" + String(telemetryFresh() ? "true" : "false") + ',';
  j += "\"communication_ok\":" + String(mainCommunicationOk() ? "true" : "false") + ',';
  j += "\"telemetry_age_s\":";
  j += lastTelemetryMs ? String((uint32_t)(millis() - lastTelemetryMs) / 1000UL) : "null";
  j += ',';
  j += "\"pump\":" + String(pumpOn ? "true" : "false") + ',';
  j += "\"pump_known\":" + String(pumpStateKnown ? "true" : "false") + ',';
  j += "\"pump_pending\":" + String(commandPending ? "true" : "false") + ',';
  j += "\"can_pump\":" + String(canControlPump() ? "true" : "false") + ',';
  j += "\"light\":" + String(lightOn ? "true" : "false") + ',';
  j += "\"light_known\":" + String(lightStateKnown ? "true" : "false") + ',';
  j += "\"light_pending\":" + String(lightCommandPending ? "true" : "false") + ',';
  j += "\"can_light\":" + String(canControlLight() ? "true" : "false") + ',';
  j += "\"alarm\":" + String(alarmActive ? "true" : "false") + ',';
  j += "\"vR\":" + String(voltageR, 1) + ',';
  j += "\"vS\":" + String(voltageS, 1) + ',';
  j += "\"vT\":" + String(voltageT, 1) + ',';
  j += "\"current_a\":" + String(currentA, 2) + ',';
  j += "\"temperature_c\":" + String(temperatureC, 1) + ',';
  j += "\"flow_l_min\":" + String(flowLitersMinute, 2) + ',';
  j += "\"total_l\":" + String(totalLiters, 2) + ',';
  j += "\"rain_pct\":" + String(rainPercent, 0) + ',';
  j += "\"voltage_min\":" + String(voltageMin, 1) + ',';
  j += "\"voltage_max\":" + String(voltageMax, 1) + ',';
  j += "\"rssi\":" + String(mainEspRssi) + ',';
  char macText[18];
  uint8_t via = activeVia();
  j += "\"link\":\"" + String(DmkLink::viaShort(via)) + "\",";
  j += "\"link_name\":\"" + String(DmkLink::viaName(via)) + "\",";
  j += "\"last_command_via\":\"" + String(DmkLink::viaShort(lastCommandVia)) + "\",";
  j += "\"paired\":" + String(servePaired ? "true" : "false") + ',';
  j += "\"pairing\":" + String(DmkLink::pairing() ? "true" : "false") + ',';
  j += "\"pair_remaining_s\":" + String(DmkLink::pairRemainingS()) + ',';
  DmkLink::macToText(serveMacAddr, macText);
  j += "\"serve_mac\":\"" + String(servePaired ? macText : "") + "\",";
  j += "\"serve_ip\":\"" + (serveIp == IPAddress(0, 0, 0, 0) ? String("") : serveIp.toString()) + "\",";
  j += "\"espnow\":" + String(DmkLink::espNowReady() ? "true" : "false") + ',';
  j += "\"channel\":" + String(DmkLink::channel()) + ',';
  j += "\"espnow_age_s\":" + (lastEspNowStatusMs ? String((millis() - lastEspNowStatusMs) / 1000UL) : String("null")) + ',';
  j += "\"lan_age_s\":" + (lastLanStatusMs ? String((millis() - lastLanStatusMs) / 1000UL) : String("null")) + ',';
  j += "\"mqtt_age_s\":" + (lastMqttTelemetryMs ? String((millis() - lastMqttTelemetryMs) / 1000UL) : String("null")) + ',';
  j += "\"main_ota\":" + String(mainOtaBusy ? "true" : "false") + ',';
  j += "\"link_message\":\"" + jsonEscape(linkMessage) + "\",";
  j += "\"default_password\":" + String(cfgOtaPassword == DEFAULT_OTA_PASSWORD ? "true" : "false") + ',';
  j += "\"target\":\"" + String(DMK_OTA_TARGET) + "\",";
  j += "\"led\":\"" + String(DmkStatusLed::describe(DmkStatusLed::current())) + "\"";
  j += '}';
  return j;
}

String wifiScanJson()
{
  // Scan assíncrono: nunca segura o loop enquanto o rádio procura redes.
  // scanComplete(): -1 = em andamento, -2 = nenhum scan iniciado/erro.
  int total = WiFi.scanComplete();

  if (!wifiScanActive)
  {
    WiFi.scanDelete();
    WiFi.scanNetworks(true, true); // async=true, mostrar redes ocultas
    wifiScanActive = true;
    return "{\"scanning\":true,\"networks\":[]}";
  }

  if (total == -1)
    return "{\"scanning\":true,\"networks\":[]}";

  if (total < 0)
  {
    wifiScanActive = false;
    WiFi.scanDelete();
    return "{\"scanning\":false,\"networks\":[]}";
  }

  String json = "{\"scanning\":false,\"networks\":[";
  bool first = true;

  for (int i = 0; i < total; i++)
  {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0)
      continue;

    if (!first)
      json += ',';
    first = false;

    json += "{\"ssid\":\"" + jsonEscape(ssid) + "\",";
    json += "\"rssi\":" + String(WiFi.RSSI(i)) + ',';
    json += "\"secure\":" + String(dmkWifiOpenEncryption(i) ? "false" : "true") + '}';
  }

  json += "]}";
  WiFi.scanDelete();
  wifiScanActive = false;
  return json;
}

String lcdConfigJson()
{
  String json = "{\"enabled\":" + String(lcdDisplayEnabled ? "true" : "false") +
                ",\"columns\":" + String(lcdColumns) +
                ",\"rows\":" + String(lcdRows) +
                ",\"custom\":" + String(lcdCustomMode ? "true" : "false") +
                ",\"max_template_chars\":96,\"lines\":[";

  for (uint8_t i = 0; i < 4; i++)
  {
    if (i)
      json += ',';
    json += "\"" + jsonEscape(lcdCustomLines[i]) + "\"";
  }

  return json + "]}";
}

String lcdRemoteJson()
{
  return "{\"enabled\":" + String(lcdRemoteMode ? "true" : "false") +
         ",\"key\":\"" + jsonEscape(lcdRemoteKey) +
         "\",\"label\":\"" + jsonEscape(lcdRemoteLabel) +
         "\",\"unit\":\"" + jsonEscape(lcdRemoteUnit) + "\"}";
}


void setupMqttOtaRoutes(){
  server.on("/ota-mqtt",HTTP_GET,[](){if(!sendWebAsset("/ota-mqtt.html"))server.send(404,"text/plain","Pagina nao embutida");});
  server.on("/api/ota-mqtt/status",HTTP_GET,[](){server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",MqttOta::statusJson());});
  server.on("/api/ota-mqtt/config",HTTP_GET,[](){if(!requireWebAuth())return;server.send(200,"application/json",RemoteOta::configJson());});
  server.on("/api/ota-mqtt/command",HTTP_POST,[](){
    if(!requireWebAuth())return;
    String json,error;bool ok=MqttOta::requestLocal(server.arg("plain"),error);
    JsonDocument ack;ack["ok"]=ok;ack["error"]=error;serializeJson(ack,json);server.send(ok?202:400,"application/json",json);
  });
  server.on("/api/pair",HTTP_POST,[](){
    if(!requireWebAuth())return;
    if(server.arg("state")=="OFF"){DmkLink::pairStop();server.send(200,"application/json","{\"message\":\"Pareamento cancelado\"}");return;}
    startPairing(DMK_PAIR_WINDOW_S);
    server.send(200,"application/json","{\"message\":\"Procurando o SERVE. No SERVE, segure o botao BOOT 3 s e solte (ou use Parear no painel dele).\"}");
  });
  server.on("/api/pair/forget",HTTP_POST,[](){
    if(!requireWebAuth())return;
    DmkLink::forget();servePaired=false;serveSession=0;
    server.send(200,"application/json","{\"message\":\"Pareamento apagado. Pareie novamente com o SERVE.\"}");
  });
}
void setupWeb()
{
  setupMqttOtaRoutes();
  server.on("/", HTTP_GET, []()
            { sendIndexPage(); });

  server.on("/api/status", HTTP_GET, []()
            {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json; charset=utf-8", statusJson()); });
  server.on("/app-config.js", HTTP_GET, []()
            { sendAppConfig(); });
  server.on("/api/lcd", HTTP_GET, []()
            { server.send(200, "application/json; charset=utf-8", lcdConfigJson()); });
  server.on("/api/lcd", HTTP_POST, []()
            {
    if (!requireWebAuth()) return;
    String lines[4];
    for (uint8_t i = 0; i < 4; i++)
      lines[i] = server.arg(String("line") + (i + 1));

    uint8_t columns = (uint8_t)server.arg("columns").toInt();
    uint8_t rows = (uint8_t)server.arg("rows").toInt();

    if (columns < 8 || columns > 40 || rows < 1 || rows > 4)
    {
      server.send(400, "application/json",
                  "{\"message\":\"Geometria do LCD invalida. Use 8-40 colunas e 1-4 linhas.\"}");
      return;
    }

    if (!saveLcdConfig(server.arg("enabled") == "true",
                       columns,
                       rows,
                       server.arg("custom") == "true",
                       lines))
    {
      server.send(500, "application/json", "{\"message\":\"Falha ao gravar ou verificar o LCD na memoria\"}");
      return;
    }

    server.send(200, "application/json", "{\"message\":\"LCD salvo e verificado na memoria\"}"); });
  server.on("/api/lcd/remote", HTTP_GET, []()
            { server.send(200, "application/json", lcdRemoteJson()); });
  server.on("/api/lcd/remote", HTTP_POST, []()
            {
    if (!requireWebAuth()) return;
    if (!saveLcdRemoteConfig(server.arg("enabled") == "true", server.arg("key"), server.arg("label"), server.arg("unit")))
    {
      server.send(500, "application/json", "{\"message\":\"Falha ao gravar ou verificar o sensor remoto do LCD\"}");
      return;
    }
    server.send(200, "application/json", "{\"message\":\"Sensor remoto salvo e verificado no LCD\"}"); });
  server.on("/api/telemetry", HTTP_GET, []()
            { server.send(200, "application/json", lastTelemetryPayload); });

  server.on("/api/wifi/scan", HTTP_GET, []()
            { server.send(200, "application/json; charset=utf-8", wifiScanJson()); });

  // Rotas usadas por Android, iPhone, Windows e navegadores para detectar
  // portal cativo. Evita erros no monitor e leva o usuário ao painel.
  auto captivePortal = []()
  {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "Abrindo painel DMK Smart");
  };
  server.on("/generate_204", HTTP_GET, captivePortal);
  server.on("/gen_204", HTTP_GET, captivePortal);
  server.on("/hotspot-detect.html", HTTP_GET, captivePortal);
  server.on("/library/test/success.html", HTTP_GET, captivePortal);
  server.on("/ncsi.txt", HTTP_GET, captivePortal);
  server.on("/connecttest.txt", HTTP_GET, captivePortal);
  server.on("/favicon.ico", HTTP_GET, []()
            { server.send(404, "text/plain", "Ícone não configurado"); });

  server.on("/api/ap", HTTP_POST, []()
            {
    if (!requireWebAuth()) return;
    String state = server.arg("state");
    state.toUpperCase();
    if (state == "ON") {
      startConfigAp();
    } else if (state == "OFF") {
      stopConfigAp("desligado pelo painel");
    } else {
      server.send(400, "application/json", "{\"message\":\"Estado inválido\"}");
      return;
    }
    server.send(200, "application/json", "{\"message\":\"Configuração AP atualizada\"}"); });

  server.on("/save", HTTP_POST, []()
            {
    if (!requireWebAuth()) return;
    String ssid  = server.arg("ssid");
    String pass  = server.arg("pass");
    String mqttHost = server.arg("mqttHost");
    uint16_t mqttPort = (uint16_t)server.arg("mqttPort").toInt();
    String topic = server.arg("topic");
    String mqttUsername = server.arg("mqttUser");
    String mqttPassword = server.arg("mqttPass");
    String mqttClientId = server.arg("mqttClient");
    String otaPassword = server.arg("otaPass");

    ssid.trim();
    mqttHost.trim();
    topic.trim();
    mqttUsername.trim();
    mqttClientId.trim();

    if (pass.length() == 0) {
      pass = cfgWifiPass;
    }
    if (mqttPassword.length() == 0 && server.arg("clearMqttPass") != "true") {
      mqttPassword = cfgMqttPassword;
    }

    if (ssid.length() == 0) {
      server.send(
        400,
        "text/plain; charset=utf-8",
        "SSID obrigatorio."
      );
      return;
    }

    if (mqttHost.length() == 0 || mqttPort == 0) {
      server.send(400, "text/plain; charset=utf-8", "Servidor MQTT e porta obrigatorios.");
      return;
    }

    if (!saveConfig(ssid, pass, mqttHost, mqttPort, topic,
                    mqttUsername, mqttPassword, mqttClientId, otaPassword))
    {
      server.send(500, "text/plain; charset=utf-8", "Falha ao gravar/verificar configuracao na memoria. O dispositivo nao sera reiniciado.");
      return;
    }

    server.send(
      200,
      "text/html; charset=utf-8",
      "<h2>Salvo.</h2><p>O dispositivo vai reiniciar.</p>"
    );

    delay(600);
    ESP.restart(); });

  server.on("/api/pump", HTTP_POST, []()
            {
    if (!requireWebAuth()) return;
    String state = server.arg("state"); state.toUpperCase();
    if (state != "ON" && state != "OFF") {
      server.send(400, "application/json", "{\"message\":\"Estado invalido\"}"); return;
    }
    if (!canControlPump()) {
      server.send(503, "application/json", "{\"message\":\"Bomba sem estado confirmado ou comunicacao indisponivel\"}"); return;
    }
    if (!sendControlCommand(DmkProto::TARGET_PUMP, state == "ON")) {
      server.send(503, "application/json", "{\"message\":\"Nenhum caminho disponivel ate o SERVE\"}"); return;
    }
    server.send(202, "application/json", "{\"message\":\"Comando enviado; aguardando confirmacao\"}"); });

  server.on("/api/light", HTTP_POST, []()
            {
    if (!requireWebAuth()) return;
    String state = server.arg("state"); state.toUpperCase();
    if (state != "ON" && state != "OFF") {
      server.send(400, "application/json", "{\"message\":\"Estado invalido\"}"); return;
    }
    if (!canControlLight()) {
      server.send(503, "application/json", "{\"message\":\"Luz sem estado confirmado ou comunicacao indisponivel\"}"); return;
    }
    if (!sendControlCommand(DmkProto::TARGET_LIGHT, state == "ON")) {
      server.send(503, "application/json", "{\"message\":\"Nenhum caminho disponivel ate o SERVE\"}"); return;
    }
    server.send(202, "application/json", "{\"message\":\"Comando enviado; aguardando confirmacao\"}"); });

  server.on("/update", HTTP_GET, []()
            {
    if (!requireOtaAuth()) return;
    sendIndexPage(); });

  server.on("/update", HTTP_POST, []()
            {
    if (!requireOtaAuth()) return;
    bool ok = otaUploadOk && !Update.hasError();
    server.send(200, "text/html; charset=utf-8",
      ok ? "<h2>Atualizacao concluida.</h2><p>O controle remoto vai reiniciar.</p>"
         : "<h2>Falha na atualizacao.</h2><p>O firmware anterior foi mantido.</p>");
    if (ok) { delay(700); ESP.restart(); } }, []()
            {
    if(MqttOta::busy()) return;
    if (!requestFromAuthorizedUpload()) return;
    HTTPUpload &upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      otaUploadOk = false;
      if (!dmkUpdateBegin()) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_END) {
      otaUploadOk = Update.end(true);
      if (!otaUploadOk) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
      dmkUpdateAbort();
    } });

  server.onNotFound([]()
                    {
    if (configApActive) {
      server.sendHeader("Location", "/", true);
      server.send(302, "text/plain", "Abrindo painel DMK Smart");
    } else {
      server.send(404, "text/plain", "Rota não encontrada");
    } });
}

void startWebIfReady()
{
  if (webStarted)
    return;

  if (WiFi.status() != WL_CONNECTED && !configApActive)
  {
    return;
  }

  server.begin();
  webStarted = true;

  Serial.println("[WEB] Servidor de configuracao ativo.");
}

// ---------------------------------------------------------------------------
// JSON SIMPLES - SEM ARDUINOJSON
// ---------------------------------------------------------------------------
bool jsonGetFloat(
    const String &json,
    const char *key,
    float &value)
{
  String search = "\"";
  search += key;
  search += "\":";

  int pos = json.indexOf(search);

  if (pos < 0)
    return false;

  pos += search.length();

  const char *start = json.c_str() + pos;
  char *endPtr = nullptr;

  float v = strtof(start, &endPtr);

  if (endPtr == start)
    return false;

  value = v;
  return true;
}

bool jsonGetInt(
    const String &json,
    const char *key,
    int &value)
{
  float temp;

  if (!jsonGetFloat(json, key, temp))
  {
    return false;
  }

  value = (int)temp;
  return true;
}

bool jsonGetBool(
    const String &json,
    const char *key,
    bool &value)
{
  String search = "\"";
  search += key;
  search += "\":";

  int pos = json.indexOf(search);

  if (pos < 0)
    return false;

  pos += search.length();

  while (pos < (int)json.length() &&
         (json[pos] == ' ' || json[pos] == '\t'))
  {
    pos++;
  }

  if (json.substring(pos, pos + 4) == "true")
  {
    value = true;
    return true;
  }

  if (json.substring(pos, pos + 5) == "false")
  {
    value = false;
    return true;
  }

  return false;
}

bool jsonGetString(const String &json, const char *key, String &value)
{
  String search = "\"";
  search += key;
  search += "\":\"";
  int pos = json.indexOf(search);
  if (pos < 0)
    return false;
  pos += search.length();
  int end = json.indexOf('"', pos);
  if (end < 0)
    return false;
  value = json.substring(pos, end);
  return true;
}

// ---------------------------------------------------------------------------
// MQTT
// ---------------------------------------------------------------------------
// Publica ON/OFF no topico de comando (caminho 3: MQTT).
bool mqttPublishRaw(const String &topic, bool turnOn)
{
  if (!mqttConnected)
    return false;
  const char *payload = turnOn ? "ON" : "OFF";
#if defined(DMK_BOARD_ESP8266)
  return mqttClient.publish(topic.c_str(), payload, false);
#else
  return esp_mqtt_client_publish(mqttClient, topic.c_str(), payload, 0, 1, 0) >= 0;
#endif
}

// Tenta um caminho especifico. true = comando saiu por ele.
bool tryCommandStage(int8_t stage)
{
  using namespace DmkProto;
  Cmd c;
  c.session = serveSession;
  c.target = pendingCmd.target;
  c.state = pendingCmd.on ? 1 : 0;
  c.seq = pendingCmd.seq;
  switch (stage)
  {
  case 0: // ESP-NOW direto ao SERVE pareado
    if (!servePaired || !DmkLink::hasKey() || !DmkLink::espNowReady() || serveSession == 0)
      return false;
    return DmkLink::send(T_CMD, &c, sizeof(c), DmkLink::VIA_ESPNOW, serveMacAddr);
  case 1: // rede local (UDP) ao IP do SERVE
    if (!servePaired || !DmkLink::udpReady() || serveIp == IPAddress(0, 0, 0, 0) || serveSession == 0)
      return false;
    return DmkLink::send(T_CMD, &c, sizeof(c), DmkLink::VIA_LAN, nullptr, serveIp);
  case 2: // MQTT (topicos da versao 1.0.0)
    return mqttPublishRaw(pendingCmd.target == TARGET_PUMP ? topicPumpSet : topicLightSet, pendingCmd.on);
  }
  return false;
}

bool advanceCommandStage()
{
  for (int8_t st = pendingCmd.stage + 1; st <= 2; st++)
  {
    if (tryCommandStage(st))
    {
      pendingCmd.stage = st;
      pendingCmd.stageAt = millis();
      lastCommandVia = st == 0 ? DmkLink::VIA_ESPNOW : st == 1 ? DmkLink::VIA_LAN : DmkLink::VIA_MQTT;
      Serial.printf("[CMD] %s %s enviado via %s (seq %u)\n",
                    pendingCmd.target == DmkProto::TARGET_PUMP ? "BOMBA" : "LUZ",
                    pendingCmd.on ? "ON" : "OFF", DmkLink::viaName(lastCommandVia), pendingCmd.seq);
      return true;
    }
  }
  pendingCmd.active = false;
  return false;
}

// Envia liga/desliga pelo melhor caminho e cai para o proximo se nao houver
// confirmacao: ESP-NOW (350 ms) -> rede local (700 ms) -> MQTT.
bool sendControlCommand(uint8_t target, bool turnOn)
{
  pendingCmd.active = true;
  pendingCmd.target = target;
  pendingCmd.on = turnOn;
  pendingCmd.seq = ++commandSeq;
  pendingCmd.stage = -1;
  pendingCmd.acked = false;
  if (!advanceCommandStage())
  {
    Serial.println("[CMD] Nenhum caminho disponivel ate o SERVE.");
    linkMessage = "SEM CAMINHO";
    return false;
  }
  if (target == DmkProto::TARGET_PUMP)
  {
    commandPending = true;
    pendingTargetOn = turnOn;
    commandSentMs = millis();
  }
  else
  {
    lightCommandPending = true;
    pendingLightTargetOn = turnOn;
    lightCommandSentMs = millis();
  }
  linkMessage = "";
  return true;
}

bool mqttPublishPumpCommand(bool turnOn) { return sendControlCommand(DmkProto::TARGET_PUMP, turnOn); }
bool mqttPublishLightCommand(bool turnOn) { return sendControlCommand(DmkProto::TARGET_LIGHT, turnOn); }

void maintainCommandRoute()
{
  if (!pendingCmd.active || pendingCmd.acked)
    return;
  uint32_t wait = pendingCmd.stage == 0 ? 350 : pendingCmd.stage == 1 ? 700 : 0;
  if (wait == 0)
  {
    pendingCmd.active = false; // MQTT: a confirmacao vem pela telemetria
    return;
  }
  if ((uint32_t)(millis() - pendingCmd.stageAt) >= wait)
    advanceCommandStage();
}

void queueLcdStateChange(uint8_t device, bool on)
{
  LcdStateEvent event = {device, on};
  if (!lcdQueueSend(event))
  {
    Serial.println("[LCD] Fila de avisos cheia; evento descartado.");
  }
}

void handleTelemetryPayload(const String &payload, bool fromMqtt = true)
{
  // O JSON MQTT tem algumas chaves extras (relogio etc.). Quando MQTT esta
  // ativo ele continua sendo a fonte das variaveis do LCD; ESP-NOW/rede local
  // atualizam os valores principais sem apagar essas chaves.
  if (fromMqtt)
  {
    lastMqttTelemetryMs = millis();
    lastTelemetryPayload = payload;
  }
  else if (!freshSince(lastMqttTelemetryMs))
    lastTelemetryPayload = payload;
  jsonGetFloat(payload, "vR", voltageR);
  jsonGetFloat(payload, "vS", voltageS);
  jsonGetFloat(payload, "vT", voltageT);

  jsonGetFloat(payload, "vmin", voltageMin);
  jsonGetFloat(payload, "vmax", voltageMax);

  jsonGetInt(payload, "rssi", mainEspRssi);
  jsonGetFloat(payload, "current_a", currentA);
  jsonGetFloat(payload, "temperature_c", temperatureC);
  jsonGetFloat(payload, "flow_l_min", flowLitersMinute);
  jsonGetFloat(payload, "total_l", totalLiters);
  jsonGetFloat(payload, "rain_pct", rainPercent);
  String receivedFirmware;
  if (jsonGetString(payload, "firmware", receivedFirmware))
  {
    receivedFirmware.toCharArray(mainFirmwareVersion, sizeof(mainFirmwareVersion));
  }

  bool receivedAlarm;
  if (jsonGetBool(payload, "alarm", receivedAlarm))
    alarmActive = receivedAlarm;
  bool receivedLight;
  if (jsonGetBool(payload, "light", receivedLight))
  {
    if (lightStateKnown && lightOn != receivedLight)
    {
      queueLcdStateChange(2, receivedLight);
    }
    lightOn = receivedLight;
    lightStateKnown = true;
    if (lightCommandPending && lightOn == pendingLightTargetOn)
      lightCommandPending = false;
  }

  bool receivedPump;

  if (jsonGetBool(payload, "pump", receivedPump))
  {
    if (pumpStateKnown && pumpOn != receivedPump)
    {
      queueLcdStateChange(1, receivedPump);
    }
    pumpOn = receivedPump;
    pumpStateKnown = true;
    if (commandPending && pumpOn == pendingTargetOn)
      commandPending = false;
  }

  // Qualquer telemetria valida recebida confirma presença do ESP principal.
  mainEspOnline = true;
  lastTelemetryMs = millis();
}


// ---------------------------------------------------------------------------
// ESP-NOW / REDE LOCAL: estado recebido do SERVE
// ---------------------------------------------------------------------------
String statusToJson(const DmkProto::Status &st)
{
  using namespace DmkProto;
  char fw[13], reason[21], name[25];
  memcpy(fw, st.fw, 12); fw[12] = 0;
  memcpy(reason, st.reason, 20); reason[20] = 0;
  memcpy(name, st.name, 24); name[24] = 0;
  String j;
  j.reserve(640);
  j += "{\"vR\":" + String(st.vR10 / 10.0f, 1);
  j += ",\"vS\":" + String(st.vS10 / 10.0f, 1);
  j += ",\"vT\":" + String(st.vT10 / 10.0f, 1);
  j += ",\"adcVrmsR\":" + String(st.adcR / 10000.0f, 4);
  j += ",\"adcVrmsS\":" + String(st.adcS / 10000.0f, 4);
  j += ",\"adcVrmsT\":" + String(st.adcT / 10000.0f, 4);
  j += ",\"current_a\":" + String(st.current_mA / 1000.0f, 3);
  j += ",\"temperature_c\":" + String(st.temp10 / 10.0f, 1);
  j += ",\"flow_l_min\":" + String(st.flow_mLmin / 1000.0f, 3);
  j += ",\"total_l\":" + String(st.total_cL / 100.0f, 2);
  j += ",\"rain_pct\":" + String(st.rain);
  j += ",\"alarm\":" + String((st.flags & SF_ALARM) ? "true" : "false");
  j += ",\"light\":" + String((st.flags & SF_LIGHT) ? "true" : "false");
  j += ",\"firmware\":\"" + jsonEscape(fw) + "\"";
  j += ",\"pump\":" + String((st.flags & SF_PUMP) ? "true" : "false");
  j += ",\"protection_enabled\":" + String((st.flags & SF_PROT_ON) ? "true" : "false");
  j += ",\"protection_ok\":" + String((st.flags & SF_PROT_OK) ? "true" : "false");
  j += ",\"protection_reason\":\"" + jsonEscape(reason) + "\"";
  j += ",\"clock_valid\":" + String((st.flags & SF_CLOCK) ? "true" : "false");
  j += ",\"vmin\":" + String(st.vmin10 / 10.0f, 1);
  j += ",\"vmax\":" + String(st.vmax10 / 10.0f, 1);
  j += ",\"rssi\":" + String(st.rssi);
  j += ",\"uptime_s\":" + String(st.uptime);
  j += ",\"serve_name\":\"" + jsonEscape(name) + "\"";
  j += ",\"serve_mqtt\":" + String((st.flags & SF_MQTT) ? "true" : "false");
  j += "}";
  return j;
}

void onLinkFrame(const DmkLink::Rx &rx)
{
  using namespace DmkProto;
  if (!servePaired || memcmp(rx.mac, serveMacAddr, 6) != 0)
    return; // somente o SERVE pareado
  if (rx.type == T_STATUS && rx.len >= sizeof(Status))
  {
    Status st;
    memcpy(&st, rx.data, sizeof(st));
    serveSession = st.session;
    mainOtaBusy = st.otaBusy != 0;
    if (st.ip[0] != 0)
      serveIp = IPAddress(st.ip[0], st.ip[1], st.ip[2], st.ip[3]);
    if (rx.via == DmkLink::VIA_ESPNOW)
    {
      lastEspNowStatusMs = millis();
      uint8_t ch = DmkLink::channel();
      if (ch && ch != DmkLink::savedChannel())
        DmkLink::saveChannel(ch);
    }
    else
    {
      lastLanStatusMs = millis();
      serveIp = rx.ip;
    }
    handleTelemetryPayload(statusToJson(st), false);
    return;
  }
  if (rx.type == T_ACK && rx.len >= sizeof(Ack))
  {
    Ack a;
    memcpy(&a, rx.data, sizeof(a));
    if (!pendingCmd.active || a.seq != pendingCmd.seq)
      return;
    if (a.result == ACK_OK)
    {
      pendingCmd.acked = true;
      pendingCmd.ackVia = rx.via;
      pendingCmd.active = false;
    }
    else if (a.result == ACK_BUSY)
    {
      pendingCmd.active = false;
      commandPending = lightCommandPending = false;
      linkMessage = "SERVE OCUPADO";
      Serial.println("[CMD] SERVE ocupado (atualizando firmware). Comando recusado.");
    }
    else
      advanceCommandStage(); // sessao antiga: tenta o proximo caminho
  }
}

void startPairing(uint32_t seconds)
{
  if (DmkLink::pairing())
    return;
  if (DmkLink::pairStart(seconds))
  {
    lastLcdUpdateMs = 0;
    linkMessage = "PAREANDO";
  }
}

void onPaired(const uint8_t mac[6], uint8_t channel, const uint8_t ip[4], const char *serveName, const String &cfgJson)
{
  memcpy(serveMacAddr, mac, 6);
  servePaired = true;
  if (ip[0])
    serveIp = IPAddress(ip[0], ip[1], ip[2], ip[3]);
  (void)channel;
  linkMessage = "PAREADO";
  lastLcdUpdateMs = 0;
  if (cfgJson.length() == 0)
    return;
  // O SERVE enviou Wi-Fi + MQTT (cifrados): grava e reinicia ja configurado.
  JsonDocument doc;
  if (deserializeJson(doc, cfgJson))
    return;
  String ssid = doc["s"] | "";
  if (ssid.length() == 0)
    return;
  String topic = doc["t"] | "";
  uint16_t port = doc["o"] | DEFAULT_MQTT_PORT;
  if (saveConfig(ssid, doc["p"] | "", doc["h"] | DEFAULT_MQTT_HOST, port, topic,
                 doc["u"] | "", doc["w"] | "", cfgMqttClientId, ""))
  {
    Serial.printf("[PAREAR] Wi-Fi '%s' e MQTT recebidos de '%s'. Reiniciando...\n", ssid.c_str(), serveName);
    restartAtMs = millis() + 2500;
  }
}

void maintainLink()
{
  DmkLink::loop();
  if (DmkLink::pairing() || !servePaired)
    return;
  uint32_t now = millis();
  bool espFresh = freshSince(lastEspNowStatusMs);
  if (espFresh || (uint32_t)(now - lastPingMs) < 1500)
    return;
  lastPingMs = now;
  // Sem Wi-Fi conectado o canal do radio esta livre: procura o SERVE
  // comecando pelo canal salvo no pareamento.
  if (WiFi.status() != WL_CONNECTED)
  {
    if (lastEspNowStatusMs == 0 || (uint32_t)(now - lastEspNowStatusMs) > 9000)
    {
      if (sweepChannel == 0)
        sweepChannel = DmkLink::savedChannel() ? DmkLink::savedChannel() : 1;
      else
        sweepChannel = (uint8_t)(sweepChannel % 13 + 1);
      DmkLink::setChannel(sweepChannel);
    }
  }
  else
    sweepChannel = 0;
  DmkLink::send(DmkProto::T_PING, nullptr, 0, DmkLink::VIA_ESPNOW, serveMacAddr);
  if (DmkLink::udpReady() && serveIp != IPAddress(0, 0, 0, 0))
    DmkLink::send(DmkProto::T_PING, nullptr, 0, DmkLink::VIA_LAN, nullptr, serveIp);
}

void updateStatusLed()
{
  using namespace DmkStatusLed;
  if (MqttOta::busy())
    set(LED_OTA);
  else if (DmkLink::pairing())
    set(LED_FAST);
  else if (configApActive)
    set(LED_AP);
  else
  {
    switch (activeVia())
    {
    case DmkLink::VIA_ESPNOW: set(LED_ON); break;
    case DmkLink::VIA_LAN: set(LED_BLIP1); break;
    case DmkLink::VIA_MQTT: set(LED_BLIP2); break;
    default: set(LED_SLOW); break;
    }
  }
  DmkStatusLed::loop();
}

void handleIncomingMqtt(const String &topic, String payload)
{
  if (topic == String(topicTelemetry.c_str()))
  {
    handleTelemetryPayload(payload);
  }
  else if (topic == String(topicPumpState.c_str()))
  {
    String value = payload;
    value.trim();
    value.toUpperCase();

    if (value == "ON" || value == "OFF")
    {
      bool nextState = (value == "ON");
      if (pumpStateKnown && pumpOn != nextState)
        queueLcdStateChange(1, nextState);

      pumpOn = nextState;
      pumpStateKnown = true;
    }

    if (commandPending && pumpStateKnown && pumpOn == pendingTargetOn)
    {
      commandPending = false;
      Serial.println("[MQTT] Estado confirmado pelo ESP principal.");
    }
  }
  else if (topic == String(topicAvailability.c_str()))
  {
    payload.trim();
    payload.toLowerCase();

    if (payload == "online")
      mainEspOnline = true;
    else if (payload == "offline" && !telemetryFresh())
      mainEspOnline = false;

    Serial.printf("[MQTT] ESP principal: %s\n",
                  mainCommunicationOk() ? "ONLINE" : "OFFLINE");
  }
  else if (topic == String(topicLightState.c_str()))
  {
    payload.trim();
    payload.toUpperCase();

    if (payload == "ON" || payload == "OFF")
    {
      bool nextState = (payload == "ON");
      if (lightStateKnown && lightOn != nextState)
        queueLcdStateChange(2, nextState);

      lightOn = nextState;
      lightStateKnown = true;

      if (lightCommandPending && lightOn == pendingLightTargetOn)
      {
        lightCommandPending = false;
        Serial.println("[MQTT] Luz confirmada pelo ESP principal.");
      }
    }
  }
}

#if defined(DMK_BOARD_ESP8266)

void mqttCallback(char *topic, uint8_t *payload, unsigned int length)
{
  String data;
  data.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++)
    data += (char)payload[i];

  handleIncomingMqtt(String(topic), data);
}

bool connectMqttEsp8266()
{
  if (WiFi.status() != WL_CONNECTED)
    return false;

  static String clientId;
  clientId = cfgMqttClientId.length() ? cfgMqttClientId : (deviceName + "-" + chipSuffix());

  Serial.printf("[MQTT] Conectando em %s:%u | client=%s | auth=%s...\n",
                cfgMqttHost.c_str(), cfgMqttPort, clientId.c_str(),
                cfgMqttUsername.length() ? "usuario/senha" : "sem usuario");

  bool connected = false;
  if (cfgMqttUsername.length())
    connected = mqttClient.connect(clientId.c_str(),
                                   cfgMqttUsername.c_str(),
                                   cfgMqttPassword.c_str());
  else
    connected = mqttClient.connect(clientId.c_str());

  if (!connected)
  {
    mqttConnected = false;
    Serial.printf("[MQTT] Falha rc=%d\n", mqttClient.state());
    return false;
  }

  mqttConnected = true;
  mqttClient.subscribe(topicTelemetry.c_str());
  mqttClient.subscribe(topicPumpState.c_str());
  mqttClient.subscribe(topicAvailability.c_str());
  mqttClient.subscribe(topicLightState.c_str());

  Serial.println("[MQTT] Conectado ao broker.");
  return true;
}

void setupMqtt()
{
  buildTopics();

  Serial.println("[MQTT] Preparando PubSubClient (ESP8266)...");
  Serial.printf("[MQTT] Topico base: %s\n", cfgTopicBase.c_str());

  mqttClient.setServer(cfgMqttHost.c_str(), cfgMqttPort);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(30);
  // Evita que uma tentativa de broker indisponivel segure o loop por muito tempo.
  mqttClient.setSocketTimeout(2);
  mqttClient.setBufferSize(2048);

  mqttStarted = true;
  lastMqttReconnectMs = 0;
}

void maintainMqtt()
{
  if (!mqttStarted || WiFi.status() != WL_CONNECTED)
  {
    mqttConnected = false;
    return;
  }

  if (!mqttClient.connected())
  {
    mqttConnected = false;
    if (!telemetryFresh())
      mainEspOnline = false;

    uint32_t now = millis();
    // Reconectar sem martelar o broker. Enquanto isso web/LCD/botoes continuam ativos.
    if ((uint32_t)(now - lastMqttReconnectMs) < 10000)
      return;

    lastMqttReconnectMs = now;
    connectMqttEsp8266();
  }

  if (mqttClient.connected())
  {
    mqttConnected = true;
    mqttClient.loop();
  }
}

#else

void mqttEventHandler(
    void *handlerArgs,
    esp_event_base_t base,
    int32_t eventId,
    void *eventData)
{
  (void)handlerArgs;
  (void)base;

  esp_mqtt_event_handle_t event =
      static_cast<esp_mqtt_event_handle_t>(eventData);

  switch ((esp_mqtt_event_id_t)eventId)
  {
  case MQTT_EVENT_CONNECTED:
    mqttConnected = true;

    Serial.println("[MQTT] Conectado ao broker.");

    esp_mqtt_client_subscribe(event->client, topicTelemetry.c_str(), 0);
    esp_mqtt_client_subscribe(event->client, topicPumpState.c_str(), 0);
    esp_mqtt_client_subscribe(event->client, topicAvailability.c_str(), 0);
    esp_mqtt_client_subscribe(event->client, topicLightState.c_str(), 0);
    break;

  case MQTT_EVENT_DISCONNECTED:
    mqttConnected = false;
    if (!telemetryFresh())
      mainEspOnline = false;
    Serial.println("[MQTT] Desconectado.");
    break;

  case MQTT_EVENT_DATA:
  {
    String topic;
    topic.reserve(event->topic_len + 1);
    for (int i = 0; i < event->topic_len; i++)
      topic += event->topic[i];

    String payload;
    payload.reserve(event->data_len + 1);
    for (int i = 0; i < event->data_len; i++)
      payload += event->data[i];

    handleIncomingMqtt(topic, payload);
    break;
  }

  case MQTT_EVENT_ERROR:
    mqttConnected = false;
    Serial.println("[MQTT] Evento de erro.");
    break;

  default:
    break;
  }
}

void setupMqtt()
{
  buildTopics();

  Serial.println("[MQTT] Preparando cliente nativo...");
  Serial.printf("[MQTT] Topico base: %s\n", cfgTopicBase.c_str());

  esp_mqtt_client_config_t mqttCfg = {};

  static String clientId;
  clientId = cfgMqttClientId.length() ? cfgMqttClientId : (deviceName + "-" + chipSuffix());
  static String mqttUri;
  mqttUri = "mqtt://" + cfgMqttHost + ":" + String(cfgMqttPort);
  Serial.printf("[MQTT] URI: %s\n", mqttUri.c_str());

#if ESP_IDF_VERSION_MAJOR >= 5
  mqttCfg.broker.address.uri = mqttUri.c_str();
  mqttCfg.credentials.client_id = clientId.c_str();
  mqttCfg.credentials.username = cfgMqttUsername.length() ? cfgMqttUsername.c_str() : nullptr;
  mqttCfg.credentials.authentication.password = cfgMqttUsername.length() ? cfgMqttPassword.c_str() : nullptr;
  mqttCfg.session.keepalive = 30;
  mqttCfg.buffer.size = 2048;
  mqttCfg.buffer.out_size = 2048;
#else
  mqttCfg.uri = mqttUri.c_str();
  mqttCfg.client_id = clientId.c_str();
  mqttCfg.username = cfgMqttUsername.length() ? cfgMqttUsername.c_str() : nullptr;
  mqttCfg.password = cfgMqttUsername.length() ? cfgMqttPassword.c_str() : nullptr;
  mqttCfg.keepalive = 30;
  mqttCfg.buffer_size = 2048;
#endif

  mqttClient = esp_mqtt_client_init(&mqttCfg);

  if (mqttClient == nullptr)
  {
    Serial.println("[MQTT] ERRO: init retornou NULL.");
    return;
  }

  esp_err_t regResult = esp_mqtt_client_register_event(
      mqttClient,
      MQTT_EVENT_ANY,
      mqttEventHandler,
      nullptr);

  if (regResult != ESP_OK)
  {
    Serial.printf("[MQTT] ERRO evento: %s\n",
                  esp_err_to_name(regResult));

    esp_mqtt_client_destroy(mqttClient);
    mqttClient = nullptr;
    return;
  }

  esp_err_t startResult = esp_mqtt_client_start(mqttClient);

  if (startResult == ESP_OK)
  {
    mqttStarted = true;
    Serial.printf("[MQTT] Cliente iniciado: %s\n", clientId.c_str());
  }
  else
  {
    Serial.printf("[MQTT] ERRO start: %s\n",
                  esp_err_to_name(startResult));

    esp_mqtt_client_destroy(mqttClient);
    mqttClient = nullptr;
  }
}

void maintainMqtt()
{
  // esp-mqtt gerencia reconexao internamente no ESP32.
}

#endif

void startMqttIfReady()
{
  if (!mqttStarted)
  {
    if (WiFi.status() != WL_CONNECTED)
      return;
    setupMqtt();
  }

  maintainMqtt();
}

// ---------------------------------------------------------------------------
// BOTAO
// ---------------------------------------------------------------------------
bool buttonPressed(DebouncedButton &button)
{
  bool reading = digitalRead(button.pin);
  if (reading != button.lastReading)
  {
    button.lastReading = reading;
    button.changedAt = millis();
  }
  if ((uint32_t)(millis() - button.changedAt) < BUTTON_DEBOUNCE_MS)
    return false;
  if (reading == button.stableState)
    return false;
  button.stableState = reading;
  return reading == LOW;
}

void processButtons()
{
  uint32_t now = millis();

  // O botao MENU/RESET tem tres funcoes sem bloquear o loop:
  // 1) toque curto = avanca uma tela;
  // 2) segurar por 5 s = liga/desliga o LCD;
  // 3) continuar segurando ate 30 s = abre o AP de configuracao.
  bool previousMenuState = menuButton.stableState;
  bool menuPressEvent = buttonPressed(menuButton);
  bool menuReleaseEvent = (previousMenuState == LOW && menuButton.stableState == HIGH);

  if (menuPressEvent)
  {
    menuPressedAt = now;
    menuLongActionDone = false;
    menuApActionDone = false;
  }

  if (menuButton.stableState == LOW &&
      !menuLongActionDone &&
      (uint32_t)(now - menuPressedAt) >= LCD_POWER_HOLD_MS)
  {
    menuLongActionDone = true;
    lcdSetEnabled(!lcdDisplayEnabled);
  }

  if (menuButton.stableState == LOW &&
      !menuApActionDone &&
      (uint32_t)(now - menuPressedAt) >= CONFIG_AP_HOLD_MS)
  {
    menuApActionDone = true;
    Serial.println("[BOTAO] 30 s detectados. Abrindo AP de configuracao agora...");
    startConfigAp();
  }

  // So executa o toque curto quando o botao e solto.
  // Isso impede que um toque longo tambem troque de pagina.
  if (menuReleaseEvent)
  {
    if (!menuLongActionDone && lcdDisplayEnabled && activeLcdEvent.device == 0)
    {
      lcdPage = (lcdPage + 1) % LCD_PAGE_COUNT;
      lastMenuActionMs = now;
      lastLcdUpdateMs = 0;
      Serial.printf("[LCD] Tela %u/%u\n", lcdPage + 1, LCD_PAGE_COUNT);
    }
    menuLongActionDone = false;
    menuApActionDone = false;
  }

  // BOMBA e LUZ: toque curto (ao soltar) = liga/desliga.
  // BOMBA + LUZ juntos por 3 s = modo PAREAMENTO com o SERVE.
  static uint32_t pumpDownAt = 0, lightDownAt = 0;
  static bool comboUsed = false, pairTriggered = false;
  bool pumpWasDown = pumpButton.stableState == LOW;
  bool lightWasDown = lightButton.stableState == LOW;
  if (buttonPressed(pumpButton))
    pumpDownAt = now;
  if (buttonPressed(lightButton))
    lightDownAt = now;
  bool pumpDown = pumpButton.stableState == LOW;
  bool lightDown = lightButton.stableState == LOW;

  if (pumpDown && lightDown)
  {
    comboUsed = true;
    uint32_t since = (uint32_t)(now - (pumpDownAt > lightDownAt ? pumpDownAt : lightDownAt));
    if (!pairTriggered && since >= DMK_PAIR_HOLD_MS)
    {
      pairTriggered = true;
      Serial.println("[BOTAO] BOMBA + LUZ por 3 s: modo pareamento.");
      startPairing(DMK_PAIR_WINDOW_S);
    }
  }

  if (pumpWasDown && !pumpDown && !comboUsed && (uint32_t)(now - pumpDownAt) < 1500)
  {
    if (canControlPump())
      sendControlCommand(DmkProto::TARGET_PUMP, !pumpOn);
    else
      Serial.println("[BOTAO] Bomba sem dados atuais ou comando pendente.");
  }

  if (lightWasDown && !lightDown && !comboUsed && (uint32_t)(now - lightDownAt) < 1500)
  {
    if (canControlLight())
      sendControlCommand(DmkProto::TARGET_LIGHT, !lightOn);
    else
      Serial.println("[BOTAO] Luz sem dados atuais ou comando pendente.");
  }

  if (!pumpDown && !lightDown)
  {
    comboUsed = false;
    pairTriggered = false;
  }
}

void updateLcdNavigation()
{
  uint32_t now = millis();
  if (activeLcdEvent.device != 0 &&
      (uint32_t)(now - activeLcdEventAt) >= LCD_EVENT_MS)
  {
    activeLcdEvent = {};
    lcdPage = 0;
    lastLcdUpdateMs = 0;
  }

  // Uma alteracao confirmada tem prioridade sobre a navegacao normal.
  if (activeLcdEvent.device == 0)
  {
    LcdStateEvent next;
    if (lcdQueueReceive(next))
    {
      activeLcdEvent = next;
      activeLcdEventAt = now;
      lcdPage = 0;
      lastLcdUpdateMs = 0;
    }
  }

  if (activeLcdEvent.device == 0 && lcdPage != 0 &&
      (uint32_t)(now - lastMenuActionMs) >= LCD_MENU_IDLE_MS)
  {
    lcdPage = 0;
    lastLcdUpdateMs = 0;
    Serial.println("[LCD] Menu inativo: retorno as tensoes.");
  }
}

// ---------------------------------------------------------------------------
// LEDs
// ---------------------------------------------------------------------------
void updateLeds()
{
  bool ok = mainCommunicationOk();
  digitalWrite(PIN_PUMP_LED, ok && pumpStateKnown && pumpOn ? HIGH : LOW);
  digitalWrite(PIN_LIGHT_LED, ok && lightStateKnown && lightOn ? HIGH : LOW);
}

// ---------------------------------------------------------------------------
// LCD
// ---------------------------------------------------------------------------
String voltage3(float voltage, bool fresh)
{
  if (!fresh)
    return "---";

  // Mantem sempre 3 caracteres no LCD: 000..999.
  // Ex.: 0 V -> 000 | 220 V -> 220 | sem dados -> ---
  float limited = voltage;
  if (limited < 0.0f)
    limited = 0.0f;
  if (limited > 999.0f)
    limited = 999.0f;
  int value = (int)(limited + 0.5f);

  char buf[4];
  snprintf(buf, sizeof(buf), "%03d", value);
  return String(buf);
}

String mqttTelemetryTokenValue(const String &rawToken)
{
  String token = rawToken;
  token.trim();

  String upper = token;
  upper.toUpperCase();

  if (upper == "SSID")
    return WiFi.status() == WL_CONNECTED ? WiFi.SSID() : cfgWifiSsid;
  if (upper == "RSSI")
    return WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) : String("--");
  if (upper == "IP")
    return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("--");
  if (upper == "MQTT")
    return mqttConnected ? String("ON") : String("OFF");
  if (upper == "MQTT_HOST")
    return cfgMqttHost;
  if (upper == "MQTT_TOPIC")
    return cfgTopicBase;
  if (upper == "BOMBA" || upper == "PUMP")
    return pumpStateKnown ? (pumpOn ? String("ON") : String("OFF")) : String("--");
  if (upper == "LUZ" || upper == "LIGHT")
    return lightStateKnown ? (lightOn ? String("ON") : String("OFF")) : String("--");
  if (upper == "VR")
    return telemetryFresh() ? String((int)(voltageR + 0.5f)) : String("--");
  if (upper == "VS")
    return telemetryFresh() ? String((int)(voltageS + 0.5f)) : String("--");
  if (upper == "VT")
    return telemetryFresh() ? String((int)(voltageT + 0.5f)) : String("--");
  if (upper == "CORRENTE" || upper == "CURRENT")
    return telemetryFresh() ? String(currentA, 2) : String("--");
  if (upper == "TEMP" || upper == "TEMPERATURA")
    return telemetryFresh() ? String(temperatureC, 1) : String("--");
  if (upper == "FLUXO" || upper == "FLOW")
    return telemetryFresh() ? String(flowLitersMinute, 2) : String("--");
  if (upper == "TOTAL")
    return telemetryFresh() ? String(totalLiters, 1) : String("--");
  if (upper == "CHUVA" || upper == "RAIN")
    return telemetryFresh() ? String(rainPercent, 0) : String("--");
  if (upper == "ALARME" || upper == "ALARM")
    return telemetryFresh() ? (alarmActive ? String("ATIVO") : String("NORMAL")) : String("--");
  if (upper == "ESP_RSSI")
    return telemetryFresh() ? String(mainEspRssi) : String("--");

  // Tensoes ADC RMS sempre como inteiro no LCD.
  // Ex.: 117.00 -> 117 | 74.40 -> 74 | 220.60 -> 221
  if (upper == "ADCVRMSR" || upper == "ADCVRMSS" || upper == "ADCVRMST")
  {
    float voltageValue;
    if (jsonGetFloat(lastTelemetryPayload, token.c_str(), voltageValue))
      return String((int)(voltageValue + 0.5f));

    String lowerVoltageToken = token;
    lowerVoltageToken.toLowerCase();
    if (jsonGetFloat(lastTelemetryPayload, lowerVoltageToken.c_str(), voltageValue))
      return String((int)(voltageValue + 0.5f));

    return String("--");
  }

  // Qualquer chave recebida no JSON MQTT pode ser usada como {nome_da_chave}.
  // Primeiro tenta exatamente como digitada; depois em minúsculas.
  float numberValue;
  if (jsonGetFloat(lastTelemetryPayload, token.c_str(), numberValue))
    return String(numberValue, 2);

  String stringValue;
  if (jsonGetString(lastTelemetryPayload, token.c_str(), stringValue))
    return stringValue;

  bool boolValue;
  if (jsonGetBool(lastTelemetryPayload, token.c_str(), boolValue))
    return boolValue ? String("ON") : String("OFF");

  String lower = token;
  lower.toLowerCase();
  if (lower != token)
  {
    if (jsonGetFloat(lastTelemetryPayload, lower.c_str(), numberValue))
      return String(numberValue, 2);
    if (jsonGetString(lastTelemetryPayload, lower.c_str(), stringValue))
      return stringValue;
    if (jsonGetBool(lastTelemetryPayload, lower.c_str(), boolValue))
      return boolValue ? String("ON") : String("OFF");
  }

  return String("--");
}

String resolveLcdTemplate(const String &source)
{
  String out;
  out.reserve(source.length() + 12);

  for (size_t i = 0; i < source.length();)
  {
    if (source[i] == '{')
    {
      int close = source.indexOf('}', i + 1);
      if (close > (int)i + 1)
      {
        String token = source.substring(i + 1, close);
        out += mqttTelemetryTokenValue(token);
        i = (size_t)close + 1;
        continue;
      }
    }

    out += source[i];
    i++;
  }

  return out;
}

void updateLcd()
{
  if (!lcdReady || !lcdDisplayEnabled)
    return;
  uint32_t now = millis();
  if ((uint32_t)(now - lastLcdUpdateMs) < LCD_UPDATE_MS)
    return;
  lastLcdUpdateMs = now;

  bool ok = mainCommunicationOk();
  String line[4];
  char buf[28];

  if (DmkLink::pairing() || (linkMessage == "PAREADO" && restartAtMs))
  {
    if (DmkLink::pairing())
    {
      line[0] = "PAREANDO...";
      line[1] = "NO SERVE: BOTAO";
      line[2] = "BOOT 3s E SOLTE";
      snprintf(buf, sizeof(buf), "RESTAM %lus", (unsigned long)DmkLink::pairRemainingS());
      line[3] = buf;
    }
    else
    {
      line[0] = "PAREADO!";
      line[1] = "WI-FI RECEBIDO";
      line[2] = "REINICIANDO...";
      line[3] = "";
    }
  }
  else if (lcdRemoteMode && lcdRemoteKey.length())
  {
    float numericValue;
    String displayValue;
    if (jsonGetFloat(lastTelemetryPayload, lcdRemoteKey.c_str(), numericValue))
      displayValue = String(numericValue, 1);
    else if (!jsonGetString(lastTelemetryPayload, lcdRemoteKey.c_str(), displayValue))
      displayValue = "--";
    line[0] = lcdRemoteLabel.length() ? lcdRemoteLabel : lcdRemoteKey;
    line[1] = displayValue + (lcdRemoteUnit.length() ? " " + lcdRemoteUnit : "");
    line[2] = "SENSOR REMOTO";
    line[3] = telemetryFresh() ? "ATUALIZADO" : "SEM DADOS";
  }
  else if (lcdCustomMode)
  {
    // Resolve os tokens a cada atualização, portanto os valores mudam ao vivo.
    for (uint8_t i = 0; i < 4; i++)
      line[i] = resolveLcdTemplate(lcdCustomLines[i]);
  }
  else if (activeLcdEvent.device != 0)
  {
    line[0] = activeLcdEvent.device == 1 ? "BOMBA" : "LUZ";
    line[1] = activeLcdEvent.on ? "LIGADA" : "DESLIGADA";
    line[2] = "ACAO CONFIRMADA";
    uint32_t left = (LCD_EVENT_MS - (uint32_t)(now - activeLcdEventAt) + 999) / 1000;
    snprintf(buf, sizeof(buf), "TENSOES EM %lus", (unsigned long)left);
    line[3] = buf;
  }
  else
    switch (lcdPage)
    {
    case 0:
    {
      // Tela padrao fixa, sem quebra de texto no LCD 16x4.
      // Linhas 3 e 4 ficam limpas para manter a leitura simples.
      String f1 = voltage3(voltageR, ok);
      String f2 = voltage3(voltageS, ok);
      String f3 = voltage3(voltageT, ok);

      line[0] = "F1: " + f1 + "/F2: " + f2;
      line[1] = "F3 : " + f3 + "  ( V~ )";
      line[2] = "";
      line[3] = "";
      break;
    }
    case 1:
      line[0] = "BOMBA/LUZ   2/6";
      line[1] = ok && pumpStateKnown ? (pumpOn ? "BOMBA: LIGADA" : "BOMBA: DESLIG.") : "BOMBA: SEM DADOS";
      line[2] = ok && lightStateKnown ? (lightOn ? "LUZ: LIGADA" : "LUZ: DESLIGADA") : "LUZ: SEM DADOS";
      line[3] = commandPending ? "BOMBA: AGUARDE" : lightCommandPending ? "LUZ: AGUARDE"
                                                                        : "BOTOES: CONTROLE";
      break;
    case 2:
      line[0] = "AGUA/CORR.  3/6";
      if (ok)
      {
        snprintf(buf, sizeof(buf), "CORR: %.2f A", currentA);
        line[1] = buf;
        snprintf(buf, sizeof(buf), "FLUXO: %.2fL/m", flowLitersMinute);
        line[2] = buf;
        snprintf(buf, sizeof(buf), "TOTAL: %.1f L", totalLiters);
        line[3] = buf;
      }
      else
      {
        line[1] = "CORR: -- A";
        line[2] = "FLUXO: -- L/m";
        line[3] = "TOTAL: -- L";
      }
      break;
    case 3:
      line[0] = "AMBIENTE   4/6";
      if (ok)
      {
        snprintf(buf, sizeof(buf), "TEMP: %.1f C", temperatureC);
        line[1] = buf;
        snprintf(buf, sizeof(buf), "CHUVA: %.0f %%", rainPercent);
        line[2] = buf;
        line[3] = alarmActive ? "ALARME: ATIVO" : "ALARME: NORMAL";
      }
      else
      {
        line[1] = "TEMP: -- C";
        line[2] = "CHUVA: -- %";
        line[3] = "ALARME: SEM DADO";
      }
      break;
    case 4:
      line[0] = "LINK/REDE  5/6";
      line[1] = String("VIA: ") + DmkLink::viaName(activeVia());
      line[2] = String(WiFi.status() == WL_CONNECTED ? "WIFI:ON " : "WIFI:OFF") + (mqttConnected ? " MQTT:ON" : " MQTT:--");
      if (ok)
      {
        snprintf(buf, sizeof(buf), "ESP:ON %ddBm", mainEspRssi);
        line[3] = buf;
      }
      else
        line[3] = "ESP PRINC: OFF";
      break;
    default:
      line[0] = "VERSOES/IP 6/6";
      line[1] = "REM: " + String(FIRMWARE_VERSION);
      line[2] = "PRI: " + String(ok ? mainFirmwareVersion : "--");
      line[3] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("IP: --");
      break;
    }
  for (uint8_t row = 0; row < lcdRows && row < 4; row++)
    lcdPrintLineIfChanged(row, line[row]);

  // Limpa cache das linhas que deixaram de existir ao mudar, por exemplo 20x4 -> 16x2.
  for (uint8_t row = lcdRows; row < 4; row++)
    lastLcdLines[row] = "";
}

// ---------------------------------------------------------------------------
// TIMEOUT DE COMANDO
// ---------------------------------------------------------------------------
void maintainCommandState()
{
  if (commandPending && (uint32_t)(millis() - commandSentMs) >= COMMAND_TIMEOUT_MS)
  {
    commandPending = false;
    Serial.println("[BOMBA] Timeout aguardando confirmacao.");
  }
  if (lightCommandPending && (uint32_t)(millis() - lightCommandSentMs) >= COMMAND_TIMEOUT_MS)
  {
    lightCommandPending = false;
    Serial.println("[LUZ] Timeout aguardando confirmacao.");
  }
}

// ---------------------------------------------------------------------------
// SETUP
// ---------------------------------------------------------------------------
bool dmkPrepareMqttOta(String &error){if(Update.isRunning()){error="Outra OTA em andamento";return false;}return true;}
void dmkEndMqttOta(bool){}
bool dmkCanAutoUpdate(){return !commandPending && !lightCommandPending;}

void setup()
{
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("======================================================");

  if (!dmkFilesystemBegin())
  {
    Serial.println("[FS] ERRO: LittleFS nao montou (configuracoes do ESP8266 dependem dele).");
  }
  Serial.println(" DMK REMOTO - BOMBA/LUZ + LCD 16x4 + MQTT");
  Serial.printf(" FIRMWARE: %s v%s | build %s\n", FIRMWARE_NAME, FIRMWARE_VERSION, BUILD_DATE);
  Serial.println("======================================================");

  pinMode(PIN_PUMP_BUTTON, INPUT_PULLUP);
  pinMode(PIN_LIGHT_BUTTON, INPUT_PULLUP);
  pinMode(PIN_MENU_BUTTON, INPUT_PULLUP);
  pinMode(PIN_PUMP_LED, OUTPUT);
  pinMode(PIN_LIGHT_LED, OUTPUT);
  digitalWrite(PIN_PUMP_LED, LOW);
  digitalWrite(PIN_LIGHT_LED, LOW);
  DmkStatusLed::begin(PIN_STATUS_LED, STATUS_LED_ACTIVE_LOW, STATUS_LED_RGB);
  DmkStatusLed::set(DmkStatusLed::LED_SLOW);

  lcdQueueInit();

  loadConfig();

  lcdInit();

  if (lcdReady && !lcdDisplayEnabled)
  {
    lcdCommand(0x08);
    lcdExpanderWrite(0);
  }

  if (lcdReady)
  {
    lcdClear();
    if (lcdRows >= 1)
      lcdPrintLine(0, "DMK IOT SMART");
    if (lcdRows >= 2)
      lcdPrintLine(1, "BOMBA + LUZ");
    if (lcdRows >= 3)
      lcdPrintLine(2, "LCD " + String(lcdColumns) + "x" + String(lcdRows));
    if (lcdRows >= 4)
      lcdPrintLine(3, "INICIANDO...");
  }

  setupWeb();

  beginWifi();

  // Enlace com o SERVE (ESP-NOW + rede local).
  DmkLink::controleSetup(onPaired);
  DmkLink::begin(deviceName.c_str(), FIRMWARE_VERSION, onLinkFrame);
  servePaired = DmkLink::hasKey() && DmkLink::serveMac(serveMacAddr);
  if (servePaired)
  {
    if (WiFi.status() != WL_CONNECTED && DmkLink::savedChannel())
      DmkLink::setChannel(DmkLink::savedChannel());
  }
  else
  {
    Serial.println("[PAREAR] Controle sem SERVE pareado: procurando automaticamente.");
    startPairing(DMK_PAIR_FIRST_BOOT_S);
  }

  // Forca a primeira tela operacional logo apos o setup.
  lastLcdUpdateMs = 0;
  updateLcd();
  MqttOta::begin();
}

// ---------------------------------------------------------------------------
// LOOP
// ---------------------------------------------------------------------------
void loop()
{
  MqttOta::loop(cfgMqttHost,cfgMqttPort,cfgMqttUsername,cfgMqttPassword,cfgOtaPassword);

  maintainWifi();
  maintainConfigAp();

  startWebIfReady();

  // Prioridade para interface local: web, portal, botoes e LCD.
  // Assim uma tentativa MQTT nao deixa o equipamento parecer travado.
  if (webStarted)
    server.handleClient();

  if (configApActive)
    dnsServer.processNextRequest();

  maintainLink();
  processButtons();
  maintainCommandRoute();
  updateLcdNavigation();
  maintainCommandState();
  updateLeds();
  updateLcd();
  updateStatusLed();

  if (restartAtMs && (int32_t)(millis() - restartAtMs) >= 0)
    ESP.restart();

  // MQTT fica por ultimo porque no ESP8266 uma tentativa TCP/DNS pode bloquear
  // por alguns instantes quando o broker ou a internet estao indisponiveis.
  startMqttIfReady();

#if defined(DMK_BOARD_ESP8266)
  yield();
#else
  delay(1);
#endif
}
