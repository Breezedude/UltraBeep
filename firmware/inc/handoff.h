#ifndef HANDOFF_H
#define HANDOFF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* POSIX cksum (CRC-32/CKSUM), the checksum of .update images. */
typedef struct {
  uint32_t crc;
  uint32_t len;
} cksum_ctx_t;

void cksum_init(cksum_ctx_t *ctx);
void cksum_update(cksum_ctx_t *ctx, const uint8_t *data, size_t n);
uint32_t cksum_final(const cksum_ctx_t *ctx);

/*
 * Hand-off to the stock bootloader: it installs 0:/ultrabip*.update after a
 * window watchdog reset. The caller holds the storage lock with the SD card
 * mounted. Returns false if there is no valid image in the root.
 */
bool handoff_prepare_update_from_root(void);
/* Unmount the SD card before calling. Never returns. */
void handoff_wwdg_reset(void) __attribute__((noreturn));

#endif
