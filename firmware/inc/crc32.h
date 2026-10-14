#ifndef CRC32_H
#define CRC32_H

#include <stddef.h>
#include <stdint.h>

/* CRC-32 (zlib/PNG polynomial, reflected); check value of "123456789" = 0xCBF43926. */
uint32_t crc32(const void *data, size_t n);
/* Continues a CRC over more data: crc32_update(crc32(a), b) == crc32(a b); crc32_update(0, a) == crc32(a). */
uint32_t crc32_update(uint32_t crc, const void *data, size_t n);

#endif
