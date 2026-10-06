#pragma once

#if defined(DMK_BOARD_ESP8266)

#include <Arduino.h>
#include <LittleFS.h>

class Preferences
{
public:
  bool begin(const char *name, bool readOnly = false)
  {
    namespace_ = sanitize(name ? String(name) : String("dmk"));
    readOnly_ = readOnly;
    return true;
  }

  void end() {}

  bool isKey(const char *key)
  {
    return LittleFS.exists(pathFor(key));
  }

  String getString(const char *key, const String &defaultValue = String())
  {
    File f = LittleFS.open(pathFor(key), "r");
    if (!f)
      return defaultValue;
    String value = f.readString();
    f.close();
    return value;
  }

  bool getBool(const char *key, bool defaultValue = false)
  {
    String value = getString(key, defaultValue ? "1" : "0");
    value.trim();
    return value == "1" || value.equalsIgnoreCase("true");
  }

  uint8_t getUChar(const char *key, uint8_t defaultValue = 0)
  {
    String value = getString(key, String(defaultValue));
    return (uint8_t)value.toInt();
  }

  uint16_t getUShort(const char *key, uint16_t defaultValue = 0)
  {
    String value = getString(key, String(defaultValue));
    long parsed = value.toInt();
    if (parsed < 0)
      parsed = 0;
    if (parsed > 65535)
      parsed = 65535;
    return (uint16_t)parsed;
  }

  size_t putString(const char *key, const String &value)
  {
    return writeValue(key, value);
  }

  size_t putBool(const char *key, bool value)
  {
    return writeValue(key, value ? "1" : "0");
  }

  size_t putUChar(const char *key, uint8_t value)
  {
    return writeValue(key, String(value));
  }

  size_t putUShort(const char *key, uint16_t value)
  {
    return writeValue(key, String(value));
  }

private:
  String namespace_ = "dmk";
  bool readOnly_ = false;

  static String sanitize(String value)
  {
    String out;
    for (size_t i = 0; i < value.length(); i++)
    {
      char c = value[i];
      if ((c >= 'a' && c <= 'z') ||
          (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') ||
          c == '_' || c == '-')
        out += c;
      else
        out += '_';
    }
    return out;
  }

  String pathFor(const char *key) const
  {
    String safeKey = sanitize(key ? String(key) : String("key"));
    return String("/p_") + namespace_ + "_" + safeKey;
  }

  size_t writeValue(const char *key, const String &value)
  {
    if (readOnly_)
      return 0;

    File f = LittleFS.open(pathFor(key), "w");
    if (!f)
      return 0;

    size_t written = f.print(value);
    f.close();
    return written;
  }
};

#endif
