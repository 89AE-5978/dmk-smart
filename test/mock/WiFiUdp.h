#pragma once
#include "Arduino.h"
class WiFiUDP {
  int pos = 0, cur = -1;
 public:
  uint8_t begin(uint16_t);
  void stop() {}
  int beginPacket(IPAddress, uint16_t);
  size_t write(const uint8_t *d, size_t n);
  int endPacket();
  int parsePacket();
  int read(uint8_t *b, size_t n);
  IPAddress remoteIP();
  uint16_t remotePort() { return 47800; }
};
