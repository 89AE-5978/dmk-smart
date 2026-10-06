#include "PumpHistory.h"

#include <LittleFS.h>
#include <new>
#if defined(DMK_BOARD_ESP8266)
#include "PreferencesCompat.h"
#else
#include <Preferences.h>
#endif
#include <ctype.h>
#include <time.h>
#include <math.h>

namespace {

constexpr const char *HISTORY_FILE = "/pump_history.jsonl";
constexpr const char *HISTORY_TMP  = "/pump_history.tmp";
constexpr size_t MAX_HISTORY_BYTES = 96 * 1024;  // ~500-700 eventos, depende do payload
constexpr uint16_t PRUNE_ROWS = 120;              // remove os mais antigos ao atingir o limite
constexpr uint16_t MAX_JSON_ROWS = 60;            // protege RAM ao montar resposta HTTP

Preferences historyPrefs;
uint32_t sequenceId = 0;
bool fsReady = false;

#if !defined(DMK_BOARD_ESP8266)
portMUX_TYPE pendingMux = portMUX_INITIALIZER_UNLOCKED;
#endif
volatile bool pendingAvailable = false;
PumpCommandContext pendingContext = {-1, false, 0.0, 0.0, 0.0f, "", ""};

void safeCopy(char *dst, size_t dstSize, const char *src, const char *fallback = "") {
  if (dst == nullptr || dstSize == 0) return;
  const char *value = (src != nullptr && src[0] != '\0') ? src : fallback;
  snprintf(dst, dstSize, "%s", value != nullptr ? value : "");
}

String jsonEscape(const String &input) {
  String out;
  out.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); ++i) {
    const char c = input[i];
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((uint8_t)c >= 0x20) out += c;
        break;
    }
  }
  return out;
}

String jsonEscape(const char *input) {
  return jsonEscape(String(input != nullptr ? input : ""));
}

bool validCoordinates(double lat, double lon) {
  if (!isfinite(lat) || !isfinite(lon)) return false;
  if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return false;

  // No DMK, 0.0/0.0 e o valor padrao quando o aplicativo nao envia GPS.
  // Assim nao marcamos o ponto vazio como uma localizacao real.
  if (fabs(lat) < 0.0000001 && fabs(lon) < 0.0000001) return false;

  return true;
}

int findJsonKey(const String &json, const char *key) {
  String token = "\"" + String(key) + "\"";
  int p = json.indexOf(token);
  if (p < 0) return -1;
  p = json.indexOf(':', p + token.length());
  if (p < 0) return -1;
  return p + 1;
}

bool extractJsonString(const String &json, const char *key, String &out) {
  int p = findJsonKey(json, key);
  if (p < 0) return false;

  while (p < (int)json.length() && isspace((unsigned char)json[p])) ++p;
  if (p >= (int)json.length() || json[p] != '"') return false;
  ++p;

  String value;
  bool escaped = false;
  for (; p < (int)json.length(); ++p) {
    char c = json[p];
    if (escaped) {
      switch (c) {
        case 'n': value += '\n'; break;
        case 'r': value += '\r'; break;
        case 't': value += '\t'; break;
        default: value += c; break;
      }
      escaped = false;
      continue;
    }
    if (c == '\\') {
      escaped = true;
      continue;
    }
    if (c == '"') {
      out = value;
      return true;
    }
    value += c;
  }
  return false;
}

bool extractJsonNumber(const String &json, const char *key, double &out) {
  int p = findJsonKey(json, key);
  if (p < 0) return false;
  while (p < (int)json.length() && isspace((unsigned char)json[p])) ++p;

  int end = p;
  while (end < (int)json.length()) {
    char c = json[end];
    if (!(isdigit((unsigned char)c) || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')) break;
    ++end;
  }
  if (end <= p) return false;

  String number = json.substring(p, end);
  char *parseEnd = nullptr;
  double value = strtod(number.c_str(), &parseEnd);
  if (parseEnd == number.c_str() || !isfinite(value)) return false;
  out = value;
  return true;
}

int8_t parseStateText(String state) {
  state.trim();
  state.toUpperCase();
  if (state == "ON" || state == "1" || state == "TRUE" || state == "LIGAR" || state == "LIGADA") return 1;
  if (state == "OFF" || state == "0" || state == "FALSE" || state == "DESLIGAR" || state == "DESLIGADA") return 0;
  return -1;
}

bool parseMqttPayload(const char *data, int len, PumpCommandContext &ctx) {
  if (data == nullptr || len <= 0) return false;

  String payload;
  payload.reserve((size_t)len + 1);
  for (int i = 0; i < len; ++i) payload += data[i];
  payload.trim();

  memset(&ctx, 0, sizeof(ctx));
  ctx.command = -1;
  safeCopy(ctx.source, sizeof(ctx.source), "MQTT", "MQTT");

  // Compatibilidade com o firmware/app antigo.
  if (!payload.startsWith("{")) {
    ctx.command = parseStateText(payload);
    return ctx.command >= 0;
  }

  String state;
  if (!extractJsonString(payload, "state", state)) {
    // Tambem aceita "command" por compatibilidade com outros apps.
    extractJsonString(payload, "command", state);
  }
  ctx.command = parseStateText(state);
  if (ctx.command < 0) return false;

  String source;
  if (extractJsonString(payload, "source", source)) {
    source.trim();
    if (source.length() > 0) safeCopy(ctx.source, sizeof(ctx.source), source.c_str(), "MQTT");
  }

  String requestId;
  if (extractJsonString(payload, "request_id", requestId)) {
    safeCopy(ctx.requestId, sizeof(ctx.requestId), requestId.c_str(), "");
  }

  double lat = 0.0;
  double lon = 0.0;
  double accuracy = 0.0;

  bool hasLat = extractJsonNumber(payload, "lat", lat) ||
                extractJsonNumber(payload, "latitude", lat);
  bool hasLon = extractJsonNumber(payload, "lon", lon) ||
                extractJsonNumber(payload, "lng", lon) ||
                extractJsonNumber(payload, "longitude", lon);

  extractJsonNumber(payload, "accuracy_m", accuracy);
  if (accuracy <= 0.0) extractJsonNumber(payload, "accuracy", accuracy);

  if (hasLat && hasLon && validCoordinates(lat, lon)) {
    ctx.hasLocation = true;
    ctx.latitude = lat;
    ctx.longitude = lon;
    ctx.accuracyM = (accuracy >= 0.0 && accuracy < 100000.0) ? (float)accuracy : 0.0f;
  } else {
    // GPS nao enviado, vazio, 0/0 ou invalido: comando continua normalmente.
    ctx.hasLocation = false;
    ctx.latitude = 0.0;
    ctx.longitude = 0.0;
    ctx.accuracyM = 0.0f;
  }

  return true;
}

bool storePending(const PumpCommandContext &ctx) {
#if defined(DMK_BOARD_ESP8266)
  noInterrupts();
  pendingContext = ctx;
  pendingAvailable = true;
  interrupts();
#else
  portENTER_CRITICAL(&pendingMux);
  pendingContext = ctx;
  pendingAvailable = true;
  portEXIT_CRITICAL(&pendingMux);
#endif
  return true;
}

void pruneIfNeeded() {
  if (!fsReady || !LittleFS.exists(HISTORY_FILE)) return;

  File file = LittleFS.open(HISTORY_FILE, "r");
  if (!file) return;
  size_t size = file.size();
  file.close();

  if (size <= MAX_HISTORY_BYTES) return;

  File in = LittleFS.open(HISTORY_FILE, "r");
  File out = LittleFS.open(HISTORY_TMP, "w");
  if (!in || !out) {
    if (in) in.close();
    if (out) out.close();
    return;
  }

  uint16_t skipped = 0;
  while (in.available()) {
    String line = in.readStringUntil('\n');
    if (skipped < PRUNE_ROWS) {
      ++skipped;
      continue;
    }
    line.trim();
    if (line.length() == 0) continue;
    out.print(line);
    out.print('\n');
  }

  in.close();
  out.close();

  LittleFS.remove(HISTORY_FILE);
  if (!LittleFS.rename(HISTORY_TMP, HISTORY_FILE)) {
    // Se a troca falhar, removemos o temporario para nao deixar lixo.
    LittleFS.remove(HISTORY_TMP);
  }
}

String isoTimeUtc(time_t epoch) {
  if (epoch < 1700000000) return "";  // relogio ainda nao sincronizado

  struct tm tmUtc;
  if (gmtime_r(&epoch, &tmUtc) == nullptr) return "";

  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmUtc);
  return String(buf);
}


String isoTimeLocal(time_t epoch) {
  if (epoch < 1700000000) return "";

  struct tm tmLocal;
  if (localtime_r(&epoch, &tmLocal) == nullptr) return "";

  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmLocal);
  return String(buf);
}

bool appendEvent(bool pumpOn,
                 const PumpCommandContext &ctx,
                 const char *status,
                 const char *reason) {
  if (!fsReady) return false;

  pruneIfNeeded();

  File file = LittleFS.open(HISTORY_FILE, "a");
  if (!file) return false;

  ++sequenceId;
  historyPrefs.begin("pumphist", false);
  historyPrefs.putULong("seq", sequenceId);
  historyPrefs.end();

  time_t nowEpoch = time(nullptr);
  if (nowEpoch < 1700000000) nowEpoch = 0;

  String line;
  line.reserve(320);
  line += "{\"id\":" + String(sequenceId);
  line += ",\"epoch\":" + String((unsigned long)nowEpoch);
  line += ",\"time_utc\":\"" + jsonEscape(isoTimeUtc(nowEpoch)) + "\"";
  line += ",\"time_local\":\"" + jsonEscape(isoTimeLocal(nowEpoch)) + "\"";
  line += ",\"uptime_ms\":" + String(millis());
  line += ",\"state\":\"";
  line += pumpOn ? "ON" : "OFF";
  line += "\"";
  line += ",\"source\":\"" + jsonEscape(ctx.source) + "\"";
  line += ",\"location_valid\":";
  line += ctx.hasLocation ? "true" : "false";

  if (ctx.hasLocation) {
    line += ",\"lat\":" + String(ctx.latitude, 7);
    line += ",\"lon\":" + String(ctx.longitude, 7);
    line += ",\"accuracy_m\":" + String(ctx.accuracyM, 1);
  } else {
    // Mantem esquema numerico fixo para o app/tabela: vazio = 0.0 / 0.0.
    line += ",\"lat\":0.0,\"lon\":0.0,\"accuracy_m\":0.0";
  }

  line += ",\"status\":\"" + jsonEscape(status != nullptr ? status : "") + "\"";
  line += ",\"reason\":\"" + jsonEscape(reason != nullptr ? reason : "") + "\"";
  line += ",\"request_id\":\"" + jsonEscape(ctx.requestId) + "\"";
  line += "}";

  size_t written = file.println(line);
  file.flush();
  file.close();

  Serial.printf("[PUMP-HISTORY] #%lu %s | %s | GPS=%s\n",
                (unsigned long)sequenceId,
                pumpOn ? "ON" : "OFF",
                ctx.source,
                ctx.hasLocation ? "SIM" : "NAO");

  return written > 0;
}

}  // namespace

namespace PumpHistory {

bool begin(bool filesystemReady) {
  fsReady = filesystemReady;
  if (!fsReady) {
    Serial.println("[PUMP-HISTORY] LittleFS indisponivel. Historico desativado.");
    return false;
  }

  if (historyPrefs.begin("pumphist", false)) {
    sequenceId = historyPrefs.isKey("seq") ? historyPrefs.getULong("seq", 0) : 0;
    historyPrefs.end();
  } else {
    sequenceId = 0;
    Serial.println("[PUMP-HISTORY] Aviso: namespace NVS pumphist indisponivel.");
  }

  // Cria o arquivo se ainda nao existir.
  if (!LittleFS.exists(HISTORY_FILE)) {
    File f = LittleFS.open(HISTORY_FILE, "w");
    if (!f) {
      Serial.println("[PUMP-HISTORY] ERRO criando arquivo de historico.");
      fsReady = false;
      return false;
    }
    f.close();
  }

  Serial.printf("[PUMP-HISTORY] Pronto. Arquivo: %s | sequencia=%lu\n",
                HISTORY_FILE,
                (unsigned long)sequenceId);
  return true;
}

bool queueCommandFromMqtt(const char *data, int len) {
  PumpCommandContext ctx;
  if (!parseMqttPayload(data, len, ctx)) {
    Serial.println("[PUMP-HISTORY] Payload MQTT de bomba invalido.");
    return false;
  }
  return storePending(ctx);
}

bool queueCommand(bool turnOn,
                  const char *source,
                  bool hasLocation,
                  double latitude,
                  double longitude,
                  float accuracyM,
                  const char *requestId) {
  PumpCommandContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.command = turnOn ? 1 : 0;
  ctx.hasLocation = hasLocation && validCoordinates(latitude, longitude);
  ctx.latitude = ctx.hasLocation ? latitude : 0.0;
  ctx.longitude = ctx.hasLocation ? longitude : 0.0;
  ctx.accuracyM = (ctx.hasLocation && accuracyM >= 0.0f) ? accuracyM : 0.0f;
  safeCopy(ctx.source, sizeof(ctx.source), source, "UNKNOWN");
  safeCopy(ctx.requestId, sizeof(ctx.requestId), requestId, "");
  return storePending(ctx);
}

bool takePending(PumpCommandContext &out) {
  bool available = false;

#if defined(DMK_BOARD_ESP8266)
  noInterrupts();
  if (pendingAvailable) {
    out = pendingContext;
    pendingAvailable = false;
    pendingContext.command = -1;
    available = true;
  }
  interrupts();
#else
  portENTER_CRITICAL(&pendingMux);
  if (pendingAvailable) {
    out = pendingContext;
    pendingAvailable = false;
    pendingContext.command = -1;
    available = true;
  }
  portEXIT_CRITICAL(&pendingMux);
#endif

  return available;
}

bool logEvent(bool pumpOn,
              const PumpCommandContext &ctx,
              const char *status,
              const char *reason) {
  return appendEvent(pumpOn, ctx, status, reason);
}

bool logAutomatic(bool pumpOn,
                  const char *source,
                  const char *status,
                  const char *reason) {
  PumpCommandContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.command = pumpOn ? 1 : 0;
  ctx.hasLocation = false;
  ctx.latitude = 0.0;
  ctx.longitude = 0.0;
  ctx.accuracyM = 0.0f;
  safeCopy(ctx.source, sizeof(ctx.source), source, "AUTOMATIC");
  return appendEvent(pumpOn, ctx, status, reason);
}

String toJson(uint16_t maxRows) {
  if (!fsReady) return "{\"ok\":false,\"message\":\"LittleFS indisponivel\",\"events\":[]}";
  if (maxRows == 0) maxRows = 1;
  if (maxRows > MAX_JSON_ROWS) maxRows = MAX_JSON_ROWS;

  File file = LittleFS.open(HISTORY_FILE, "r");
  if (!file) return "{\"ok\":false,\"message\":\"Historico indisponivel\",\"events\":[]}";

  // Ring buffer pequeno: mantem somente os ultimos N registros na RAM.
  String *rows = new (std::nothrow) String[maxRows];
  if (rows == nullptr) {
    file.close();
    return "{\"ok\":false,\"message\":\"Memoria insuficiente\",\"events\":[]}";
  }

  uint32_t total = 0;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    rows[total % maxRows] = line;
    ++total;
  }
  file.close();

  const uint16_t used = (total < maxRows) ? (uint16_t)total : maxRows;
  const uint32_t start = (total > maxRows) ? (total - maxRows) : 0;

  String json;
  json.reserve(256 + used * 240);
  json += "{\"ok\":true,\"total\":" + String(total) + ",\"events\":[";
  for (uint16_t i = 0; i < used; ++i) {
    uint32_t logical = start + i;
    uint16_t slot = logical % maxRows;
    if (i) json += ',';
    json += rows[slot];
  }
  json += "]}";

  delete[] rows;
  return json;
}


uint16_t forEachRecent(uint16_t maxRows,
                       HistoryLineCallback callback,
                       void *userData,
                       uint32_t *totalRows) {
  if (totalRows != nullptr) *totalRows = 0;
  if (!fsReady || callback == nullptr) return 0;

  if (maxRows == 0) maxRows = 1;
  if (maxRows > MAX_JSON_ROWS) maxRows = MAX_JSON_ROWS;

  File file = LittleFS.open(HISTORY_FILE, "r");
  if (!file) return 0;

  String *rows = new (std::nothrow) String[maxRows];
  if (rows == nullptr) {
    file.close();
    return 0;
  }

  uint32_t total = 0;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    rows[total % maxRows] = line;
    ++total;
  }
  file.close();

  if (totalRows != nullptr) *totalRows = total;

  const uint16_t used = (total < maxRows) ? (uint16_t)total : maxRows;
  const uint32_t start = (total > maxRows) ? (total - maxRows) : 0;

  uint16_t sent = 0;
  for (uint16_t i = 0; i < used; ++i) {
    uint32_t logical = start + i;
    uint16_t slot = logical % maxRows;

    if (!callback(rows[slot], userData)) break;
    ++sent;
  }

  delete[] rows;
  return sent;
}

bool clear() {
  if (!fsReady) return false;
  if (LittleFS.exists(HISTORY_FILE)) LittleFS.remove(HISTORY_FILE);

  File f = LittleFS.open(HISTORY_FILE, "w");
  if (!f) return false;
  f.close();

  sequenceId = 0;
  historyPrefs.begin("pumphist", false);
  historyPrefs.putULong("seq", 0);
  historyPrefs.end();

  Serial.println("[PUMP-HISTORY] Historico apagado.");
  return true;
}

uint32_t count() {
  if (!fsReady) return 0;
  File file = LittleFS.open(HISTORY_FILE, "r");
  if (!file) return 0;
  uint32_t total = 0;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) ++total;
  }
  file.close();
  return total;
}

const char *filePath() {
  return HISTORY_FILE;
}

}  // namespace PumpHistory
