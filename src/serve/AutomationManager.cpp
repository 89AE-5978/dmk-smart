#include "AutomationManager.h"

#include "DmkClock.h"
#if defined(DMK_BOARD_ESP8266)
#include "PreferencesCompat.h"
#else
#include <Preferences.h>
#endif

namespace {

constexpr uint32_t STORE_MAGIC = 0x444D4B41; // DMKA
constexpr uint16_t STORE_VERSION = 1;

struct StoreData {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  uint32_t nextId;
  AutomationManager::Automation rows[AutomationManager::MAX_AUTOMATIONS];
};

Preferences prefs;
StoreData storeData = {};
AutomationManager::PumpCallback pumpCb = nullptr;
AutomationManager::LightCallback lightCb = nullptr;
uint32_t lastCheckMs = 0;

void safeCopy(char *dst, size_t size, const char *src, const char *fallback) {
  if (!dst || size == 0) return;
  const char *v = (src && src[0]) ? src : fallback;
  snprintf(dst, size, "%s", v ? v : "");
}

String jsonEscapeAuto(const String &input) {
  String out;
  out.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); ++i) {
    char c = input[i];
    if (c == '\\') out += "\\\\";
    else if (c == '"') out += "\\\"";
    else if (c == '\n') out += "\\n";
    else if ((uint8_t)c >= 0x20) out += c;
  }
  return out;
}

void resetStore() {
  memset(&storeData, 0, sizeof(storeData));
  storeData.magic = STORE_MAGIC;
  storeData.version = STORE_VERSION;
  storeData.nextId = 1;
}

bool persist() {
  prefs.begin("dmkauto", false);
  size_t written = prefs.putBytes("data", &storeData, sizeof(storeData));
  prefs.end();
  return written == sizeof(storeData);
}

void load() {
  resetStore();

  if (!prefs.begin("dmkauto", false)) {
    Serial.println("[AUTO] Aviso: namespace NVS dmkauto indisponivel. Agenda vazia.");
    return;
  }
  size_t size = prefs.isKey("data") ? prefs.getBytesLength("data") : 0;

  if (size == sizeof(storeData)) {
    StoreData temp = {};
    size_t read = prefs.getBytes("data", &temp, sizeof(temp));

    if (read == sizeof(temp) &&
        temp.magic == STORE_MAGIC &&
        temp.version == STORE_VERSION) {
      storeData = temp;
    }
  }
  prefs.end();

  if (storeData.nextId == 0)
    storeData.nextId = 1;
}

int findIndex(uint32_t id) {
  if (id == 0) return -1;
  for (uint8_t i = 0; i < AutomationManager::MAX_AUTOMATIONS; ++i) {
    if (storeData.rows[i].id == id)
      return i;
  }
  return -1;
}

int findFree() {
  for (uint8_t i = 0; i < AutomationManager::MAX_AUTOMATIONS; ++i) {
    if (storeData.rows[i].id == 0)
      return i;
  }
  return -1;
}

String localTimeFromEpoch(time_t epoch) {
  if (epoch <= 0)
    return "";

  struct tm t;
  if (localtime_r(&epoch, &t) == nullptr)
    return "";

  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  return String(buf);
}

} // namespace

namespace AutomationManager {

bool begin(PumpCallback pumpCallback, LightCallback lightCallback) {
  pumpCb = pumpCallback;
  lightCb = lightCallback;
  load();

  Serial.printf("[AUTO] Automacoes carregadas: %u/%u\n",
                count(), MAX_AUTOMATIONS);
  return true;
}

void update() {
  uint32_t nowMs = millis();
  if ((uint32_t)(nowMs - lastCheckMs) < 500)
    return;
  lastCheckMs = nowMs;

  if (!DmkClock::isValid())
    return;

  time_t epoch = DmkClock::nowEpoch();
  if (epoch <= 0)
    return;

  struct tm localTm;
  if (localtime_r(&epoch, &localTm) == nullptr)
    return;

  // Uma chave por minuto garante somente UMA execucao por horario.
  uint32_t minuteKey = (uint32_t)(epoch / 60);
  uint8_t dayBit = (uint8_t)(1U << localTm.tm_wday);

  bool changed = false;

  for (uint8_t i = 0; i < MAX_AUTOMATIONS; ++i) {
    Automation &a = storeData.rows[i];

    if (a.id == 0 || !a.enabled)
      continue;

    if ((a.daysMask & dayBit) == 0)
      continue;

    if (a.hour != (uint8_t)localTm.tm_hour ||
        a.minute != (uint8_t)localTm.tm_min)
      continue;

    if (a.lastRunMinuteKey == minuteKey)
      continue;

    // Marca ANTES do callback. Assim um callback lento/reentrada nao repete.
    a.lastRunMinuteKey = minuteKey;
    a.lastRunEpoch = epoch;
    changed = true;

    Serial.printf("[AUTO] #%lu %s -> %s %s | %02u:%02u\n",
                  (unsigned long)a.id,
                  a.name,
                  a.target == TARGET_PUMP ? "BOMBA" : "LUZ",
                  a.turnOn ? "ON" : "OFF",
                  a.hour,
                  a.minute);

    if (a.target == TARGET_PUMP) {
      if (pumpCb != nullptr)
        pumpCb(a.turnOn, a.id);
    } else if (a.target == TARGET_LIGHT) {
      if (lightCb != nullptr)
        lightCb(a.turnOn, a.id);
    }
  }

  if (changed)
    persist();
}

bool save(uint32_t id,
          const char *name,
          uint8_t target,
          bool turnOn,
          uint8_t hour,
          uint8_t minute,
          uint8_t daysMask,
          bool enabled,
          uint32_t *savedId) {
  if (target != TARGET_PUMP && target != TARGET_LIGHT)
    return false;
  if (hour > 23 || minute > 59)
    return false;
  if ((daysMask & EVERY_DAY) == 0)
    return false;

  int index = -1;

  if (id != 0)
    index = findIndex(id);
  else
    index = findFree();

  if (index < 0)
    return false;

  Automation &a = storeData.rows[index];

  // Nova automacao.
  if (id == 0) {
    memset(&a, 0, sizeof(a));
    a.id = storeData.nextId++;
    if (storeData.nextId == 0)
      storeData.nextId = 1;
  }

  a.enabled = enabled;
  a.target = target;
  a.turnOn = turnOn;
  a.hour = hour;
  a.minute = minute;
  a.daysMask = daysMask & EVERY_DAY;
  safeCopy(a.name, sizeof(a.name), name, "Automacao");

  if (!persist())
    return false;

  if (savedId)
    *savedId = a.id;

  return true;
}

bool remove(uint32_t id) {
  int index = findIndex(id);
  if (index < 0)
    return false;

  memset(&storeData.rows[index], 0, sizeof(storeData.rows[index]));
  return persist();
}

bool setEnabled(uint32_t id, bool enabled) {
  int index = findIndex(id);
  if (index < 0)
    return false;

  storeData.rows[index].enabled = enabled;
  return persist();
}

bool clear() {
  resetStore();
  return persist();
}

String toJson() {
  String json;
  json.reserve(2600);
  json += "{\"ok\":true,\"clock_valid\":";
  json += DmkClock::isValid() ? "true" : "false";
  json += ",\"clock_local\":\"";
  json += jsonEscapeAuto(DmkClock::localDateTime());
  json += "\",\"automations\":[";

  bool first = true;

  for (uint8_t i = 0; i < MAX_AUTOMATIONS; ++i) {
    const Automation &a = storeData.rows[i];
    if (a.id == 0)
      continue;

    if (!first)
      json += ",";
    first = false;

    json += "{";
    json += "\"id\":" + String(a.id);
    json += ",\"name\":\"" + jsonEscapeAuto(String(a.name)) + "\"";
    json += ",\"enabled\":";
    json += a.enabled ? "true" : "false";
    json += ",\"target\":\"";
    json += (a.target == TARGET_PUMP) ? "PUMP" : "LIGHT";
    json += "\"";
    json += ",\"state\":\"";
    json += a.turnOn ? "ON" : "OFF";
    json += "\"";
    json += ",\"hour\":" + String(a.hour);
    json += ",\"minute\":" + String(a.minute);
    json += ",\"days_mask\":" + String(a.daysMask);
    json += ",\"last_run_epoch\":" + String((unsigned long)a.lastRunEpoch);
    json += ",\"last_run_local\":\"";
    json += jsonEscapeAuto(localTimeFromEpoch(a.lastRunEpoch));
    json += "\"";
    json += "}";
  }

  json += "]}";
  return json;
}

bool get(uint32_t id, Automation &out) {
  int index = findIndex(id);
  if (index < 0)
    return false;
  out = storeData.rows[index];
  return true;
}

uint8_t count() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_AUTOMATIONS; ++i)
    if (storeData.rows[i].id != 0)
      ++n;
  return n;
}

} // namespace AutomationManager
