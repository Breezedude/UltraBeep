#include "handoff.h"

/* CRC-32/CKSUM as used by the POSIX cksum utility and by the stock bootloader
 * for .update images (see tools/mkupdate.py). */

static uint32_t crc_byte(uint32_t crc, uint8_t byte)
{
  crc ^= (uint32_t)byte << 24;
  for (int i = 0; i < 8; i++) {
    crc = (crc & 0x80000000U) ? (crc << 1) ^ 0x04C11DB7U : (crc << 1);
  }
  return crc;
}

void cksum_init(cksum_ctx_t *ctx)
{
  ctx->crc = 0;
  ctx->len = 0;
}

void cksum_update(cksum_ctx_t *ctx, const uint8_t *data, size_t n)
{
  uint32_t crc = ctx->crc;
  for (size_t i = 0; i < n; i++) {
    crc = crc_byte(crc, data[i]);
  }
  ctx->crc = crc;
  ctx->len += (uint32_t)n;
}

uint32_t cksum_final(const cksum_ctx_t *ctx)
{
  uint32_t crc = ctx->crc;
  for (uint32_t len = ctx->len; len != 0; len >>= 8) {
    crc = crc_byte(crc, (uint8_t)len);
  }
  return ~crc;
}
