// Compilado duas vezes: -DSIDE=S (SERVE) e -DSIDE=C (CONTROLE)
#include "DmkLink.h"
#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
#define FN(x) CAT(SIDE_PREFIX, x)
extern void FN(onFrame)(const DmkLink::Rx &rx);
bool FN(begin)(const char *name) { return DmkLink::begin(name, "1.1.0", FN(onFrame)); }
void FN(loop)() { DmkLink::loop(); }
bool FN(hasKey)() { return DmkLink::hasKey(); }
uint32_t FN(net)() { return DmkLink::netId(); }
uint32_t FN(rejected)() { return DmkLink::framesRejected(); }
bool FN(send)(uint8_t t, const void *p, uint8_t l, int via, const uint8_t *mac, IPAddress ip) { return DmkLink::send(t, p, l, (DmkLink::Via)via, mac, ip); }
#if defined(DMK_ROLE_SERVE)
static String share() { return String("{\"s\":\"MinhaRede\",\"p\":\"senha-wifi-123\",\"h\":\"broker.hivemq.com\",\"o\":1883,\"u\":\"admin\",\"w\":\"mqtt-pass\",\"t\":\"dmk/energia/cacibanova/verdejante/pe\"}"); }
extern void s_paired(const uint8_t mac[6], const char *name);
void FN(setup)() { DmkLink::serveSetup(share, s_paired); }
void FN(pairOpen)(uint32_t s) { DmkLink::pairOpen(s); }
uint8_t FN(peers)() { return DmkLink::peerCount(); }
void FN(rekey)() { DmkLink::regenerateKey(); }
#else
extern void c_paired(const uint8_t mac[6], uint8_t ch, const uint8_t ip[4], const char *name, const String &cfg);
void FN(setup)() { DmkLink::controleSetup(c_paired); }
bool FN(pairStart)(uint32_t s) { return DmkLink::pairStart(s); }
bool FN(pairing)() { return DmkLink::pairing(); }
bool FN(serveMac)(uint8_t *m) { return DmkLink::serveMac(m); }
uint8_t FN(chan)() { return DmkLink::savedChannel(); }
#endif
