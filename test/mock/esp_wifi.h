#pragma once
#include "esp_now.h"
typedef int wifi_second_chan_t;
#define WIFI_SECOND_CHAN_NONE 0
esp_err_t esp_wifi_get_channel(uint8_t *p, wifi_second_chan_t *s);
esp_err_t esp_wifi_set_channel(uint8_t p, wifi_second_chan_t s);
