#pragma once

// ============================================================
// DMK SERVE - PINOS POR PLACA
// ============================================================

#if defined(DMK_BOARD_ESP32S3)

// ------------------------------------------------------------
// ESP32-S3
// ------------------------------------------------------------
#define PIN_ZMPT_R 1
#define PIN_ZMPT_S 2
#define PIN_ZMPT_T 3

#define PIN_RELAY 4
#define PIN_LIGHT_RELAY 5

#define PIN_FLOW 6
#define PIN_CURRENT 7
#define PIN_TEMPERATURE 8
#define PIN_RAIN 9

#define PIN_ALARM 10
#define PIN_CONFIG_BUTTON 0

// LED de status: LED RGB da propria placa (GPIO48 na DevKitC-1 v1.0;
// na v1.1 troque para 38). Para LED azul comum use o GPIO e RGB false.
#define PIN_STATUS_LED 48
#define STATUS_LED_ACTIVE_LOW false
#define STATUS_LED_RGB true

#elif defined(DMK_BOARD_ESP8266)

// ------------------------------------------------------------
// ESP8266 / ESP-12E / NodeMCU
// ------------------------------------------------------------

// O ESP8266 possui apenas UM ADC.
// Por isso somente um sensor analógico pode ficar direto no A0.
#define PIN_ZMPT_R A0

// Não disponíveis diretamente no ESP8266.
// Precisam de ADC externo para funcionar.
#define PIN_ZMPT_S -1
#define PIN_ZMPT_T -1
#define PIN_CURRENT -1
#define PIN_RAIN -1

// GPIO digitais
#define PIN_RELAY 5       // D1
#define PIN_LIGHT_RELAY 4 // D2

#define PIN_FLOW 12        // D6
#define PIN_TEMPERATURE -1 // requer ADC externo no ESP8266
#define PIN_ALARM 13       // D7

#define PIN_CONFIG_BUTTON 0 // D3 / FLASH

// LED azul da propria placa (GPIO2 / D4), aceso em LOW.
#define PIN_STATUS_LED 2
#define STATUS_LED_ACTIVE_LOW true
#define STATUS_LED_RGB false

#elif defined(DMK_BOARD_ESP32)

// ------------------------------------------------------------
// ESP32 comum
// Sensores analógicos em ADC1 para coexistir com Wi-Fi.
// ------------------------------------------------------------
#define PIN_ZMPT_R 34
#define PIN_ZMPT_S 35
#define PIN_ZMPT_T 32

#define PIN_RELAY 26
#define PIN_LIGHT_RELAY 25

#define PIN_FLOW 27
#define PIN_CURRENT 36
#define PIN_TEMPERATURE 33
#define PIN_RAIN 39

#define PIN_ALARM 19
#define PIN_CONFIG_BUTTON 0

// LED azul da propria placa (GPIO2 na ESP32 DevKit).
#define PIN_STATUS_LED 2
#define STATUS_LED_ACTIVE_LOW false
#define STATUS_LED_RGB false

#else

#error "Placa DMK nao definida. Use DMK_BOARD_ESP32, DMK_BOARD_ESP32S3 ou DMK_BOARD_ESP8266."

#endif