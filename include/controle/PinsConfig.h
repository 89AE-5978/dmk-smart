#pragma once

// ============================================================================
// DMK CONTROLE REMOTO - PINOS POR PLACA
// Botoes ligados entre GPIO e GND usando INPUT_PULLUP.
// BOMBA + LUZ juntos por 3 s = parear com o SERVE.
// ============================================================================

#if defined(DMK_BOARD_ESP32S3)

// ESP32-S3 DevKitC-1
static const uint8_t PIN_PUMP_BUTTON = 4;
static const uint8_t PIN_LIGHT_BUTTON = 5;
static const uint8_t PIN_MENU_BUTTON = 6;
static const uint8_t PIN_PUMP_LED = 7;
static const uint8_t PIN_LIGHT_LED = 8;
static const uint8_t PIN_I2C_SDA = 15;
static const uint8_t PIN_I2C_SCL = 16;
// LED de status: RGB da placa (GPIO48 na DevKitC-1 v1.0; v1.1 = 38).
static const int PIN_STATUS_LED = 48;
static const bool STATUS_LED_ACTIVE_LOW = false;
static const bool STATUS_LED_RGB = true;

#elif defined(DMK_BOARD_ESP8266)

// ESP8266MOD / ESP-12E / NodeMCU.
// Evita GPIO0 e GPIO15 nos botoes porque participam do boot.
// GPIO2 = LED azul da placa (status). GPIO15 tem pull-down: LED da luz
// ligado do pino ao GND (com resistor) nao atrapalha o boot.
static const uint8_t PIN_PUMP_BUTTON = 14;  // D5
static const uint8_t PIN_LIGHT_BUTTON = 12; // D6
static const uint8_t PIN_MENU_BUTTON = 13;  // D7
static const uint8_t PIN_PUMP_LED = 16;     // D0
static const uint8_t PIN_LIGHT_LED = 15;    // D8 (na 1.0.0 era D4)
static const uint8_t PIN_I2C_SDA = 4;       // D2
static const uint8_t PIN_I2C_SCL = 5;       // D1
static const int PIN_STATUS_LED = 2;        // D4 - LED azul da placa
static const bool STATUS_LED_ACTIVE_LOW = true;
static const bool STATUS_LED_RGB = false;

#elif defined(DMK_BOARD_ESP32)

// ESP32 Dev Module
static const uint8_t PIN_PUMP_BUTTON = 27;
static const uint8_t PIN_LIGHT_BUTTON = 32;
static const uint8_t PIN_MENU_BUTTON = 33;
static const uint8_t PIN_PUMP_LED = 25;
static const uint8_t PIN_LIGHT_LED = 26;
static const uint8_t PIN_I2C_SDA = 21;
static const uint8_t PIN_I2C_SCL = 22;
static const int PIN_STATUS_LED = 2;        // LED azul da placa
static const bool STATUS_LED_ACTIVE_LOW = false;
static const bool STATUS_LED_RGB = false;

#else
#error "Placa DMK nao definida. Use DMK_BOARD_ESP8266, DMK_BOARD_ESP32 ou DMK_BOARD_ESP32S3."
#endif
