#include "DmkStatusLed.h"

namespace DmkStatusLed {
namespace {
int ledPin = -1;
bool ledActiveLow = false;
bool ledRgb = false;
Pattern pattern = LED_SLOW;
int8_t lastLevel = -1;

void write(bool on) {
  if (ledPin < 0 || lastLevel == (int8_t)on) return;
  lastLevel = (int8_t)on;
#if !defined(DMK_BOARD_ESP8266)
  if (ledRgb) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    rgbLedWrite((uint8_t)ledPin, 0, 0, on ? 40 : 0);
#else
    neopixelWrite((uint8_t)ledPin, 0, 0, on ? 40 : 0);
#endif
    return;
  }
#endif
  digitalWrite(ledPin, (on ^ ledActiveLow) ? HIGH : LOW);
}

// Retorna aceso/apagado para o instante t (ms) dentro do padrao.
bool levelAt(uint32_t t) {
  switch (pattern) {
  case LED_OFF: return false;
  case LED_ON: return true;
  case LED_SLOW: return (t % 1000) < 500;
  case LED_FAST: return (t % 200) < 100;
  case LED_OTA: return (t % 80) < 40;
  case LED_BLIP1: {
    uint32_t p = t % 2000;
    return !(p >= 1700 && p < 1820);
  }
  case LED_BLIP2: {
    uint32_t p = t % 2000;
    return !((p >= 1500 && p < 1620) || (p >= 1760 && p < 1880));
  }
  case LED_AP: {
    uint32_t p = t % 2000;
    return (p < 90) || (p >= 250 && p < 340);
  }
  }
  return false;
}
} // namespace

void begin(int pin, bool activeLow, bool rgb) {
  ledPin = pin;
  ledActiveLow = activeLow;
  ledRgb = rgb;
  lastLevel = -1;
  if (ledPin < 0) return;
  if (!ledRgb) pinMode(ledPin, OUTPUT);
  write(false);
}

void set(Pattern p) { pattern = p; }
Pattern current() { return pattern; }

void loop() {
  if (ledPin < 0) return;
  write(levelAt(millis()));
}

const char *describe(Pattern p) {
  switch (p) {
  case LED_ON: return "aceso fixo";
  case LED_BLIP1: return "aceso com 1 piscada";
  case LED_BLIP2: return "aceso com 2 piscadas";
  case LED_SLOW: return "piscando lento";
  case LED_FAST: return "piscando rapido (pareamento)";
  case LED_AP: return "2 flashes (AP aberto)";
  case LED_OTA: return "atualizando firmware";
  default: return "apagado";
  }
}

} // namespace DmkStatusLed
