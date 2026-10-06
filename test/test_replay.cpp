// Testa a janela anti-repeticao isolada (copia da logica do DmkLink).
#include <cstdint>
#include <cstring>
#include <cstdio>
static uint32_t fakeMillis=0; static uint32_t millis(){return ++fakeMillis;}
#define REPLAY_ONLY
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


int main(){
  int f=0; uint8_t A[6]={1,2,3,4,5,6},B[6]={9,9,9,9,9,9};
  auto T=[&](bool got,bool exp,const char*n){printf("%s %s\n",got==exp?"OK  ":"FAIL",n);if(got!=exp)f++;};
  T(replayOk(A,5,10,true),true,"primeiro quadro");
  T(replayOk(A,5,10,true),false,"repetido recusado");
  T(replayOk(A,5,12,true),true,"seq maior");
  T(replayOk(A,5,11,true),true,"fora de ordem (11 depois de 12)");
  T(replayOk(A,5,11,true),false,"11 repetido recusado");
  T(replayOk(A,5,200,true),true,"salto grande");
  T(replayOk(A,5,12,true),false,"antigo demais recusado");
  T(replayOk(A,4,999,true),false,"boot anterior recusado");
  T(replayOk(A,6,1,true),true,"novo boot aceito");
  T(replayOk(B,1,1,true),true,"outro remetente");
  T(replayOk(A,6,1,true),false,"A ainda lembrado");
  return f;
}
