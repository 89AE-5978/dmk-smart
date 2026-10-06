#include "DmkLink.h"
#include "DmkCrypto.h"
#include "DmkPrefs.h"

#if defined(DMK_BOARD_ESP8266)
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
extern "C" {
#include <espnow.h>
#include <user_interface.h>
}
#else
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>
#include <freertos/FreeRTOS.h>
#endif

using namespace DmkProto;

namespace DmkLink {
namespace {

const char *PREF_NS = "dmklink";
const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

FrameHandler handler = nullptr;
String devName = "DMK";
String fwVersion = "";

bool keyLoaded = false;
uint8_t netKey[KEY_LEN];
uint8_t encKey[32];
uint8_t macKey[32];
uint32_t net = 0;
uint32_t bootNo = 0;
uint32_t seqNo = 0;
uint8_t ownMac[6];

bool espNowOk = false;
WiFiUDP udp;
bool udpOpen = false;
uint32_t okCount = 0, badCount = 0;

// ---- fila de recepcao ESP-NOW (callback roda na tarefa Wi-Fi) ----
struct RxSlot {
  uint8_t mac[6];
  uint8_t len;
  uint8_t data[MAX_FRAME];
};
#if defined(DMK_BOARD_ESP8266)
const uint8_t RX_SLOTS = 4;  // ESP8266: pouca RAM
#else
const uint8_t RX_SLOTS = 8;
#endif
RxSlot rxRing[RX_SLOTS];
volatile uint8_t rxHead = 0, rxTail = 0;
#if !defined(DMK_BOARD_ESP8266)
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;
#endif

void pushRx(const uint8_t *mac, const uint8_t *data, int len) {
  if (len <= 0 || len > MAX_FRAME || data == nullptr || mac == nullptr) return;
#if !defined(DMK_BOARD_ESP8266)
  portENTER_CRITICAL(&rxMux);
#endif
  uint8_t next = (uint8_t)((rxHead + 1) % RX_SLOTS);
  if (next != rxTail) {
    RxSlot &s = rxRing[rxHead];
    memcpy(s.mac, mac, 6);
    s.len = (uint8_t)len;
    memcpy(s.data, data, (size_t)len);
    rxHead = next;
  }
#if !defined(DMK_BOARD_ESP8266)
  portEXIT_CRITICAL(&rxMux);
#endif
}

bool popRx(RxSlot &out) {
  bool got = false;
#if !defined(DMK_BOARD_ESP8266)
  portENTER_CRITICAL(&rxMux);
#endif
  if (rxTail != rxHead) {
    out = rxRing[rxTail];
    rxTail = (uint8_t)((rxTail + 1) % RX_SLOTS);
    got = true;
  }
#if !defined(DMK_BOARD_ESP8266)
  portEXIT_CRITICAL(&rxMux);
#endif
  return got;
}

#if defined(DMK_BOARD_ESP8266)
void onEspNowRecv(uint8_t *mac, uint8_t *data, uint8_t len) { pushRx(mac, data, len); }
#elif ESP_IDF_VERSION_MAJOR >= 5
void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (info) pushRx(info->src_addr, data, len);
}
#else
void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) { pushRx(mac, data, len); }
#endif

// ---- anti-repeticao: janela deslizante de 64 quadros por remetente ----
// (boot,seq) precisa ser novo. Aceita pequena troca de ordem entre ESP-NOW e
// UDP, mas recusa qualquer quadro ja visto ou antigo demais.
struct Seen {
  uint8_t mac[6];
  uint32_t boot, maxSeq;
  uint64_t window;  // bit i = (maxSeq - i) ja recebido
  uint32_t at;
  bool used;
};
Seen seen[12];

bool replayOk(const uint8_t mac[6], uint32_t boot, uint32_t seq, bool commit) {
  Seen *slot = nullptr, *victim = &seen[0];
  for (auto &s : seen) {
    if (s.used && memcmp(s.mac, mac, 6) == 0) { slot = &s; break; }
    if (!s.used) { if (victim->used) victim = &s; }
    else if (victim->used && s.at < victim->at) victim = &s;
  }
  if (slot) {
    if (boot < slot->boot) return false;
    if (boot == slot->boot) {
      if (seq <= slot->maxSeq) {
        uint32_t back = slot->maxSeq - seq;
        if (back >= 64 || (slot->window & ((uint64_t)1 << back))) return false;
        if (commit) { slot->window |= ((uint64_t)1 << back); slot->at = millis(); }
        return true;
      }
    }
  }
  if (!commit) return true;
  if (!slot) {
    slot = victim;
    memset(slot, 0, sizeof(*slot));
    memcpy(slot->mac, mac, 6);
    slot->used = true;
    slot->boot = boot;
    slot->maxSeq = seq;
    slot->window = 1;
  } else if (boot > slot->boot) {
    slot->boot = boot;
    slot->maxSeq = seq;
    slot->window = 1;
  } else {
    uint32_t shift = seq - slot->maxSeq;
    slot->window = shift >= 64 ? 1 : ((slot->window << shift) | 1);
    slot->maxSeq = seq;
  }
  slot->at = millis();
  return true;
}

void deriveKeys() {
  uint8_t tmp[32];
  DmkCrypto::Sha256 s;
  s.begin(); s.update("DMK-ENC", 7); s.update(netKey, KEY_LEN); s.finish(encKey);
  s.begin(); s.update("DMK-MAC", 7); s.update(netKey, KEY_LEN); s.finish(macKey);
  s.begin(); s.update("DMK-NET", 7); s.update(netKey, KEY_LEN); s.finish(tmp);
  net = ((uint32_t)tmp[0] << 24) | ((uint32_t)tmp[1] << 16) | ((uint32_t)tmp[2] << 8) | tmp[3];
  if (net == 0) net = 1;  // 0 e reservado ao pareamento
}

bool storeKey(const uint8_t key[KEY_LEN]) {
  char hex[KEY_LEN * 2 + 1];
  DmkCrypto::toHex(key, KEY_LEN, hex);
  Preferences p;
  if (!p.begin(PREF_NS, false)) return false;
  p.putString("key", hex);
  bool ok = p.getString("key", "") == String(hex);
  p.end();
  if (ok) {
    memcpy(netKey, key, KEY_LEN);
    deriveKeys();
    keyLoaded = true;
    memset(seen, 0, sizeof(seen));
  }
  return ok;
}

void keystream(const uint8_t src[6], uint32_t boot, uint32_t seq, uint8_t *data, size_t len) {
  uint8_t block[32];
  uint8_t idx = 0;
  for (size_t done = 0; done < len; idx++) {
    DmkCrypto::HmacSha256 m;
    m.begin(encKey, 32);
    uint8_t meta[15];
    memcpy(meta, src, 6);
    memcpy(meta + 6, &boot, 4);
    memcpy(meta + 10, &seq, 4);
    meta[14] = idx;
    m.update("P", 1);
    m.update(meta, sizeof(meta));
    m.finish(block);
    for (uint8_t i = 0; i < 32 && done < len; i++, done++) data[done] ^= block[i];
  }
}

void fillHeader(Header &h, uint8_t type, uint32_t netValue) {
  h.m0 = MAGIC0; h.m1 = MAGIC1; h.ver = VERSION; h.type = type;
  h.net = netValue;
  h.boot = bootNo;
  h.seq = ++seqNo;
  memcpy(h.src, ownMac, 6);
}

bool ensurePeer(const uint8_t *mac) {
  if (!espNowOk) return false;
#if defined(DMK_BOARD_ESP8266)
  uint8_t ch = wifi_get_channel();
  if (esp_now_is_peer_exist((uint8_t *)mac) > 0) {
    esp_now_set_peer_channel((uint8_t *)mac, ch);
    return true;
  }
  return esp_now_add_peer((uint8_t *)mac, ESP_NOW_ROLE_COMBO, ch, nullptr, 0) == 0;
#else
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;  // canal atual
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  esp_err_t r = esp_now_add_peer(&peer);
  if (r == ESP_ERR_ESPNOW_FULL) {
    // Remove um par qualquer (nao broadcast) para abrir espaco.
    esp_now_peer_info_t it;
    if (esp_now_fetch_peer(true, &it) == ESP_OK) {
      do {
        if (memcmp(it.peer_addr, BROADCAST, 6) != 0) { esp_now_del_peer(it.peer_addr); break; }
      } while (esp_now_fetch_peer(false, &it) == ESP_OK);
    }
    r = esp_now_add_peer(&peer);
  }
  return r == ESP_OK || r == ESP_ERR_ESPNOW_EXIST;
#endif
}

bool rawEspNow(const uint8_t *mac, const uint8_t *data, size_t len) {
  if (!espNowOk || len > MAX_FRAME) return false;
  const uint8_t *dest = mac ? mac : BROADCAST;
  if (!ensurePeer(dest)) return false;
#if defined(DMK_BOARD_ESP8266)
  return esp_now_send((uint8_t *)dest, (uint8_t *)data, (int)len) == 0;
#else
  return esp_now_send(dest, data, len) == ESP_OK;
#endif
}

bool rawUdp(IPAddress ip, uint16_t port, const uint8_t *data, size_t len) {
  if (!udpOpen) return false;
  if (ip == IPAddress(0, 0, 0, 0)) ip = WiFi.broadcastIP();
  if (!udp.beginPacket(ip, port)) return false;
  udp.write(data, len);
  return udp.endPacket() == 1;
}

bool isZero(const uint8_t *p, size_t n) {
  uint8_t acc = 0;
  for (size_t i = 0; i < n; i++) acc |= p[i];
  return acc == 0;
}

void copyText(char *dst, size_t size, const String &src) {
  memset(dst, 0, size);
  strncpy(dst, src.c_str(), size - 1);
}

// Chave de sessao do pareamento: SHA256("DMK-PAIR-v1" | X25519 | pubC | pubS)
void pairKey(const uint8_t shared[32], const uint8_t pubC[32], const uint8_t pubS[32], uint8_t out[32]) {
  DmkCrypto::Sha256 s;
  s.begin();
  s.update("DMK-PAIR-v1", 11);
  s.update(shared, 32);
  s.update(pubC, 32);
  s.update(pubS, 32);
  s.finish(out);
}

void pairTag(const uint8_t K[32], const char *label, const uint8_t *a, size_t alen, uint8_t out16[16]) {
  uint8_t full[32];
  DmkCrypto::HmacSha256 m;
  m.begin(K, 32);
  m.update(label, strlen(label));
  m.update(a, alen);
  m.finish(full);
  memcpy(out16, full, 16);
}

#if defined(DMK_ROLE_SERVE)
// ============================ SERVE: pareamento ============================
ShareConfigFn shareFn = nullptr;
PairedFn pairedFn = nullptr;
struct PeerInfo {
  uint8_t mac[6];
  char name[24];
  uint32_t lastSeen;
  bool used;
};
const uint8_t MAX_PEERS = 8;
PeerInfo peers[MAX_PEERS];

struct {
  bool open = false;
  uint32_t until = 0;
  uint8_t priv[32], pub[32];
  bool haveReq = false;
  uint32_t reqId = 0;
  uint8_t reqPub[32];
  uint32_t lastCompute = 0;
  uint8_t frames[5][MAX_FRAME];
  uint8_t lens[5];
  uint8_t count = 0;
} sp;

void savePeers() {
  String s;
  for (auto &p : peers) {
    if (!p.used) continue;
    char m[13];
    DmkCrypto::toHex(p.mac, 6, m);
    String n = p.name;
    n.replace(";", " ");
    n.replace("|", " ");
    s += String(m) + "|" + n + ";";
  }
  Preferences pr;
  if (pr.begin(PREF_NS, false)) {
    pr.putString("peers", s);
    pr.end();
  }
}

void loadPeers() {
  memset(peers, 0, sizeof(peers));
  Preferences pr;
  if (!pr.begin(PREF_NS, true)) return;
  String s = pr.isKey("peers") ? pr.getString("peers", "") : String("");
  pr.end();
  uint8_t i = 0;
  int start = 0;
  while (i < MAX_PEERS && start < (int)s.length()) {
    int end = s.indexOf(';', start);
    if (end < 0) end = s.length();
    String item = s.substring(start, end);
    int bar = item.indexOf('|');
    if (bar == 12 && DmkCrypto::fromHex(item.substring(0, 12).c_str(), peers[i].mac, 6)) {
      copyText(peers[i].name, sizeof(peers[i].name), item.substring(13));
      peers[i].used = true;
      i++;
    }
    start = end + 1;
  }
}

PeerInfo *findPeer(const uint8_t mac[6]) {
  for (auto &p : peers)
    if (p.used && memcmp(p.mac, mac, 6) == 0) return &p;
  return nullptr;
}

void addOrUpdatePeer(const uint8_t mac[6], const char *name) {
  PeerInfo *p = findPeer(mac);
  bool changed = false;
  if (!p) {
    for (auto &q : peers)
      if (!q.used) { p = &q; break; }
    if (!p) {  // lista cheia: substitui o mais antigo
      p = &peers[0];
      for (auto &q : peers)
        if (q.lastSeen < p->lastSeen) p = &q;
    }
    memset(p, 0, sizeof(*p));
    memcpy(p->mac, mac, 6);
    p->used = true;
    changed = true;
  }
  if (name && *name && strncmp(p->name, name, sizeof(p->name)) != 0) {
    copyText(p->name, sizeof(p->name), String(name));
    changed = true;
  }
  p->lastSeen = millis();
  if (changed) savePeers();
  ensurePeer(mac);
}

void buildPairFrames(const uint8_t K[32]) {
  sp.count = 0;
  // Configuracao de rede compartilhada (Wi-Fi/MQTT), em ate 4 pedacos.
  String cfg = shareFn ? shareFn() : String("");
  uint8_t chunks = 0;
  if (cfg.length() > 0 && cfg.length() <= (size_t)PAIR_CFG_CHUNK * 4)
    chunks = (uint8_t)((cfg.length() + PAIR_CFG_CHUNK - 1) / PAIR_CFG_CHUNK);

  // PAIR_RESP
  uint8_t *f = sp.frames[0];
  Header h;
  fillHeader(h, T_PAIR_RESP, 0);
  PairResp r;
  memset(&r, 0, sizeof(r));
  r.reqId = sp.reqId;
  memcpy(r.pub, sp.pub, 32);
  r.channel = channel();
  r.cfgChunks = chunks;
  PairSecret sec;
  memset(&sec, 0, sizeof(sec));
  memcpy(sec.key, netKey, KEY_LEN);
  memcpy(sec.serveMac, ownMac, 6);
  IPAddress ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP() : IPAddress(0, 0, 0, 0);
  for (int i = 0; i < 4; i++) sec.ip[i] = ip[i];
  copyText(sec.serveName, sizeof(sec.serveName), devName);
  memcpy(r.enc, &sec, sizeof(sec));
  DmkCrypto::streamXor(K, "resp", r.reqId, r.enc, sizeof(r.enc));
  uint8_t aad[4 + 1 + 1 + sizeof(r.enc)];
  memcpy(aad, &r.reqId, 4);
  aad[4] = r.channel;
  aad[5] = r.cfgChunks;
  memcpy(aad + 6, r.enc, sizeof(r.enc));
  pairTag(K, "resp", aad, sizeof(aad), r.tag);
  memcpy(f, &h, sizeof(h));
  memcpy(f + sizeof(h), &r, sizeof(r));
  sp.lens[0] = sizeof(h) + sizeof(r);
  sp.count = 1;
  memset(&sec, 0, sizeof(sec));

  // PAIR_CFG
  for (uint8_t idx = 0; idx < chunks; idx++) {
    uint8_t *cf = sp.frames[1 + idx];
    Header ch;
    fillHeader(ch, T_PAIR_CFG, 0);
    PairCfgHead ph;
    ph.reqId = sp.reqId;
    ph.idx = idx;
    ph.total = chunks;
    size_t off = (size_t)idx * PAIR_CFG_CHUNK;
    size_t n = cfg.length() - off;
    if (n > PAIR_CFG_CHUNK) n = PAIR_CFG_CHUNK;
    ph.len = (uint8_t)n;
    uint8_t *p = cf;
    memcpy(p, &ch, sizeof(ch)); p += sizeof(ch);
    memcpy(p, &ph, sizeof(ph)); p += sizeof(ph);
    memcpy(p, cfg.c_str() + off, n);
    char label[6] = {'c', 'f', 'g', (char)('0' + idx), 0, 0};
    DmkCrypto::streamXor(K, label, ph.reqId, p, n);
    uint8_t tagBuf[sizeof(PairCfgHead) + PAIR_CFG_CHUNK];
    memcpy(tagBuf, &ph, sizeof(ph));
    memcpy(tagBuf + sizeof(ph), p, n);
    pairTag(K, "cfg", tagBuf, sizeof(ph) + n, p + n);
    sp.lens[1 + idx] = (uint8_t)(sizeof(ch) + sizeof(ph) + n + 16);
    sp.count++;
  }
}

void sendPairFrames() {
  for (uint8_t i = 0; i < sp.count; i++) {
    rawEspNow(nullptr, sp.frames[i], sp.lens[i]);
    delay(4);
  }
}

void handlePairRaw(const uint8_t *mac, const uint8_t *data, uint8_t len) {
  (void)mac;
  if (!sp.open || !keyLoaded) return;
  const Header *h = reinterpret_cast<const Header *>(data);
  if (h->type != T_PAIR_REQ || len < sizeof(Header) + sizeof(PairReq)) return;
  PairReq req;
  memcpy(&req, data + sizeof(Header), sizeof(req));
  if (sp.haveReq && req.reqId == sp.reqId && memcmp(req.pub, sp.reqPub, 32) == 0) {
    sendPairFrames();  // resposta ja calculada: reenvia
    return;
  }
  if ((uint32_t)(millis() - sp.lastCompute) < 800) return;  // limita custo de CPU
  sp.lastCompute = millis();
  uint8_t shared[32], K[32];
  DmkCrypto::x25519(shared, sp.priv, req.pub);
  if (isZero(shared, 32)) return;
  pairKey(shared, req.pub, sp.pub, K);
  memset(shared, 0, sizeof(shared));
  sp.haveReq = true;
  sp.reqId = req.reqId;
  memcpy(sp.reqPub, req.pub, 32);
  buildPairFrames(K);
  memset(K, 0, sizeof(K));
  char name[25];
  memcpy(name, req.name, 24);
  name[24] = 0;
  Serial.printf("[PAREAR] Pedido de '%s' recebido. Enviando chave cifrada.\n", name);
  sendPairFrames();
}

#else
// =========================== CONTROLE: pareamento ===========================
PairResultFn resultFn = nullptr;
struct {
  bool active = false;
  uint32_t until = 0;
  uint8_t priv[32], pub[32];
  uint32_t reqId = 0;
  uint8_t ch = 1;
  uint32_t lastSend = 0;
  bool haveK = false;
  uint8_t servePub[32];
  uint8_t K[32];
  bool haveResp = false;
  PairSecret secret;
  uint8_t respChannel = 0;
  uint8_t cfgTotal = 0;
  uint8_t cfgMask = 0;
  uint8_t cfgLen[4];
  char cfg[PAIR_CFG_CHUNK * 4 + 1];
} cp;

uint8_t storedServeMac[6];
bool haveServeMac = false;
uint8_t storedChannel = 0;

void finishPairing() {
  // Grava chave e SERVE pareado.
  if (!storeKey(cp.secret.key)) {
    Serial.println("[PAREAR] ERRO ao gravar a chave.");
    return;
  }
  Preferences p;
  if (p.begin(PREF_NS, false)) {
    char m[13];
    DmkCrypto::toHex(cp.secret.serveMac, 6, m);
    p.putString("smac", m);
    p.putString("chan", String(cp.respChannel));
    p.end();
  }
  memcpy(storedServeMac, cp.secret.serveMac, 6);
  haveServeMac = true;
  storedChannel = cp.respChannel;
  String cfg;
  if (cp.cfgTotal) {
    for (uint8_t i = 0; i < cp.cfgTotal; i++) cfg += String(cp.cfg + (size_t)i * PAIR_CFG_CHUNK).substring(0, cp.cfgLen[i]);
  }
  char serveName[25];
  memcpy(serveName, cp.secret.serveName, 24);
  serveName[24] = 0;
  cp.active = false;
  // Confirma ao SERVE (ja com a chave nova).
  PairDone done;
  copyText(done.name, sizeof(done.name), devName);
  copyText(done.fw, sizeof(done.fw), fwVersion);
  for (int i = 0; i < 3; i++) {
    send(T_PAIR_DONE, &done, sizeof(done), VIA_ESPNOW, storedServeMac);
    delay(15);
  }
  Serial.printf("[PAREAR] Pareado com '%s' no canal %u.\n", serveName, cp.respChannel);
  if (resultFn) resultFn(cp.secret.serveMac, cp.respChannel, cp.secret.ip, serveName, cfg);
  memset(&cp.secret, 0, sizeof(cp.secret));
  memset(cp.K, 0, sizeof(cp.K));
}

void checkPairComplete() {
  if (!cp.haveResp) return;
  uint8_t need = cp.cfgTotal ? (uint8_t)((1u << cp.cfgTotal) - 1) : 0;
  if ((cp.cfgMask & need) == need) finishPairing();
}

void handlePairRaw(const uint8_t *mac, const uint8_t *data, uint8_t len) {
  (void)mac;
  if (!cp.active) return;
  const Header *h = reinterpret_cast<const Header *>(data);
  if (h->type == T_PAIR_RESP && len >= sizeof(Header) + sizeof(PairResp)) {
    PairResp r;
    memcpy(&r, data + sizeof(Header), sizeof(r));
    if (r.reqId != cp.reqId || r.cfgChunks > 4) return;
    if (!cp.haveK || memcmp(cp.servePub, r.pub, 32) != 0) {
      uint8_t shared[32];
      DmkCrypto::x25519(shared, cp.priv, r.pub);
      if (isZero(shared, 32)) return;
      pairKey(shared, cp.pub, r.pub, cp.K);
      memset(shared, 0, sizeof(shared));
      memcpy(cp.servePub, r.pub, 32);
      cp.haveK = true;
      cp.haveResp = false;
      cp.cfgMask = 0;
    }
    uint8_t aad[4 + 1 + 1 + sizeof(r.enc)], tag[16];
    memcpy(aad, &r.reqId, 4);
    aad[4] = r.channel;
    aad[5] = r.cfgChunks;
    memcpy(aad + 6, r.enc, sizeof(r.enc));
    pairTag(cp.K, "resp", aad, sizeof(aad), tag);
    if (!DmkCrypto::equalConst(tag, r.tag, 16)) { badCount++; return; }
    if (!cp.haveResp) {
      DmkCrypto::streamXor(cp.K, "resp", r.reqId, r.enc, sizeof(r.enc));
      memcpy(&cp.secret, r.enc, sizeof(cp.secret));
      cp.respChannel = r.channel ? r.channel : channel();
      cp.cfgTotal = r.cfgChunks;
      cp.haveResp = true;
      if (WiFi.status() != WL_CONNECTED) setChannel(cp.respChannel);
      Serial.printf("[PAREAR] Resposta valida do SERVE (canal %u).\n", cp.respChannel);
    }
    checkPairComplete();
    return;
  }
  if (h->type == T_PAIR_CFG && cp.haveK && len >= sizeof(Header) + sizeof(PairCfgHead) + 16) {
    PairCfgHead ph;
    memcpy(&ph, data + sizeof(Header), sizeof(ph));
    if (ph.reqId != cp.reqId || ph.idx >= 4 || ph.total > 4 || ph.len > PAIR_CFG_CHUNK) return;
    if (len < sizeof(Header) + sizeof(ph) + ph.len + 16) return;
    const uint8_t *enc = data + sizeof(Header) + sizeof(ph);
    uint8_t tagBuf[sizeof(PairCfgHead) + PAIR_CFG_CHUNK], tag[16];
    memcpy(tagBuf, &ph, sizeof(ph));
    memcpy(tagBuf + sizeof(ph), enc, ph.len);
    pairTag(cp.K, "cfg", tagBuf, sizeof(ph) + ph.len, tag);
    if (!DmkCrypto::equalConst(tag, enc + ph.len, 16)) { badCount++; return; }
    char *dst = cp.cfg + (size_t)ph.idx * PAIR_CFG_CHUNK;
    memcpy(dst, enc, ph.len);
    char label[6] = {'c', 'f', 'g', (char)('0' + ph.idx), 0, 0};
    DmkCrypto::streamXor(cp.K, label, ph.reqId, (uint8_t *)dst, ph.len);
    cp.cfgLen[ph.idx] = ph.len;
    cp.cfgMask |= (uint8_t)(1u << ph.idx);
    checkPairComplete();
  }
}

void pairLoop() {
  if (!cp.active) return;
  if ((int32_t)(millis() - cp.until) >= 0) {
    Serial.println("[PAREAR] Tempo esgotado sem resposta do SERVE.");
    cp.active = false;
    return;
  }
  if ((uint32_t)(millis() - cp.lastSend) < 260) return;
  cp.lastSend = millis();
  bool wifiUp = WiFi.status() == WL_CONNECTED;
  if (!wifiUp && !cp.haveResp) {
    setChannel(cp.ch);
    cp.ch = (uint8_t)(cp.ch % 13 + 1);
  }
  uint8_t frame[sizeof(Header) + sizeof(PairReq)];
  Header h;
  fillHeader(h, T_PAIR_REQ, 0);
  PairReq req;
  memset(&req, 0, sizeof(req));
  memcpy(req.pub, cp.pub, 32);
  req.reqId = cp.reqId;
  copyText(req.name, sizeof(req.name), devName);
  copyText(req.fw, sizeof(req.fw), fwVersion);
  memcpy(frame, &h, sizeof(h));
  memcpy(frame + sizeof(h), &req, sizeof(req));
  rawEspNow(nullptr, frame, sizeof(frame));
}

void loadServeInfo() {
  Preferences p;
  haveServeMac = false;
  storedChannel = 0;
  if (!p.begin(PREF_NS, true)) return;
  String m = p.isKey("smac") ? p.getString("smac", "") : String("");
  String c = p.isKey("chan") ? p.getString("chan", "") : String("");
  p.end();
  haveServeMac = DmkCrypto::fromHex(m.c_str(), storedServeMac, 6);
  long ch = c.toInt();
  storedChannel = (ch >= 1 && ch <= 14) ? (uint8_t)ch : 0;
}
#endif

void handleFrame(const uint8_t *radioMac, const uint8_t *data, uint8_t len, Via via, IPAddress ip, uint16_t port) {
  if (len < sizeof(Header)) return;
  Header h;
  memcpy(&h, data, sizeof(h));
  if (h.m0 != MAGIC0 || h.m1 != MAGIC1 || h.ver != VERSION) return;
  if (memcmp(h.src, ownMac, 6) == 0) return;  // eco do proprio quadro

  if (h.type >= T_PAIR_REQ) {
    if (via == VIA_ESPNOW) handlePairRaw(radioMac, data, len);
    return;
  }
  if (!keyLoaded || h.net != net || len < sizeof(Header) + TAG_LEN) return;

  uint8_t plen = (uint8_t)(len - sizeof(Header) - TAG_LEN);
  uint8_t full[32];
  DmkCrypto::hmacSha256(macKey, 32, data, sizeof(Header) + plen, full);
  if (!DmkCrypto::equalConst(full, data + sizeof(Header) + plen, TAG_LEN)) {
    badCount++;
    return;
  }
  if (!replayOk(h.src, h.boot, h.seq, true)) {
    badCount++;
    return;
  }
  uint8_t payload[MAX_PAYLOAD];
  memcpy(payload, data + sizeof(Header), plen);
  keystream(h.src, h.boot, h.seq, payload, plen);
  okCount++;
#if defined(DMK_ROLE_SERVE)
  if (via == VIA_ESPNOW) {
    if (h.type == T_PAIR_DONE && plen >= sizeof(PairDone)) {
      PairDone d;
      memcpy(&d, payload, sizeof(d));
      d.name[23] = 0;
      bool isNew = findPeer(h.src) == nullptr;
      addOrUpdatePeer(h.src, d.name);
      if (isNew || sp.open) {
        Serial.printf("[PAREAR] Controle '%s' pareado.\n", d.name);
        if (pairedFn) pairedFn(h.src, d.name);
      }
      sp.open = false;
    } else {
      notePeerSeen(h.src);
    }
  }
#endif
  if (!handler) return;
  Rx rx;
  rx.type = h.type;
  rx.via = via;
  memcpy(rx.mac, h.src, 6);
  rx.ip = ip;
  rx.port = port;
  rx.data = payload;
  rx.len = plen;
  handler(rx);
}

void loadOrInitKey() {
  Preferences p;
  String hex;
  if (p.begin(PREF_NS, false)) {
    hex = p.isKey("key") ? p.getString("key", "") : String("");
    String b = p.isKey("boot") ? p.getString("boot", "0") : String("0");
    bootNo = (uint32_t)strtoul(b.c_str(), nullptr, 10) + 1;
    p.putString("boot", String(bootNo));
    p.end();
  }
  uint8_t key[KEY_LEN];
  if (DmkCrypto::fromHex(hex.c_str(), key, KEY_LEN)) {
    memcpy(netKey, key, KEY_LEN);
    deriveKeys();
    keyLoaded = true;
  }
#if defined(DMK_ROLE_SERVE)
  if (!keyLoaded) {
    DmkCrypto::randomBytes(key, KEY_LEN);
    if (storeKey(key)) Serial.println("[LINK] Nova chave do sistema criada.");
  }
#endif
  memset(key, 0, sizeof(key));
}

} // namespace

// ============================== API PUBLICA ==============================
const char *viaName(uint8_t via) {
  switch (via) {
  case VIA_ESPNOW: return "ESP-NOW";
  case VIA_LAN: return "REDE LOCAL";
  case VIA_MQTT: return "MQTT";
  default: return "SEM LINK";
  }
}

const char *viaShort(uint8_t via) {
  switch (via) {
  case VIA_ESPNOW: return "espnow";
  case VIA_LAN: return "lan";
  case VIA_MQTT: return "mqtt";
  default: return "none";
  }
}

void macToText(const uint8_t mac[6], char out[18]) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool textToMac(const String &text, uint8_t mac[6]) {
  String t = text;
  t.replace(":", "");
  t.replace("-", "");
  t.trim();
  return DmkCrypto::fromHex(t.c_str(), mac, 6);
}

bool begin(const char *deviceName, const char *firmwareVersion, FrameHandler h) {
  handler = h;
  devName = deviceName ? deviceName : "DMK";
  fwVersion = firmwareVersion ? firmwareVersion : "";
  WiFi.macAddress(ownMac);
  loadOrInitKey();
#if defined(DMK_ROLE_SERVE)
  loadPeers();
#else
  loadServeInfo();
#endif

#if defined(DMK_BOARD_ESP8266)
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  espNowOk = esp_now_init() == 0;
  if (espNowOk) {
    esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
    esp_now_register_recv_cb(onEspNowRecv);
  }
#else
  WiFi.setSleep(false);
  espNowOk = esp_now_init() == ESP_OK;
  if (espNowOk) esp_now_register_recv_cb(onEspNowRecv);
#endif
  if (espNowOk) {
    ensurePeer(BROADCAST);
#if defined(DMK_ROLE_SERVE)
    for (auto &p : peers)
      if (p.used) ensurePeer(p.mac);
#else
    if (haveServeMac) ensurePeer(storedServeMac);
#endif
  }
  char m[18];
  macToText(ownMac, m);
  Serial.printf("[LINK] ESP-NOW %s | MAC %s | chave %s | boot %lu\n", espNowOk ? "ATIVO" : "FALHOU", m,
                keyLoaded ? "OK" : "AUSENTE (parear)", (unsigned long)bootNo);
  return espNowOk;
}

void loop() {
  // UDP acompanha o estado da STA.
  bool sta = WiFi.status() == WL_CONNECTED;
  if (sta && !udpOpen) {
    udpOpen = udp.begin(UDP_PORT) == 1;
    if (udpOpen) Serial.printf("[LINK] UDP local ativo na porta %u\n", UDP_PORT);
  } else if (!sta && udpOpen) {
    udp.stop();
    udpOpen = false;
  }

  RxSlot slot;
  uint8_t budget = 6;
  while (budget-- && popRx(slot)) handleFrame(slot.mac, slot.data, slot.len, VIA_ESPNOW, IPAddress(0, 0, 0, 0), 0);

  if (udpOpen) {
    budget = 4;
    while (budget--) {
      int size = udp.parsePacket();
      if (size <= 0) break;
      uint8_t buf[MAX_FRAME];
      int n = udp.read(buf, sizeof(buf));
      if (size > MAX_FRAME || n <= 0) continue;
      uint8_t none[6] = {0};
      handleFrame(none, buf, (uint8_t)n, VIA_LAN, udp.remoteIP(), udp.remotePort());
    }
  }

#if defined(DMK_ROLE_SERVE)
  if (sp.open && (int32_t)(millis() - sp.until) >= 0) {
    sp.open = false;
    Serial.println("[PAREAR] Janela de pareamento encerrada.");
  }
#else
  pairLoop();
#endif
}

bool hasKey() { return keyLoaded; }
uint32_t netId() { return keyLoaded ? net : 0; }
bool espNowReady() { return espNowOk; }
bool udpReady() { return udpOpen; }
uint32_t framesOk() { return okCount; }
uint32_t framesRejected() { return badCount; }

bool send(uint8_t type, const void *payload, uint8_t len, Via via, const uint8_t *mac, IPAddress ip, uint16_t port) {
  if (!keyLoaded || len > MAX_PAYLOAD) return false;
  uint8_t frame[MAX_FRAME];
  Header h;
  fillHeader(h, type, net);
  memcpy(frame, &h, sizeof(h));
  if (len) memcpy(frame + sizeof(h), payload, len);
  keystream(ownMac, h.boot, h.seq, frame + sizeof(h), len);
  uint8_t full[32];
  DmkCrypto::hmacSha256(macKey, 32, frame, sizeof(h) + len, full);
  memcpy(frame + sizeof(h) + len, full, TAG_LEN);
  size_t total = sizeof(h) + len + TAG_LEN;
  if (via == VIA_ESPNOW) return rawEspNow(mac, frame, total);
  if (via == VIA_LAN) return rawUdp(ip, port, frame, total);
  return false;
}

uint8_t channel() {
#if defined(DMK_BOARD_ESP8266)
  return wifi_get_channel();
#else
  uint8_t primary = 0;
  wifi_second_chan_t second;
  if (esp_wifi_get_channel(&primary, &second) != ESP_OK) return 0;
  return primary;
#endif
}

bool setChannel(uint8_t ch) {
  if (ch < 1 || ch > 13) return false;
  if (WiFi.status() == WL_CONNECTED) return channel() == ch;
#if defined(DMK_BOARD_ESP8266)
  return wifi_set_channel(ch);
#else
  return esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) == ESP_OK;
#endif
}

#if defined(DMK_ROLE_SERVE)
void serveSetup(ShareConfigFn shareConfig, PairedFn onPaired) {
  shareFn = shareConfig;
  pairedFn = onPaired;
}

void pairOpen(uint32_t seconds) {
  if (!keyLoaded) loadOrInitKey();
  DmkCrypto::randomBytes(sp.priv, 32);
  DmkCrypto::x25519Base(sp.pub, sp.priv);
  sp.haveReq = false;
  sp.count = 0;
  sp.open = true;
  sp.until = millis() + seconds * 1000UL;
  Serial.printf("[PAREAR] Janela aberta por %lu s (canal %u).\n", (unsigned long)seconds, channel());
}

void pairClose() { sp.open = false; }
bool pairIsOpen() { return sp.open; }
uint32_t pairRemainingS() {
  if (!sp.open) return 0;
  int32_t left = (int32_t)(sp.until - millis());
  return left > 0 ? (uint32_t)left / 1000UL : 0;
}

uint8_t peerCount() {
  uint8_t n = 0;
  for (auto &p : peers)
    if (p.used) n++;
  return n;
}

bool peerAt(uint8_t index, uint8_t mac[6], String &name, uint32_t &lastSeenS) {
  uint8_t n = 0;
  for (auto &p : peers) {
    if (!p.used) continue;
    if (n++ == index) {
      memcpy(mac, p.mac, 6);
      name = p.name;
      lastSeenS = p.lastSeen ? (millis() - p.lastSeen) / 1000UL : 0xFFFFFFFF;
      return true;
    }
  }
  return false;
}

bool removePeer(const uint8_t mac[6]) {
  PeerInfo *p = findPeer(mac);
  if (!p) return false;
  p->used = false;
  savePeers();
#if defined(DMK_BOARD_ESP8266)
  esp_now_del_peer((uint8_t *)mac);
#else
  esp_now_del_peer(mac);
#endif
  return true;
}

void notePeerSeen(const uint8_t mac[6]) {
  PeerInfo *p = findPeer(mac);
  if (p) p->lastSeen = millis();
  else addOrUpdatePeer(mac, "");
}

void regenerateKey() {
  uint8_t key[KEY_LEN];
  DmkCrypto::randomBytes(key, KEY_LEN);
  storeKey(key);
  memset(key, 0, sizeof(key));
  for (auto &p : peers) {
    if (!p.used) continue;
#if defined(DMK_BOARD_ESP8266)
    esp_now_del_peer(p.mac);
#else
    esp_now_del_peer(p.mac);
#endif
    p.used = false;
  }
  savePeers();
  Serial.println("[LINK] Chave do sistema trocada. Todos os controles precisam parear de novo.");
}
#else
void controleSetup(PairResultFn onPaired) { resultFn = onPaired; }

bool pairStart(uint32_t seconds) {
  if (!espNowOk) return false;
  DmkCrypto::randomBytes(cp.priv, 32);
  DmkCrypto::x25519Base(cp.pub, cp.priv);
  DmkCrypto::randomBytes((uint8_t *)&cp.reqId, 4);
  cp.haveK = false;
  cp.haveResp = false;
  cp.cfgMask = 0;
  cp.cfgTotal = 0;
  cp.ch = storedChannel ? storedChannel : 1;
  cp.lastSend = 0;
  cp.until = millis() + seconds * 1000UL;
  cp.active = true;
  Serial.printf("[PAREAR] Procurando SERVE por %lu s...\n", (unsigned long)seconds);
  return true;
}

void pairStop() { cp.active = false; }
bool pairing() { return cp.active; }
uint32_t pairRemainingS() {
  if (!cp.active) return 0;
  int32_t left = (int32_t)(cp.until - millis());
  return left > 0 ? (uint32_t)left / 1000UL : 0;
}

bool serveMac(uint8_t out[6]) {
  if (!haveServeMac) return false;
  memcpy(out, storedServeMac, 6);
  return true;
}

uint8_t savedChannel() { return storedChannel; }

void saveChannel(uint8_t ch) {
  if (ch < 1 || ch > 13 || ch == storedChannel) return;
  storedChannel = ch;
  Preferences p;
  if (p.begin(PREF_NS, false)) {
    p.putString("chan", String(ch));
    p.end();
  }
}

void forget() {
  Preferences p;
  if (p.begin(PREF_NS, false)) {
    p.putString("key", "");
    p.putString("smac", "");
    p.end();
  }
  keyLoaded = false;
  haveServeMac = false;
  memset(netKey, 0, sizeof(netKey));
}
#endif

} // namespace DmkLink
