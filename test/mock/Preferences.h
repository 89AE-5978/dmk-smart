#pragma once
#include "Arduino.h"
#include <map>
std::map<std::string, std::string> &prefStore();
class Preferences {
  std::string ns;
 public:
  bool begin(const char *n, bool = false) { ns = std::to_string(SerialMock::curNode()) + ":" + n; return true; }
  void end() {}
  bool isKey(const char *k) { return prefStore().count(ns + "/" + k) > 0; }
  String getString(const char *k, const String &d = String()) { auto it = prefStore().find(ns + "/" + k); return it == prefStore().end() ? d : String(it->second); }
  size_t putString(const char *k, const String &v) { prefStore()[ns + "/" + k] = v.s; return v.length(); }
};
