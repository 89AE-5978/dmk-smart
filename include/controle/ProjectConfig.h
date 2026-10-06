#pragma once

// Nome do produto, AP de configuração e tempos do sistema.
#define DEVICE_NAME "DMK"
#define CONFIG_AP_PREFIX "CONTROLE-DMK"
#define PREFERENCES_NAMESPACE "pumpremote"
#define OTA_USER "admin"

static const uint32_t BUTTON_DEBOUNCE_MS = 55;
static const uint32_t WIFI_CONNECT_MS = 15000;
static const uint32_t WIFI_RETRY_MS = 15000;
static const uint32_t CONFIG_AP_HOLD_MS = 30000;       // segurar MENU/RESET por 30 s
static const uint32_t CONFIG_AP_IDLE_TIMEOUT_MS = 300000; // fecha apos 5 min sem cliente
static const uint32_t LCD_UPDATE_MS = 350;
static const uint32_t LCD_EVENT_MS = 5000;
static const uint32_t LCD_MENU_IDLE_MS = 15000;
static const uint32_t LCD_POWER_HOLD_MS = 5000;
static const uint32_t TELEMETRY_TIMEOUT_MS = 10000;
static const uint32_t COMMAND_TIMEOUT_MS = 6000;
static const uint8_t LCD_COLS = 16;
