#pragma once

// Valores iniciais. Configurações feitas pelo painel são salvas na memória NVS.
#define DEFAULT_DEVICE_NAME "DMK-SERVE"
#define DEFAULT_WIFI_SSID ""
#define DEFAULT_WIFI_PASSWORD ""
#define DEFAULT_MQTT_HOST "broker.hivemq.com"
#define DEFAULT_MQTT_PORT 1883
#define DEFAULT_MQTT_USER "admin"
#define DEFAULT_MQTT_PASSWORD "12345678"
#define DEFAULT_MQTT_TOPIC "dmk/energia/cacibanova/verdejante/pe"
#define CONFIG_AP_PREFIX "DMK-SERVE-"
#define CONFIG_AP_PASSWORD "12345678"
#define OTA_USER "admin"
// Senha do painel web e das atualizacoes (usuario "admin"). TROQUE no 1o uso.
#define DEFAULT_OTA_PASSWORD "12345678"
#define WIFI_CONNECT_TIMEOUT_MS 20000UL
#define WIFI_RETRY_MS 30000UL
#define CONFIG_BUTTON_HOLD_MS 30000UL
#define CONFIG_AP_IDLE_TIMEOUT_MS 300000UL
