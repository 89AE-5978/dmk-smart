#pragma once
#if defined(DMK_BOARD_ESP8266)
#include <Arduino.h>
#include <LittleFS.h>

class Preferences {
public:
  Preferences() : readOnly_(false), opened_(false) {}

  bool begin(const char *name, bool readOnly = false) {
    if (!name || !*name) return false;
    ns_ = sanitize(String(name));
    readOnly_ = readOnly;
    opened_ = true;
    return true;
  }

  void end() { opened_ = false; }

  bool isKey(const char *key) {
    if (!opened_ || !key) return false;
    return LittleFS.exists(keyPath(key));
  }

  size_t putBytes(const char *key, const void *value, size_t len) {
    if (!opened_ || readOnly_ || !key || !value) return 0;
    String path = keyPath(key);
    File f = LittleFS.open(path, "w");
    if (!f) return 0;
    size_t n = f.write(reinterpret_cast<const uint8_t*>(value), len);
    f.close();
    rememberKey(key);
    return n;
  }

  size_t getBytes(const char *key, void *buf, size_t maxLen) {
    if (!opened_ || !key || !buf) return 0;
    File f = LittleFS.open(keyPath(key), "r");
    if (!f) return 0;
    size_t n = f.read(reinterpret_cast<uint8_t*>(buf), maxLen);
    f.close();
    return n;
  }

  size_t getBytesLength(const char *key) {
    if (!opened_ || !key) return 0;
    File f = LittleFS.open(keyPath(key), "r");
    if (!f) return 0;
    size_t n = f.size();
    f.close();
    return n;
  }

  size_t putString(const char *key, const String &value) {
    return putBytes(key, value.c_str(), value.length());
  }

  String getString(const char *key, const String &fallback = String()) {
    size_t len = getBytesLength(key);
    if (len == 0) return fallback;
    File f = LittleFS.open(keyPath(key), "r");
    if (!f) return fallback;
    String out;
    out.reserve(len + 1);
    while (f.available()) out += char(f.read());
    f.close();
    return out;
  }

  bool putBool(const char *key, bool value) { return putPod(key, value); }
  bool getBool(const char *key, bool fallback = false) { return getPod(key, fallback); }
  size_t putUShort(const char *key, uint16_t value) { return putPodSize(key, value); }
  uint16_t getUShort(const char *key, uint16_t fallback = 0) { return getPod(key, fallback); }
  size_t putULong(const char *key, uint32_t value) { return putPodSize(key, value); }
  uint32_t getULong(const char *key, uint32_t fallback = 0) { return getPod(key, fallback); }
  size_t putFloat(const char *key, float value) { return putPodSize(key, value); }
  float getFloat(const char *key, float fallback = 0.0f) { return getPod(key, fallback); }

  bool clear() {
    if (!opened_ || readOnly_) return false;
    String idx = indexPath();
    File f = LittleFS.open(idx, "r");
    if (f) {
      while (f.available()) {
        String k = f.readStringUntil('\n');
        k.trim();
        if (k.length()) LittleFS.remove(keyPath(k.c_str()));
      }
      f.close();
    }
    LittleFS.remove(idx);
    return true;
  }

private:
  String ns_;
  bool readOnly_;
  bool opened_;

  static String sanitize(String s) {
    for (size_t i=0;i<s.length();++i) {
      char c=s[i];
      if (!isalnum((unsigned char)c) && c!='_' && c!='-') s.setCharAt(i,'_');
    }
    return s;
  }
  String keyPath(const char *key) const {
    return String("/p_") + ns_ + "_" + sanitize(String(key)) + ".bin";
  }
  String indexPath() const { return String("/p_") + ns_ + "_keys.txt"; }
  void rememberKey(const char *key) {
    String k=sanitize(String(key));
    File r=LittleFS.open(indexPath(), "r");
    bool found=false;
    if (r) {
      while(r.available()) { String line=r.readStringUntil('\n'); line.trim(); if(line==k){found=true;break;} }
      r.close();
    }
    if (!found) { File a=LittleFS.open(indexPath(), "a"); if(a){a.println(k);a.close();} }
  }
  template<typename T> size_t putPodSize(const char *key, const T &v) { return putBytes(key, &v, sizeof(T)); }
  template<typename T> bool putPod(const char *key, const T &v) { return putBytes(key, &v, sizeof(T)) == sizeof(T); }
  template<typename T> T getPod(const char *key, const T &fallback) {
    T v{}; return getBytes(key, &v, sizeof(T)) == sizeof(T) ? v : fallback;
  }
};
#endif
