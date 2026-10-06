// Simulacao: SERVE (no 0) e CONTROLE (no 1) usando o DmkLink.cpp real.
#include "Arduino.h"
#include "DmkProto.h"
#include <vector>
#include <functional>
using namespace DmkProto;
struct Rx { uint8_t type; uint8_t via; uint8_t mac[6]; IPAddress ip; uint16_t port; const uint8_t *data; uint8_t len; };
namespace DmkLinkS { struct Rx; } namespace DmkLinkC { struct Rx; }
extern int gCur; extern uint32_t gNow; extern int gDelivered;
struct Node { uint8_t mac[6]; uint8_t ch; bool wifi; IPAddress ip; void *cb; };
extern std::function<bool(int, std::vector<uint8_t> &)> gTap;
extern std::vector<std::vector<uint8_t>> gCaptured;
// acesso aos nos do mock
struct MockNode;
void setNode(int i, const uint8_t mac[6], uint8_t ch, bool wifi, IPAddress ip);
uint8_t nodeCh(int i);
// funcoes das duas copias do DmkLink
bool s_begin(const char *); void s_loop(); bool s_hasKey(); uint32_t s_net(); uint32_t s_rejected(); bool s_send(uint8_t, const void *, uint8_t, int, const uint8_t *, IPAddress);
void s_setup(); void s_pairOpen(uint32_t); uint8_t s_peers(); void s_rekey();
bool c_begin(const char *); void c_loop(); bool c_hasKey(); uint32_t c_net(); uint32_t c_rejected(); bool c_send(uint8_t, const void *, uint8_t, int, const uint8_t *, IPAddress);
void c_setup(); bool c_pairStart(uint32_t); bool c_pairing(); bool c_serveMac(uint8_t *); uint8_t c_chan();

int fails = 0;
void check(bool ok, const char *name) { printf("%s %s\n", ok ? "OK  " : "FAIL", name); if (!ok) fails++; }

std::vector<std::pair<uint8_t, std::vector<uint8_t>>> sRx, cRx; std::vector<uint8_t> sVia, cVia;
// Os Rx das duas copias tem o mesmo layout (mesmo header compilado).
struct RxLayout { uint8_t type; uint8_t via; uint8_t mac[6]; IPAddress ip; uint16_t port; const uint8_t *data; uint8_t len; };
namespace DmkLinkS { struct Rx; void dummy(); }
void s_onFrame_impl(const RxLayout &r) { sRx.push_back({r.type, std::vector<uint8_t>(r.data, r.data + r.len)}); sVia.push_back(r.via); }
void c_onFrame_impl(const RxLayout &r) { cRx.push_back({r.type, std::vector<uint8_t>(r.data, r.data + r.len)}); cVia.push_back(r.via); }
bool cPaired = false; String cCfg; uint8_t cServeMac[6]; uint8_t cCh = 0;
void s_paired(const uint8_t *, const char *name) { printf("    SERVE: controle pareado '%s'\n", name); }
void c_paired(const uint8_t mac[6], uint8_t ch, const uint8_t *, const char *name, const String &cfg) { cPaired = true; cCfg = cfg; memcpy(cServeMac, mac, 6); cCh = ch; printf("    CONTROLE: pareado com '%s' canal %u cfg=%u bytes\n", name, ch, (unsigned)cfg.length()); }

void step(int n = 1) { for (int i = 0; i < n; i++) { gCur = 0; s_loop(); gCur = 1; c_loop(); gNow += 10; } }

int main() {
  const uint8_t SM[6] = {0x24, 0x6F, 0x28, 0xAA, 0xBB, 0x01}, CM[6] = {0x24, 0x6F, 0x28, 0xCC, 0xDD, 0x02};
  setNode(0, SM, 6, false, IPAddress(192, 168, 0, 10));
  setNode(1, CM, 1, false, IPAddress(192, 168, 0, 20));
  gCur = 0; s_setup(); s_begin("DMK-SERVE");
  gCur = 1; c_setup(); c_begin("CONTROLE - DMK");
  check(s_hasKey() && !c_hasKey(), "SERVE cria chave; controle novo sem chave");

  // ---- pareamento com varredura de canal ----
  gCur = 0; s_pairOpen(120);
  gCur = 1; c_pairStart(120);
  for (int i = 0; i < 3000 && !cPaired; i++) step();
  check(cPaired, "controle pareou (varrendo canais 1..13 ate o 6)");
  check(nodeCh(1) == 6 && cCh == 6, "controle travou no canal do SERVE");
  gCur = 0; uint32_t sn = s_net(); gCur = 1; uint32_t cn = c_net();
  check(sn == cn && sn != 0, "mesma chave (net id) nos dois lados");
  check(memcmp(cServeMac, SM, 6) == 0, "MAC do SERVE correto");
  check(cCfg.s.find("MinhaRede") != std::string::npos && cCfg.s.find("mqtt-pass") != std::string::npos, "Wi-Fi/MQTT compartilhados chegaram inteiros");
  step(20);
  gCur = 0; check(s_peers() == 1, "SERVE registrou o controle (PAIR_DONE)");

  // ---- sigilo: nenhum quadro capturado contem a senha em texto ----
  bool leak = false;
  for (auto &p : gCaptured) { std::string s(p.begin(), p.end()); if (s.find("senha-wifi-123") != std::string::npos || s.find("mqtt-pass") != std::string::npos) leak = true; }
  check(!leak, "senhas nunca aparecem em claro no ar");

  // ---- comando ESP-NOW + ACK ----
  sRx.clear(); cRx.clear();
  Cmd c{0x11223344, TARGET_PUMP, 1, 7};
  gCur = 1; c_send(T_CMD, &c, sizeof(c), 1, SM, IPAddress());
  step(2);
  check(sRx.size() == 1 && sRx[0].first == T_CMD && sRx[0].second.size() == sizeof(Cmd) && memcmp(sRx[0].second.data(), &c, sizeof(c)) == 0, "SERVE recebeu o CMD intacto (cifrado no ar)");
  Ack a{7, TARGET_PUMP, 1, ACK_OK};
  gCur = 0; s_send(T_ACK, &a, sizeof(a), 1, CM, IPAddress());
  step(2);
  check(cRx.size() == 1 && cRx[0].first == T_ACK, "CONTROLE recebeu o ACK");

  // ---- quadro capturado e reenviado (replay) ----
  std::vector<uint8_t> last = gCaptured.back();
  gCur = 0; uint32_t before = s_rejected();
  // reinjeta o ultimo CMD do controle no SERVE
  std::vector<uint8_t> cmdFrame;
  for (auto it = gCaptured.rbegin(); it != gCaptured.rend(); ++it) if ((*it)[3] == T_CMD) { cmdFrame = *it; break; }
  sRx.clear();
  extern void injectTo(int node, const uint8_t *from, std::vector<uint8_t> pkt);
  injectTo(0, CM, cmdFrame); step(2);
  gCur = 0; check(sRx.empty() && s_rejected() == before + 1, "replay do comando recusado");

  // ---- adulteracao ----
  std::vector<uint8_t> bad = cmdFrame; bad[bad.size() - 12] ^= 0x01;
  injectTo(0, CM, bad); step(2);
  gCur = 0; check(sRx.empty() && s_rejected() == before + 2, "quadro adulterado recusado");

  // ---- canal diferente nao entrega ----
  setNode(1, CM, 11, false, IPAddress(192, 168, 0, 20));
  sRx.clear(); gCur = 1; c_send(T_PING, nullptr, 0, 1, SM, IPAddress()); step(2);
  check(sRx.empty(), "ESP-NOW em canal diferente nao chega");
  setNode(1, CM, 6, false, IPAddress(192, 168, 0, 20));

  // ---- rede local (UDP) ----
  setNode(0, SM, 6, true, IPAddress(192, 168, 0, 10));
  setNode(1, CM, 6, true, IPAddress(192, 168, 0, 20));
  step(2);
  sRx.clear(); sVia.clear();
  gCur = 1; c_send(T_CMD, &c, sizeof(c), 2, nullptr, IPAddress(192, 168, 0, 10)); step(3);
  check(sRx.size() == 1 && sVia[0] == 2, "CMD pela rede local (UDP) chegou");
  cRx.clear();
  Status st{}; st.session = 99; st.flags = SF_PUMP; strcpy(st.name, "DMK-SERVE");
  gCur = 0; s_send(T_STATUS, &st, sizeof(st), 2, nullptr, IPAddress()); step(3);
  check(cRx.size() == 1 && cRx[0].first == T_STATUS && cRx[0].second.size() == sizeof(Status), "STATUS em broadcast UDP chegou");

  // ---- troca de chave bloqueia controle antigo ----
  gCur = 0; s_rekey(); uint32_t rb = s_rejected();
  sRx.clear(); gCur = 1; c_send(T_PING, nullptr, 0, 1, SM, IPAddress()); step(2);
  gCur = 0; check(sRx.empty(), "apos trocar a chave o controle antigo e ignorado");
  (void)rb;

  // ---- re-pareamento depois da troca ----
  cPaired = false; gCur = 0; s_pairOpen(60); gCur = 1; c_pairStart(60);
  for (int i = 0; i < 3000 && !cPaired; i++) step();
  gCur = 0; sn = s_net(); gCur = 1; cn = c_net();
  check(cPaired && sn == cn, "pareia de novo com a chave nova");

  printf(fails ? "\n%d FALHA(S)\n" : "\nTODOS OS TESTES PASSARAM\n", fails);
  return fails;
}
