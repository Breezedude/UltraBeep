/*
 * FatFs disk I/O dispatcher.
 *   drive 0: microSD card (sdcard.c)
 *   drive 1: voice pack image (.ubpack = raw FAT12 image, 1024-byte sectors)
 *            read through an open file on drive 0, like the stock "1:/" volume.
 *   drive 2: the extension pack voices/custom.ubpack in the same way, when it exists.
 */
#include <stddef.h>

#include "disk.h"
#include "diskio.h"
#include "ff.h"

DSTATUS sd_disk_initialize(BYTE pdrv);
DSTATUS sd_disk_status(BYTE pdrv);
DRESULT sd_disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count);
DRESULT sd_disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count);
DRESULT sd_disk_ioctl(BYTE pdrv, BYTE cmd, void *buff);

static FIL image_file[IMAGE_SLOTS];
static bool image_open[IMAGE_SLOTS];
static WORD image_sector_size[IMAGE_SLOTS];
/* Cluster link map of the image file: a seek inside it reads no FAT sectors (fast seek). */
static DWORD image_clmt[IMAGE_SLOTS][32];

bool image_attach(unsigned slot, const char *path)
{
  uint8_t boot[16];
  UINT br;
  if (slot >= IMAGE_SLOTS) {
    return false;
  }
  image_detach(slot);
  FIL *f = &image_file[slot];
  if (f_open(f, path, FA_READ) != FR_OK) {
    return false;
  }
  image_clmt[slot][0] = sizeof(image_clmt[slot]) / sizeof(image_clmt[slot][0]);
  f->cltbl = image_clmt[slot];
  if (f_lseek(f, CREATE_LINKMAP) != FR_OK) {
    f->cltbl = NULL; /* too fragmented for the table: normal seeks */
  }
  if (f_read(f, boot, sizeof(boot), &br) != FR_OK || br != sizeof(boot)) {
    f_close(f);
    return false;
  }
  image_sector_size[slot] = (WORD)(boot[11] | (boot[12] << 8));
  if (image_sector_size[slot] != 512 && image_sector_size[slot] != 1024) {
    f_close(f);
    return false;
  }
  image_open[slot] = true;
  return true;
}

void image_detach(unsigned slot)
{
  if (slot < IMAGE_SLOTS && image_open[slot]) {
    f_close(&image_file[slot]);
    image_open[slot] = false;
  }
}

static DRESULT image_read(unsigned slot, BYTE *buff, DWORD sector, UINT count)
{
  UINT br;
  FSIZE_t offset = (FSIZE_t)sector * image_sector_size[slot];
  UINT len = count * image_sector_size[slot];
  if (f_lseek(&image_file[slot], offset) != FR_OK || f_read(&image_file[slot], buff, len, &br) != FR_OK || br != len) {
    return RES_ERROR;
  }
  return RES_OK;
}

/* Drives 1 and 2 are the image slots 0 and 1. */
static bool slot_open(BYTE pdrv)
{
  return pdrv >= 1 && pdrv <= IMAGE_SLOTS && image_open[pdrv - 1];
}

DSTATUS disk_initialize(BYTE pdrv)
{
  if (pdrv == 0) {
    return sd_disk_initialize(0);
  }
  return slot_open(pdrv) ? 0 : STA_NOINIT;
}

DSTATUS disk_status(BYTE pdrv)
{
  if (pdrv == 0) {
    return sd_disk_status(0);
  }
  return slot_open(pdrv) ? STA_PROTECT : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
  if (pdrv == 0) {
    return sd_disk_read(0, buff, sector, count);
  }
  return slot_open(pdrv) ? image_read(pdrv - 1, buff, sector, count) : RES_NOTRDY;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
  if (pdrv == 0) {
    return sd_disk_write(0, buff, sector, count);
  }
  return RES_WRPRT;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
  if (pdrv == 0) {
    return sd_disk_ioctl(0, cmd, buff);
  }
  if (!slot_open(pdrv)) {
    return RES_NOTRDY;
  }
  unsigned slot = pdrv - 1;
  switch (cmd) {
  case CTRL_SYNC:
    return RES_OK;
  case GET_SECTOR_SIZE:
    *(WORD *)buff = image_sector_size[slot];
    return RES_OK;
  case GET_SECTOR_COUNT:
    *(DWORD *)buff = (DWORD)(f_size(&image_file[slot]) / image_sector_size[slot]);
    return RES_OK;
  case GET_BLOCK_SIZE:
    *(DWORD *)buff = 1;
    return RES_OK;
  default:
    return RES_PARERR;
  }
}
