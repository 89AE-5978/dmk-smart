#pragma once
#include <Arduino.h>

struct PumpCommandContext {
  int8_t command;
  bool hasLocation;
  double latitude;
  double longitude;
  float accuracyM;
  char source[24];
  char requestId[40];
};

namespace PumpHistory {
bool begin(bool filesystemReady = true);
bool queueCommandFromMqtt(const char *data, int len);
bool queueCommand(bool turnOn, const char *source, bool hasLocation = false,
                  double latitude = 0.0, double longitude = 0.0,
                  float accuracyM = 0.0f, const char *requestId = nullptr);
bool takePending(PumpCommandContext &out);
bool logEvent(bool pumpOn, const PumpCommandContext &ctx,
              const char *status = "EXECUTADO", const char *reason = "");
bool logAutomatic(bool pumpOn, const char *source,
                  const char *status = "AUTOMATICO", const char *reason = "");
String toJson(uint16_t maxRows = 50);
using HistoryLineCallback = bool (*)(const String &jsonLine, void *userData);
uint16_t forEachRecent(uint16_t maxRows, HistoryLineCallback callback,
                       void *userData = nullptr, uint32_t *totalRows = nullptr);
bool clear();
uint32_t count();
const char *filePath();
} // namespace PumpHistory
