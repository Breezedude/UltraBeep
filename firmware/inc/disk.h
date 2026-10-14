#ifndef DISK_H
#define DISK_H

#include <stdbool.h>
#include <stdint.h>

/*
 * FatFs drives (diskio.c):
 *   0: microSD card over SPI2 (sdcard.c)
 *   1: voice pack image (.ubpack, FAT12 with 1024-byte sectors) read through
 *      an open file on drive 0, like the stock "1:/" volume.
 */

typedef struct {
  uint8_t type; /* CT_* flags */
  uint32_t sectors;
  uint8_t cid[16];
  uint8_t csd[16];
} sd_info_t;

#define CT_MMC   0x01
#define CT_SD1   0x02
#define CT_SD2   0x04
#define CT_BLOCK 0x08

bool sd_get_info(sd_info_t *info);
void sd_power_off(void);

/* Image volumes: slot 0 = drive 1 (the voice pack), slot 1 = drive 2 (voices/custom.ubpack). */
#define IMAGE_SLOTS 2
bool image_attach(unsigned slot, const char *path);
void image_detach(unsigned slot);

#endif
