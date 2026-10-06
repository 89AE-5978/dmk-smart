#pragma once
#include <Arduino.h>
#include <time.h>

namespace DmkClock {
bool begin(const char *timezone = "BRT3");
void update(bool wifiConnected);
bool isValid();
time_t nowEpoch();
String localDateTime();
String localIso();
const char *source();
bool setManual(int year, int month, int day,
               int hour, int minute, int second = 0);
uint32_t lastNtpRequestMillis();
String statusJson(bool wifiConnected);
} // namespace DmkClock
