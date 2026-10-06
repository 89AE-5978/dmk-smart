#pragma once
// ============================================================================
// DMK SMART - criptografia portatil (mesmo codigo no ESP32, ESP32-S3 e ESP8266)
//   * SHA-256 / HMAC-SHA256 (FIPS 180-4 / RFC 2104)
//   * X25519 (RFC 7748) para o pareamento seguro do controle
//   * Cifra de fluxo HMAC-CTR para os dados enviados no pareamento
// Sem dependencia de mbedTLS/BearSSL: testado no PC com os vetores oficiais.
// ============================================================================
#include <stddef.h>
#include <stdint.h>

namespace DmkCrypto {

struct Sha256 {
  uint32_t h[8];
  uint64_t total;
  uint8_t buf[64];
  uint8_t used;
  void begin();
  void update(const void *data, size_t len);
  void finish(uint8_t out[32]);
};

struct HmacSha256 {
  Sha256 inner, outer;
  void begin(const uint8_t *key, size_t keyLen);
  void update(const void *data, size_t len);
  void finish(uint8_t out[32]);
};

void sha256(const void *data, size_t len, uint8_t out[32]);
void hmacSha256(const uint8_t *key, size_t keyLen, const void *data, size_t len, uint8_t out[32]);

// X25519: out = scalar * point. Base point = 9.
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void x25519Base(uint8_t out[32], const uint8_t scalar[32]);

// XOR com fluxo HMAC(key, label|nonce|contador). Mesmo comando cifra e decifra.
void streamXor(const uint8_t key[32], const char *label, uint32_t nonce, uint8_t *data, size_t len);

// Comparacao em tempo constante.
bool equalConst(const uint8_t *a, const uint8_t *b, size_t len);

// Bytes aleatorios do gerador de hardware (RF ligado = entropia real).
void randomBytes(uint8_t *out, size_t len);

// Hex <-> binario
void toHex(const uint8_t *in, size_t len, char *out); // out precisa de 2*len+1
bool fromHex(const char *in, uint8_t *out, size_t len);

} // namespace DmkCrypto
