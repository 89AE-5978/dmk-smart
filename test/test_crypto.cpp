#include "DmkCrypto.h"
#include <cstdio>
#include <cstring>
#include <string>
using namespace DmkCrypto;
static std::string hx(const uint8_t*b,size_t n){char o[200];toHex(b,n,o);return o;}
int fails=0;
void check(const char*name,std::string got,const char*exp){bool ok=got==exp;printf("%s %s\n",ok?"OK  ":"FAIL",name);if(!ok){printf("  got %s\n  exp %s\n",got.c_str(),exp);fails++;}}
int main(){
 uint8_t o[32];
 sha256("abc",3,o);check("sha abc",hx(o,32),"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
 sha256("",0,o);check("sha empty",hx(o,32),"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
 const char*m="abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";sha256(m,strlen(m),o);check("sha 448",hx(o,32),"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
 Sha256 s;s.begin();for(int i=0;i<1000000;i++){s.update("a",1);}s.finish(o);check("sha million a",hx(o,32),"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
 hmacSha256((const uint8_t*)"Jefe",4,"what do ya want for nothing?",28,o);check("hmac rfc4231 #2",hx(o,32),"5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
 uint8_t k131[131];memset(k131,0xaa,131);const char*d6="Test Using Larger Than Block-Size Key - Hash Key First";hmacSha256(k131,131,d6,strlen(d6),o);check("hmac rfc4231 #6",hx(o,32),"60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
 uint8_t a[32],b[32],pa[32],pb[32],s1[32],s2[32];
 fromHex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a",a,32);
 fromHex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb",b,32);
 x25519Base(pa,a);check("x25519 alice pub",hx(pa,32),"8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
 x25519Base(pb,b);check("x25519 bob pub",hx(pb,32),"de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
 x25519(s1,a,pb);x25519(s2,b,pa);check("x25519 shared A",hx(s1,32),"4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");check("x25519 shared B",hx(s2,32),"4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
 uint8_t sc[32],u[32];fromHex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",sc,32);fromHex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",u,32);x25519(o,sc,u);check("x25519 rfc vec1",hx(o,32),"c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
 uint8_t data[70];for(int i=0;i<70;i++)data[i]=i;uint8_t key[32]={1};streamXor(key,"t",5,data,70);streamXor(key,"t",5,data,70);bool ok=true;for(int i=0;i<70;i++)ok&=data[i]==i;check("stream roundtrip",ok?"1":"0","1");
 return fails;
}
