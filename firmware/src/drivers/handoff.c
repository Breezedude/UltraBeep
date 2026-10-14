/*
 * Hand-off to the stock bootloader. After a window watchdog reset it searches
 * the SD card root for bluebip*.update first, then ultrabip*.update, checks
 * size, header/trailer and checksum, flashes the image and deletes the file
 * (a broken file is renamed to *_failed).
 */
#include "handoff.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "ff.h"
#include "log.h"

#define HEADER_OFFSET  0x13CU
#define PAYLOAD_OFFSET 0x144U
#define PARK_DIR       "0:/firmware/parked"

static bool contains_nocase(const char *haystack, const char *needle)
{
  size_t n = strlen(needle);
  for (; *haystack; haystack++) {
    size_t i = 0;
    while (i < n && haystack[i] && tolower((unsigned char)haystack[i]) == tolower((unsigned char)needle[i])) {
      i++;
    }
    if (i == n) {
      return true;
    }
  }
  return false;
}

static bool starts_with_nocase(const char *s, const char *prefix)
{
  for (; *prefix; s++, prefix++) {
    if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) {
      return false;
    }
  }
  return true;
}

/* Files the bootloader could pick up: ultrabip* / bluebip* with .update or .install. */
static bool is_update_candidate(const FILINFO *fi)
{
  if (fi->fattrib & AM_DIR) {
    return false;
  }
  return (starts_with_nocase(fi->fname, "ultrabip") || starts_with_nocase(fi->fname, "bluebip")) &&
         (contains_nocase(fi->fname, ".update") || contains_nocase(fi->fname, ".install"));
}

static bool move_file(const char *from, const char *to)
{
  f_unlink(to);
  FRESULT fr = f_rename(from, to);
  log_printf("  move %s -> %s: %s", from, to, fr == FR_OK ? "ok" : "FAILED");
  return fr == FR_OK;
}

static bool validate_image(const char *path)
{
  static uint8_t buf[2048];
  FIL f;
  UINT br;
  bool ok = false;

  if (f_open(&f, path, FA_READ) != FR_OK) {
    log_printf("  %s: cannot open", path);
    return false;
  }
  uint32_t size = (uint32_t)f_size(&f);
  uint32_t hdr[2] = {0, 0};
  uint32_t trl[2] = {1, 1};

  if (size <= PAYLOAD_OFFSET + 8 || (size % 8) != 0) {
    log_printf("  %s: bad size %lu", path, (unsigned long)size);
    goto out;
  }
  if (f_lseek(&f, HEADER_OFFSET) != FR_OK || f_read(&f, hdr, 8, &br) != FR_OK || br != 8) {
    goto out;
  }
  if (f_lseek(&f, size - 8) != FR_OK || f_read(&f, trl, 8, &br) != FR_OK || br != 8) {
    goto out;
  }
  if (hdr[0] != trl[0] || hdr[1] != trl[1] || hdr[0] != size - PAYLOAD_OFFSET - 8) {
    log_printf("  %s: header/trailer mismatch", path);
    goto out;
  }

  cksum_ctx_t ctx;
  cksum_init(&ctx);
  f_lseek(&f, PAYLOAD_OFFSET);
  for (uint32_t left = hdr[0]; left > 0;) {
    UINT chunk = left > sizeof(buf) ? sizeof(buf) : left;
    if (f_read(&f, buf, chunk, &br) != FR_OK || br != chunk) {
      goto out;
    }
    cksum_update(&ctx, buf, br);
    left -= br;
  }
  uint32_t sum = cksum_final(&ctx);
  ok = sum == hdr[1];
  log_printf("  %s: %lu bytes, cksum %08lX (expected %08lX) %s", path, (unsigned long)size, (unsigned long)sum,
             (unsigned long)hdr[1], ok ? "OK" : "BAD");
out:
  f_close(&f);
  return ok;
}

/*
 * The first valid ultrabip*.update stays in the root; every other candidate
 * (including any bluebip*, which the bootloader would prefer) is parked, so
 * the bootloader finds exactly the chosen image.
 */
bool handoff_prepare_update_from_root(void)
{
  enum { MAX_CANDIDATES = 8 };
  static char names[MAX_CANDIDATES][64];
  unsigned count = 0;
  DIR dir;
  FILINFO fi;
  char from[80];
  char to[100];

  log_printf("Update: looking for 0:/ultrabip*.update");
  if (f_opendir(&dir, "0:/") != FR_OK) {
    return false;
  }
  while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0] && count < MAX_CANDIDATES) {
    if (is_update_candidate(&fi) && strlen(fi.fname) < sizeof(names[0])) {
      strcpy(names[count++], fi.fname);
    }
  }
  f_closedir(&dir);

  int chosen = -1;
  for (unsigned i = 0; i < count && chosen < 0; i++) {
    snprintf(from, sizeof(from), "0:/%.63s", names[i]);
    if (starts_with_nocase(names[i], "ultrabip") && contains_nocase(names[i], ".update") && validate_image(from)) {
      chosen = (int)i;
    }
  }
  if (chosen < 0) {
    log_printf("  no valid update image in the root");
    return false;
  }
  f_mkdir("0:/firmware");
  f_mkdir(PARK_DIR);
  for (unsigned i = 0; i < count; i++) {
    if ((int)i != chosen) {
      snprintf(from, sizeof(from), "0:/%.63s", names[i]);
      snprintf(to, sizeof(to), PARK_DIR "/%.63s", names[i]);
      move_file(from, to);
    }
  }
  log_printf("  installing %s", names[chosen]);
  return true;
}

void handoff_wwdg_reset(void)
{
  log_printf("Handing over to the bootloader (WWDG reset)");
  board_delay_ms(50);
  __disable_irq();
  BOARD_ATOMIC(__HAL_RCC_WWDG_CLK_ENABLE());
  WWDG->CFR = WWDG_CFR_W;
  /* Enabling the watchdog with T6 = 0 resets the device immediately. */
  WWDG->CR = WWDG_CR_WDGA | 0x3FU;
  for (;;) {
  }
}
