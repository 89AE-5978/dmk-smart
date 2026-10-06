#pragma once
#include <Arduino.h>
#include <time.h>

namespace AutomationManager {
constexpr uint8_t MAX_AUTOMATIONS = 16;
constexpr uint8_t EVERY_DAY = 0x7F;
enum Target : uint8_t { TARGET_PUMP = 1, TARGET_LIGHT = 2 };
struct Automation {
  uint32_t id;
  bool enabled;
  uint8_t target;
  bool turnOn;
  uint8_t hour;
  uint8_t minute;
  uint8_t daysMask;
  uint32_t lastRunMinuteKey;
  time_t lastRunEpoch;
  char name[32];
};
using PumpCallback = void (*)(bool turnOn, uint32_t automationId);
using LightCallback = void (*)(bool turnOn, uint32_t automationId);
bool begin(PumpCallback pumpCallback, LightCallback lightCallback);
void update();
bool save(uint32_t id, const char *name, uint8_t target, bool turnOn,
          uint8_t hour, uint8_t minute, uint8_t daysMask, bool enabled,
          uint32_t *savedId = nullptr);
bool remove(uint32_t id);
bool setEnabled(uint32_t id, bool enabled);
bool clear();
String toJson();
bool get(uint32_t id, Automation &out);
uint8_t count();
} // namespace AutomationManager
