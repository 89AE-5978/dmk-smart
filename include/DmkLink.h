#pragma once
// ============================================================================
// DMK SMART - enlace CONTROLE <-> SERVE
//   Prioridade 1: ESP-NOW (direto, sem roteador, ~20 ms)
//   Prioridade 2: UDP na rede local (mesmo Wi-Fi)
//   Prioridade 3: MQTT (feito no main de cada placa, topicos da versao 1.0.0)
// Todos os quadros ESP-NOW/UDP sao cifrados e autenticados (HMAC-SHA256) com a
// chave criada no pareamento (troca X25519).
// ============================================================================
#include <Arduino.h>
#include <IPAddress.h>
#include "DmkProto.h"

namespace DmkLink {

enum Via : uint8_t { VIA_NONE = 0, VIA_ESPNOW = 1, VIA_LAN = 2, VIA_MQTT = 3 };
const char *viaName(uint8_t via);   // "ESP-NOW", "REDE LOCAL", "MQTT", "SEM LINK"
const char *viaShort(uint8_t via);  // "espnow", "lan", "mqtt", "none"

struct Rx {
  uint8_t type;
  Via via;
  uint8_t mac[6];  // remetente autenticado
  IPAddress ip;    // somente VIA_LAN
  uint16_t port;
  const uint8_t *data;
  uint8_t len;
};
typedef void (*FrameHandler)(const Rx &rx);

// Chame depois de WiFi.mode(...). deviceName vai nos quadros de pareamento.
bool begin(const char *deviceName, const char *firmwareVersion, FrameHandler handler);
void loop();

bool hasKey();
uint32_t netId();
void macToText(const uint8_t mac[6], char out[18]);
bool textToMac(const String &text, uint8_t mac[6]);

// ESPNOW: mac == nullptr -> broadcast. LAN: ip == 0.0.0.0 -> broadcast da sub-rede.
bool send(uint8_t type, const void *payload, uint8_t len, Via via,
          const uint8_t *mac = nullptr, IPAddress ip = IPAddress(0, 0, 0, 0),
          uint16_t port = DmkProto::UDP_PORT);

uint8_t channel();
bool setChannel(uint8_t ch);  // so funciona com a STA desconectada
bool espNowReady();
bool udpReady();
uint32_t framesOk();
uint32_t framesRejected();

#if defined(DMK_ROLE_SERVE)
// ---- SERVE ----
typedef String (*ShareConfigFn)();
typedef void (*PairedFn)(const uint8_t mac[6], const char *name);
void serveSetup(ShareConfigFn shareConfig, PairedFn onPaired);
void pairOpen(uint32_t seconds);
void pairClose();
bool pairIsOpen();
uint32_t pairRemainingS();
uint8_t peerCount();
bool peerAt(uint8_t index, uint8_t mac[6], String &name, uint32_t &lastSeenS);
bool removePeer(const uint8_t mac[6]);
void notePeerSeen(const uint8_t mac[6]);
void regenerateKey();  // desfaz TODOS os pareamentos
#else
// ---- CONTROLE ----
typedef void (*PairResultFn)(const uint8_t serveMac[6], uint8_t channel, const uint8_t ip[4],
                             const char *serveName, const String &sharedConfigJson);
void controleSetup(PairResultFn onPaired);
bool pairStart(uint32_t seconds);
void pairStop();
bool pairing();
uint32_t pairRemainingS();
bool serveMac(uint8_t out[6]);
uint8_t savedChannel();
void saveChannel(uint8_t ch);
void forget();  // apaga chave e SERVE pareado
#endif

} // namespace DmkLink
