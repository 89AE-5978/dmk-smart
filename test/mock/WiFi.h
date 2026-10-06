#pragma once
#include "Arduino.h"
enum { WL_CONNECTED = 3, WL_DISCONNECTED = 6 };
struct WiFiMock {
  void macAddress(uint8_t *m);
  int status();
  IPAddress localIP();
  IPAddress broadcastIP() { return IPAddress(192, 168, 0, 255); }
  void setSleep(bool) {}
};
extern WiFiMock WiFi;
