#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

/* SHA-256 (FIPS 180-4) and HMAC-SHA-256 (RFC 2104), incremental. */
#define SHA256_SIZE  32
#define SHA256_BLOCK 64

typedef struct {
  uint32_t h[8];
  uint64_t len;
  uint8_t buf[SHA256_BLOCK];
  size_t fill;
} sha256_t;

void sha256_init(sha256_t *s);
void sha256_update(sha256_t *s, const void *data, size_t n);
void sha256_final(sha256_t *s, uint8_t out[SHA256_SIZE]);

typedef struct {
  sha256_t inner;
  uint8_t okey[SHA256_BLOCK];
} hmac_sha256_t;

void hmac_sha256_init(hmac_sha256_t *h, const uint8_t *key, size_t key_len);
void hmac_sha256_update(hmac_sha256_t *h, const void *data, size_t n);
void hmac_sha256_final(hmac_sha256_t *h, uint8_t out[SHA256_SIZE]);

#endif
