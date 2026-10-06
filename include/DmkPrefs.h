#pragma once
// Seleciona a memoria persistente correta para cada placa.
// ESP32/S3: Preferences (NVS). ESP8266: emulacao em LittleFS (mesmo formato
// usado pela versao 1.0.0, para nao perder configuracoes ja gravadas).
#if defined(DMK_BOARD_ESP8266)
#if defined(DMK_ROLE_SERVE)
#include "PreferencesCompat.h"
#else
#include "DmkPreferencesCompat.h"
#endif
#else
#include <Preferences.h>
#endif
