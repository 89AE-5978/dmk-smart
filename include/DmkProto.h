#pragma once
// ============================================================================
// DMK SMART - protocolo entre CONTROLE e SERVE
// O MESMO quadro binario trafega por ESP-NOW (prioridade 1) e por UDP na rede
// local (prioridade 2). MQTT (prioridade 3) continua usando os topicos de texto
// da versao 1.0.0, para manter o aplicativo remoto funcionando sem mudancas.
//
// Quadro autenticado:
//   Header (22 bytes) | payload CIFRADO | tag HMAC-SHA256 truncada (8 bytes)
//   - net  : identifica o sistema (4 primeiros bytes de SHA256(chave))
//   - boot : contador de reinicios do remetente (salvo na memoria)
//   - seq  : sequencia dentro do boot  -> (boot,seq) bloqueia repeticao
//   - CMD leva tambem a "sessao" atual do SERVE (muda a cada boot e a cada
//     10 minutos): comando gravado e reenviado depois e recusado.
// Quadros de pareamento usam troca de chaves X25519 + HMAC proprios.
// ============================================================================
#include <stdint.h>

namespace DmkProto {

static const uint8_t MAGIC0 = 'D';
static const uint8_t MAGIC1 = 'K';
static const uint8_t VERSION = 1;
static const uint16_t UDP_PORT = 47800;
static const uint8_t TAG_LEN = 8;
static const uint8_t MAX_FRAME = 250;  // limite ESP-NOW v1
static const uint8_t KEY_LEN = 16;

enum Type : uint8_t {
  T_STATUS = 1,    // SERVE -> CONTROLE: estado + sensores
  T_CMD = 2,       // CONTROLE -> SERVE: liga/desliga
  T_ACK = 3,       // SERVE -> CONTROLE: confirmacao do comando
  T_PING = 4,      // CONTROLE -> SERVE: pede STATUS
  T_PAIR_DONE = 5, // CONTROLE -> SERVE: confirma pareamento (ja autenticado)
  T_PAIR_REQ = 20, // pareamento (sem chave ainda)
  T_PAIR_RESP = 21,
  T_PAIR_CFG = 22,
};

enum Target : uint8_t { TARGET_PUMP = 1, TARGET_LIGHT = 2 };

enum AckResult : uint8_t {
  ACK_OK = 0,
  ACK_BAD_SESSION = 1,
  ACK_BUSY = 2,
  ACK_INVALID = 3,
};

#pragma pack(push, 1)
struct Header {
  uint8_t m0, m1, ver, type;
  uint32_t net;
  uint32_t boot;
  uint32_t seq;
  uint8_t src[6];
};

// Flags do STATUS
enum : uint8_t {
  SF_PUMP = 0x01, SF_LIGHT = 0x02, SF_ALARM = 0x04, SF_PROT_OK = 0x08,
  SF_PROT_ON = 0x10, SF_WIFI = 0x20, SF_MQTT = 0x40, SF_CLOCK = 0x80,
};

struct Status {
  uint32_t session;
  uint8_t flags;
  int8_t rssi;
  uint8_t channel;
  uint8_t ip[4];
  uint16_t vR10, vS10, vT10;      // tensao x10
  uint16_t adcR, adcS, adcT;      // Vrms no ADC x10000
  uint16_t vmin10, vmax10;
  uint32_t current_mA;
  int16_t temp10;
  uint32_t flow_mLmin;
  uint32_t total_cL;              // litros x100
  uint8_t rain;
  uint8_t otaBusy;
  uint32_t uptime;
  char fw[12];
  char reason[20];
  char name[24];
};

struct Cmd {
  uint32_t session;
  uint8_t target;
  uint8_t state;
  uint16_t seq;
};

struct Ack {
  uint16_t seq;
  uint8_t target;
  uint8_t state;
  uint8_t result;
};

struct PairDone {
  char name[24];
  char fw[12];
};

// ---- pareamento (sem autenticacao de cabecalho) ----
struct PairReq {
  uint8_t pub[32];
  uint32_t reqId;
  char name[24];
  char fw[12];
};

struct PairSecret {  // cifrado dentro do PAIR_RESP
  uint8_t key[KEY_LEN];
  uint8_t serveMac[6];
  uint8_t ip[4];
  char serveName[24];
};

struct PairResp {
  uint32_t reqId;
  uint8_t pub[32];
  uint8_t channel;
  uint8_t cfgChunks;
  uint8_t enc[sizeof(PairSecret)];
  uint8_t tag[16];
};

static const uint8_t PAIR_CFG_CHUNK = 160;
struct PairCfgHead {
  uint32_t reqId;
  uint8_t idx;
  uint8_t total;
  uint8_t len;
  // seguem len bytes cifrados + 16 bytes de tag
};
#pragma pack(pop)

static const uint8_t MAX_PAYLOAD = MAX_FRAME - sizeof(Header) - TAG_LEN;
static_assert(sizeof(Status) <= MAX_PAYLOAD, "STATUS grande demais para ESP-NOW");
static_assert(sizeof(Header) + sizeof(PairResp) <= MAX_FRAME, "PAIR_RESP grande demais");
static_assert(sizeof(Header) + sizeof(PairCfgHead) + PAIR_CFG_CHUNK + 16 <= MAX_FRAME, "PAIR_CFG grande demais");

} // namespace DmkProto
