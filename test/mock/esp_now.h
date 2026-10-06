#pragma once
#include <cstdint>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_ESPNOW_FULL 0x3066
#define ESP_ERR_ESPNOW_EXIST 0x3067
enum { WIFI_IF_STA = 0 };
typedef struct { uint8_t *src_addr; uint8_t *des_addr; void *rx_ctrl; } esp_now_recv_info_t;
typedef void (*esp_now_recv_cb_t)(const esp_now_recv_info_t *, const uint8_t *, int);
typedef struct { uint8_t peer_addr[6]; uint8_t lmk[16]; uint8_t channel; int ifidx; bool encrypt; void *priv; } esp_now_peer_info_t;
esp_err_t esp_now_init();
esp_err_t esp_now_register_recv_cb(esp_now_recv_cb_t);
bool esp_now_is_peer_exist(const uint8_t *);
esp_err_t esp_now_add_peer(const esp_now_peer_info_t *);
esp_err_t esp_now_del_peer(const uint8_t *);
esp_err_t esp_now_fetch_peer(bool, esp_now_peer_info_t *);
esp_err_t esp_now_send(const uint8_t *, const uint8_t *, size_t);
