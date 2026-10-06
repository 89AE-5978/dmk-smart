#pragma once
// ============================================================================
// LED AZUL DE STATUS (mesma linguagem nas duas placas)
//
//   ACESO FIXO ............ tudo certo (CONTROLE: ligado ao SERVE por ESP-NOW)
//   aceso + 1 piscada ..... CONTROLE: falando pela REDE LOCAL / SERVE: Wi-Fi sem MQTT
//   aceso + 2 piscadas .... CONTROLE: falando somente por MQTT
//   PISCA LENTO (1 s) ..... sem comunicacao / procurando
//   PISCA RAPIDO .......... modo PAREAMENTO ativo
//   2 flashes curtos ...... ponto de acesso (AP) de configuracao aberto
//   pisca-pisca muito rapido  atualizando firmware (nao desligue)
// ============================================================================
#include <Arduino.h>

namespace DmkStatusLed {

enum Pattern : uint8_t {
  LED_OFF = 0,
  LED_ON,
  LED_BLIP1,
  LED_BLIP2,
  LED_SLOW,
  LED_FAST,
  LED_AP,
  LED_OTA,
};

// pin < 0 desativa. rgb=true usa o LED RGB endereçavel da placa (ESP32-S3).
void begin(int pin, bool activeLow, bool rgb = false);
void set(Pattern pattern);
Pattern current();
void loop();
const char *describe(Pattern pattern);

} // namespace DmkStatusLed
