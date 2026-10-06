#include "DmkClock.h"

#if defined(DMK_BOARD_ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif
#include <sys/time.h>

namespace {

constexpr time_t MIN_VALID_EPOCH = 1700000000; // 2023-11-14
constexpr uint32_t NTP_RETRY_INVALID_MS = 30UL * 1000UL;
constexpr uint32_t NTP_REFRESH_MS = 6UL * 60UL * 60UL * 1000UL;

const char *NTP_1 = "pool.ntp.org";
const char *NTP_2 = "time.google.com";
const char *NTP_3 = "time.cloudflare.com";

String tzName = "BRT3";
bool clockValid = false;
bool ntpRequested = false;
bool wasWifiConnected = false;
uint32_t lastNtpRequest = 0;
uint32_t lastValidCheck = 0;
char clockSource[12] = "INVALIDO";

void setSource(const char *value) {
  snprintf(clockSource, sizeof(clockSource), "%s", value ? value : "SISTEMA");
}

void requestNtp() {
  // IMPORTANTE:
  // configTime(0, 0, ...) pode sobrescrever o TZ local e deixar localtime()
  // em UTC. Isso causava +3 horas no Brasil (ex.: 03:55 em vez de 00:55).
  //
  // configTzTime() sincroniza o epoch por NTP e PRESERVA o fuso POSIX BRT3,
  // portanto o relogio interno permanece UTC e a exibicao local fica UTC-3.
  #if defined(DMK_BOARD_ESP8266)
  configTime(tzName.c_str(), NTP_1, NTP_2, NTP_3);
#else
  configTzTime(tzName.c_str(), NTP_1, NTP_2, NTP_3);
#endif
  ntpRequested = true;
  lastNtpRequest = millis();
  Serial.printf("[CLOCK] Sincronizacao NTP solicitada | TZ=%s\n",
                tzName.c_str());
}

bool epochLooksValid(time_t epoch) {
  return epoch >= MIN_VALID_EPOCH;
}

String jsonEscapeClock(const String &input) {
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

} // namespace

namespace DmkClock {

bool begin(const char *timezone) {
  if (timezone != nullptr && timezone[0] != '\0')
    tzName = timezone;

  setenv("TZ", tzName.c_str(), 1);
  tzset();

  time_t now = time(nullptr);
  clockValid = epochLooksValid(now);

  if (clockValid) {
    setSource("SISTEMA");
    Serial.printf("[CLOCK] Hora ja valida no boot: %s\n", localDateTime().c_str());
  } else {
    setSource("INVALIDO");
    Serial.println("[CLOCK] Aguardando NTP ou ajuste manual.");
  }

  return true;
}

void update(bool wifiConnected) {
  uint32_t nowMs = millis();

  if (wifiConnected && !wasWifiConnected) {
    requestNtp();
  }
  wasWifiConnected = wifiConnected;

  // Enquanto invalido, tenta NTP periodicamente sem bloquear o loop.
  if (wifiConnected && !clockValid) {
    if (!ntpRequested ||
        (uint32_t)(nowMs - lastNtpRequest) >= NTP_RETRY_INVALID_MS) {
      requestNtp();
    }
  }

  // Reforca a sincronizacao periodicamente quando ha internet.
  if (wifiConnected && clockValid &&
      (uint32_t)(nowMs - lastNtpRequest) >= NTP_REFRESH_MS) {
    requestNtp();
  }

  // Nao consulta time() em toda volta do loop.
  if ((uint32_t)(nowMs - lastValidCheck) < 500)
    return;
  lastValidCheck = nowMs;

  time_t now = time(nullptr);
  bool validNow = epochLooksValid(now);

  if (!clockValid && validNow) {
    clockValid = true;

    // Se a hora ficou valida enquanto havia internet e havia um pedido NTP,
    // assumimos NTP. Ajuste manual define MANUAL diretamente.
    if (wifiConnected && ntpRequested && strcmp(clockSource, "MANUAL") != 0)
      setSource("NTP");
    else if (strcmp(clockSource, "MANUAL") != 0)
      setSource("SISTEMA");

    Serial.printf("[CLOCK] Relogio valido: %s | fonte=%s\n",
                  localDateTime().c_str(),
                  clockSource);
  } else if (clockValid && !validNow) {
    clockValid = false;
    setSource("INVALIDO");
    Serial.println("[CLOCK] Relogio ficou invalido. Automacoes pausadas.");
  }
}

bool isValid() {
  if (!clockValid)
    return false;
  return epochLooksValid(time(nullptr));
}

time_t nowEpoch() {
  time_t now = time(nullptr);
  return epochLooksValid(now) ? now : 0;
}

String localDateTime() {
  time_t now = nowEpoch();
  if (now == 0)
    return "";

  struct tm localTm;
  if (localtime_r(&now, &localTm) == nullptr)
    return "";

  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &localTm);
  return String(buf);
}

String localIso() {
  time_t now = nowEpoch();
  if (now == 0)
    return "";

  struct tm localTm;
  if (localtime_r(&now, &localTm) == nullptr)
    return "";

  char dateBuf[24];
  strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%dT%H:%M:%S", &localTm);

  // Para BRT3 o offset e -03:00. O projeto usa esse fuso por padrao.
  // A hora interna continua em UTC.
  String out(dateBuf);
  out += "-03:00";
  return out;
}

const char *source() {
  return clockSource;
}

bool setManual(int year, int month, int day,
               int hour, int minute, int second) {
  if (year < 2024 || year > 2099) return false;
  if (month < 1 || month > 12) return false;
  if (day < 1 || day > 31) return false;
  if (hour < 0 || hour > 23) return false;
  if (minute < 0 || minute > 59) return false;
  if (second < 0 || second > 59) return false;

  struct tm localTm = {};
  localTm.tm_year = year - 1900;
  localTm.tm_mon = month - 1;
  localTm.tm_mday = day;
  localTm.tm_hour = hour;
  localTm.tm_min = minute;
  localTm.tm_sec = second;
  localTm.tm_isdst = -1;

  time_t epoch = mktime(&localTm);
  if (!epochLooksValid(epoch))
    return false;

  struct timeval tv;
  tv.tv_sec = epoch;
  tv.tv_usec = 0;

  if (settimeofday(&tv, nullptr) != 0)
    return false;

  clockValid = true;
  setSource("MANUAL");

  Serial.printf("[CLOCK] Hora ajustada manualmente: %s\n",
                localDateTime().c_str());
  return true;
}

uint32_t lastNtpRequestMillis() {
  return lastNtpRequest;
}

String statusJson(bool wifiConnected) {
  time_t epoch = nowEpoch();

  String json;
  json.reserve(320);
  json += "{";
  json += "\"valid\":";
  json += isValid() ? "true" : "false";
  json += ",\"epoch\":";
  json += String((unsigned long)epoch);
  json += ",\"local\":\"";
  json += jsonEscapeClock(localDateTime());
  json += "\",\"iso_local\":\"";
  json += jsonEscapeClock(localIso());
  json += "\",\"timezone\":\"";
  json += jsonEscapeClock(tzName);
  json += "\",\"source\":\"";
  json += jsonEscapeClock(source());
  json += "\",\"wifi\":";
  json += wifiConnected ? "true" : "false";
  json += "}";
  return json;
}

} // namespace DmkClock
