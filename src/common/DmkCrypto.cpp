#include "DmkCrypto.h"
#include <string.h>

#if defined(ARDUINO)
#include <Arduino.h>
#if defined(ESP8266)
#include <user_interface.h>
#else
#include <esp_system.h>
#if __has_include(<esp_random.h>)
#include <esp_random.h>
#endif
#endif
#else
#include <stdlib.h>
#endif

namespace DmkCrypto {

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static inline uint32_t ror(uint32_t x, uint8_t n) { return (x >> n) | (x << (32 - n)); }

static void compress(uint32_t h[8], const uint8_t block[64]) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
           ((uint32_t)block[i * 4 + 2] << 8) | block[i * 4 + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = hh + S1 + ch + K[i] + w[i];
    uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
    uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + mj;
    hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void Sha256::begin() {
  static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(h, init, sizeof(h));
  total = 0;
  used = 0;
}

void Sha256::update(const void *data, size_t len) {
  const uint8_t *p = static_cast<const uint8_t *>(data);
  total += len;
  while (len > 0) {
    if (used == 0 && len >= 64) {
      compress(h, p);
      p += 64;
      len -= 64;
      continue;
    }
    size_t take = 64 - used;
    if (take > len) take = len;
    memcpy(buf + used, p, take);
    used += (uint8_t)take;
    p += take;
    len -= take;
    if (used == 64) {
      compress(h, buf);
      used = 0;
    }
  }
}

void Sha256::finish(uint8_t out[32]) {
  uint64_t bits = total * 8ULL;
  uint8_t pad = 0x80;
  update(&pad, 1);
  uint8_t zero = 0;
  while (used != 56) update(&zero, 1);
  uint8_t lenBytes[8];
  for (int i = 0; i < 8; i++) lenBytes[i] = (uint8_t)(bits >> (56 - 8 * i));
  update(lenBytes, 8);
  for (int i = 0; i < 8; i++) {
    out[i * 4] = (uint8_t)(h[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
    out[i * 4 + 3] = (uint8_t)h[i];
  }
}

void sha256(const void *data, size_t len, uint8_t out[32]) {
  Sha256 s;
  s.begin();
  s.update(data, len);
  s.finish(out);
}

void HmacSha256::begin(const uint8_t *key, size_t keyLen) {
  uint8_t k[64];
  memset(k, 0, sizeof(k));
  if (keyLen > 64) sha256(key, keyLen, k);
  else if (keyLen) memcpy(k, key, keyLen);
  uint8_t pad[64];
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
  inner.begin();
  inner.update(pad, 64);
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
  outer.begin();
  outer.update(pad, 64);
  memset(k, 0, sizeof(k));
}

void HmacSha256::update(const void *data, size_t len) { inner.update(data, len); }

void HmacSha256::finish(uint8_t out[32]) {
  uint8_t ih[32];
  inner.finish(ih);
  outer.update(ih, 32);
  outer.finish(out);
}

void hmacSha256(const uint8_t *key, size_t keyLen, const void *data, size_t len, uint8_t out[32]) {
  HmacSha256 m;
  m.begin(key, keyLen);
  m.update(data, len);
  m.finish(out);
}

// ---------------------------------------------------------------------------
// X25519 (baseado no TweetNaCl, dominio publico)
// ---------------------------------------------------------------------------
typedef int64_t gf[16];
static const gf C121665 = {0xDB41, 1};

static void car25519(gf o) {
  for (int i = 0; i < 16; i++) {
    o[i] += ((int64_t)1 << 16);
    int64_t c = o[i] >> 16;
    o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
    o[i] -= c * ((int64_t)1 << 16);
  }
}

static void sel25519(gf p, gf q, int b) {
  int64_t c = ~((int64_t)b - 1);
  for (int i = 0; i < 16; i++) {
    int64_t t = c & (p[i] ^ q[i]);
    p[i] ^= t;
    q[i] ^= t;
  }
}

static void pack25519(uint8_t *o, const gf n) {
  gf m, t;
  for (int i = 0; i < 16; i++) t[i] = n[i];
  car25519(t);
  car25519(t);
  car25519(t);
  for (int j = 0; j < 2; j++) {
    m[0] = t[0] - 0xffed;
    for (int i = 1; i < 15; i++) {
      m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
      m[i - 1] &= 0xffff;
    }
    m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
    int b = (int)((m[15] >> 16) & 1);
    m[14] &= 0xffff;
    sel25519(t, m, 1 - b);
  }
  for (int i = 0; i < 16; i++) {
    o[2 * i] = (uint8_t)(t[i] & 0xff);
    o[2 * i + 1] = (uint8_t)(t[i] >> 8);
  }
}

static void unpack25519(gf o, const uint8_t *n) {
  for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
  o[15] &= 0x7fff;
}

static void fadd(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void fsub(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }

static void fmul(gf o, const gf a, const gf b) {
  int64_t t[31];
  for (int i = 0; i < 31; i++) t[i] = 0;
  for (int i = 0; i < 16; i++)
    for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
  for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
  for (int i = 0; i < 16; i++) o[i] = t[i];
  car25519(o);
  car25519(o);
}

static void fsq(gf o, const gf a) { fmul(o, a, a); }

static void inv25519(gf o, const gf in) {
  gf c;
  for (int a = 0; a < 16; a++) c[a] = in[a];
  for (int a = 253; a >= 0; a--) {
    fsq(c, c);
    if (a != 2 && a != 4) fmul(c, c, in);
#if defined(ARDUINO)
    if ((a & 63) == 0) yield();
#endif
  }
  for (int a = 0; a < 16; a++) o[a] = c[a];
}

void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
  uint8_t z[32];
  gf x, a, b, c, d, e, f;
  for (int i = 0; i < 31; i++) z[i] = scalar[i];
  z[31] = (scalar[31] & 127) | 64;
  z[0] &= 248;
  unpack25519(x, point);
  for (int i = 0; i < 16; i++) {
    b[i] = x[i];
    d[i] = a[i] = c[i] = 0;
  }
  a[0] = d[0] = 1;
  for (int i = 254; i >= 0; --i) {
    int r = (z[i >> 3] >> (i & 7)) & 1;
    sel25519(a, b, r);
    sel25519(c, d, r);
    fadd(e, a, c);
    fsub(a, a, c);
    fadd(c, b, d);
    fsub(b, b, d);
    fsq(d, e);
    fsq(f, a);
    fmul(a, c, a);
    fmul(c, b, e);
    fadd(e, a, c);
    fsub(a, a, c);
    fsq(b, a);
    fsub(c, d, f);
    fmul(a, c, C121665);
    fadd(a, a, d);
    fmul(c, c, a);
    fmul(a, d, f);
    fmul(d, b, x);
    fsq(b, e);
    sel25519(a, b, r);
    sel25519(c, d, r);
#if defined(ARDUINO)
    if ((i & 31) == 0) yield();
#endif
  }
  inv25519(c, c);
  fmul(a, a, c);
  pack25519(out, a);
  memset(z, 0, sizeof(z));
}

void x25519Base(uint8_t out[32], const uint8_t scalar[32]) {
  uint8_t base[32] = {9};
  x25519(out, scalar, base);
}

// ---------------------------------------------------------------------------
// Utilitarios
// ---------------------------------------------------------------------------
void streamXor(const uint8_t key[32], const char *label, uint32_t nonce, uint8_t *data, size_t len) {
  uint32_t counter = 0;
  size_t done = 0;
  size_t labelLen = strlen(label);
  while (done < len) {
    uint8_t block[32];
    HmacSha256 m;
    m.begin(key, 32);
    m.update(label, labelLen);
    uint8_t meta[8] = {(uint8_t)(nonce >> 24), (uint8_t)(nonce >> 16), (uint8_t)(nonce >> 8), (uint8_t)nonce,
                       (uint8_t)(counter >> 24), (uint8_t)(counter >> 16), (uint8_t)(counter >> 8), (uint8_t)counter};
    m.update(meta, 8);
    m.finish(block);
    for (size_t i = 0; i < 32 && done < len; i++, done++) data[done] ^= block[i];
    counter++;
  }
}

bool equalConst(const uint8_t *a, const uint8_t *b, size_t len) {
  uint8_t diff = 0;
  for (size_t i = 0; i < len; i++) diff |= a[i] ^ b[i];
  return diff == 0;
}

void randomBytes(uint8_t *out, size_t len) {
  size_t i = 0;
  while (i < len) {
#if defined(ARDUINO)
#if defined(ESP8266)
    uint32_t r = os_random();
#else
    uint32_t r = esp_random();
#endif
#else
    uint32_t r = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
#endif
    for (int b = 0; b < 4 && i < len; b++, i++) out[i] = (uint8_t)(r >> (8 * b));
  }
}

void toHex(const uint8_t *in, size_t len, char *out) {
  static const char H[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[2 * i] = H[in[i] >> 4];
    out[2 * i + 1] = H[in[i] & 15];
  }
  out[2 * len] = 0;
}

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool fromHex(const char *in, uint8_t *out, size_t len) {
  if (!in || strlen(in) != len * 2) return false;
  for (size_t i = 0; i < len; i++) {
    int hi = hexVal(in[2 * i]), lo = hexVal(in[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

} // namespace DmkCrypto
