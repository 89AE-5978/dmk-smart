#pragma once

// ============================================================================
// DMK SMART — PERSONALIZAÇÃO DO EQUIPAMENTO
// Altere este arquivo antes da primeira gravação. Depois, o painel web pode
// sobrescrever Wi-Fi, MQTT e LCD e guardar os dados na memória do ESP.
// ============================================================================

#define USER_DEVICE_NAME "CONTROLE - DMK"

// Wi-Fi e MQTT padrão. Deixe o SSID vazio para iniciar em modo configuração.
#define DEFAULT_WIFI_SSID "DMK-SMART"
#define DEFAULT_WIFI_PASSWORD "12345678"
#define DEFAULT_MQTT_HOST "broker.hivemq.com"
#define DEFAULT_MQTT_PORT 1883
#define DEFAULT_MQTT_TOPIC "dmk/energia/cacibanova/verdejante/pe"
#define DEFAULT_MQTT_USERNAME "admin"
#define DEFAULT_MQTT_PASSWORD "12345678"
// Vazio = gera automaticamente: <nome-do-dispositivo>-<chip-id>
#define DEFAULT_MQTT_CLIENT_ID ""
// Senha do painel web e das atualizacoes (usuario "admin"). TROQUE no 1o uso.
#define DEFAULT_OTA_PASSWORD "12345678"

// Rede AP local do ESP. O AP só abre por comando (botão 30 s ou painel).
#define CONFIG_AP_PASSWORD "12345678"

// IP fixo opcional: mude para true e preencha conforme a sua rede.
#define WIFI_USE_STATIC_IP false
#define WIFI_STATIC_IP "192.168.1.250"
#define WIFI_GATEWAY "192.168.1.1"
#define WIFI_SUBNET "255.255.255.0"
#define WIFI_DNS "1.1.1.1"

// LCD padrão. O painel permite alterar e salvar essas opções depois.
#define LCD_DEFAULT_COLUMNS 16
#define LCD_DEFAULT_ROWS 4
#define LCD_DEFAULT_CUSTOM_MODE false
#define LCD_DEFAULT_LINE1 "DMK IOT SMART"
#define LCD_DEFAULT_LINE2 "BOMBA + LUZ"
#define LCD_DEFAULT_LINE3 "CONTROLE REMOTO"
#define LCD_DEFAULT_LINE4 "CONFIGURE NO APP"
