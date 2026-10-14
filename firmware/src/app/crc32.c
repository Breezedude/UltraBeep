#include "crc32.h"

uint32_t crc32_update(uint32_t crc, const void *data, size_t n)
{
  const uint8_t *p = (const uint8_t *)data;
  uint32_t c = crc ^ 0xFFFFFFFFU;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) {
      c = (c & 1U) ? 0xEDB88320U ^ (c >> 1) : c >> 1;
    }
  }
  return c ^ 0xFFFFFFFFU;
}

uint32_t crc32(const void *data, size_t n)
{
  return crc32_update(0, data, n);
}
