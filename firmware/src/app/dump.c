#include "dump.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ble_app.h"
#include "board.h"
#include "config.h"
#include "crc32.h"
#include "log.h"
#include "sha256.h"
#include "storage.h"
#include "version.h"

#define DUMP_DIR  CONFIG_EXT_DIR "/dump"
#define BOOT_ADDR 0x08000000UL
#define BOOT_SIZE 0xF000UL /* up to the application at 0x0800F000 */
#define OTP_ADDR  0x1FFF7000UL
#define OTP_SIZE  1024UL

static bool text_ok;

/* PCROP area start and end are 9-bit offsets in 2 KB units; an end below the start = off. */
static bool pcrop_covers_boot(uint32_t start_reg, uint32_t end_reg)
{
  uint32_t start = start_reg & 0x1FFU;
  uint32_t end = end_reg & 0x1FFU;
  return end >= start && start < BOOT_SIZE / 0x800U;
}

/* Copies memory through RAM into a file, so that the SD card's DMA never reads flash. */
static bool write_memory(const char *path, uint32_t addr, uint32_t len)
{
  uint8_t buf[256];
  bool ok = storage_write_open(path);
  for (uint32_t off = 0; ok && off < len; off += sizeof(buf)) {
    uint32_t n = len - off < sizeof(buf) ? len - off : (uint32_t)sizeof(buf);
    memcpy(buf, (const void *)(addr + off), n);
    ok = storage_write_chunk(buf, n);
  }
  return storage_write_close() && ok;
}

/* One formatted piece of the open text file. */
static void text(const char *fmt, ...)
{
  char line[160];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (n > 0) {
    size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1U;
    text_ok = storage_write_chunk(line, len) && text_ok;
  }
}

/* The version line the bootloader prints at its start, without its colour codes, e.g.
 * "Bootloader: BL_v3 (2022-08-11_... 17:22:18) | GIT: ... | GCC 10.3.1". */
static void boot_text(char *out, size_t size)
{
  static const char key[] = "Bootloader: BL_v";
  const char *p = (const char *)BOOT_ADDR;
  out[0] = 0;
  for (uint32_t i = 0; i + sizeof(key) - 1U <= BOOT_SIZE; i++) {
    if (memcmp(p + i, key, sizeof(key) - 1U) != 0) {
      continue;
    }
    size_t n = 0;
    for (uint32_t j = i; n + 1U < size && j < BOOT_SIZE; j++) {
      if (p[j] == 0x1B) { /* ESC [ ... letter */
        while (j + 1U < BOOT_SIZE && !((p[j + 1U] | 0x20) >= 'a' && (p[j + 1U] | 0x20) <= 'z')) {
          j++;
        }
        j++;
        continue;
      }
      if (p[j] < 0x20 || p[j] >= 0x7F) {
        break; /* the line end */
      }
      out[n++] = p[j];
    }
    out[n] = 0;
    return;
  }
}

static const char *ok_text(bool ok)
{
  return ok ? "ok" : "FAILED";
}

bool dump_device(void)
{
  if (!storage_is_mounted()) {
    console_printf("dump: the SD card is not mounted (switch the device on; as a USB drive the PC has it)\r\n");
    return false;
  }
  storage_mkdir(CONFIG_EXT_DIR);
  storage_mkdir(DUMP_DIR);
  const uint32_t pa = FLASH->PCROP1ASR, pae = FLASH->PCROP1AER, pb = FLASH->PCROP1BSR, pbe = FLASH->PCROP1BER;
  const bool readable = !pcrop_covers_boot(pa, pae) && !pcrop_covers_boot(pb, pbe);

  bool boot_ok = false;
  uint32_t crc = 0;
  uint8_t sha[SHA256_SIZE] = {0};
  char version[100] = "";
  if (readable) {
    boot_ok = write_memory(DUMP_DIR "/bootloader_08000000.bin", BOOT_ADDR, BOOT_SIZE);
    crc = crc32((const void *)BOOT_ADDR, BOOT_SIZE);
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, (const void *)BOOT_ADDR, BOOT_SIZE);
    sha256_final(&s, sha);
    boot_text(version, sizeof(version));
    console_printf("dump: bootloader 0x08000000-0x0800EFFF (60 KB) -> bootloader_08000000.bin %s, CRC32 %08lX\r\n",
                   ok_text(boot_ok), (unsigned long)crc);
    console_printf("      \"%s\"\r\n", version[0] ? version : "(no version text found)");
  } else {
    console_printf("dump: the bootloader lies under read protection (PCROP) - not read\r\n");
  }

  bool otp_ok = write_memory(DUMP_DIR "/otp_1FFF7000.bin", OTP_ADDR, OTP_SIZE);
  console_printf("dump: OTP 1 KB -> otp_1FFF7000.bin %s\r\n", ok_text(otp_ok));

  /* the option bytes in the format of the earlier dumps (hwcheck), for a plain diff */
  text_ok = storage_write_open(DUMP_DIR "/option_bytes.txt");
  text("OPTR=%08lX\r\nSFR=%08lX\r\nSRRVR=%08lX\r\n", (unsigned long)FLASH->OPTR, (unsigned long)FLASH->SFR,
       (unsigned long)FLASH->SRRVR);
  text("PCROP1ASR=%08lX\r\nPCROP1AER=%08lX\r\nPCROP1BSR=%08lX\r\nPCROP1BER=%08lX\r\n", (unsigned long)pa,
       (unsigned long)pae, (unsigned long)pb, (unsigned long)pbe);
  text("WRP1AR=%08lX\r\nWRP1BR=%08lX\r\nIPCCBR=%08lX\r\n", (unsigned long)FLASH->WRP1AR, (unsigned long)FLASH->WRP1BR,
       (unsigned long)FLASH->IPCCBR);
  bool opt_ok = storage_write_close() && text_ok;

  char serial[20];
  board_serial_number(serial, sizeof(serial));
  const uint32_t *uid = (const uint32_t *)UID_BASE;
  const uint32_t idcode = DBGMCU->IDCODE;
  ble_info_t b;
  ble_get_info(&b);
  text_ok = storage_write_open(DUMP_DIR "/device.txt");
  text("firmware=%s %s\r\nserial=%s\r\nuid96=%08lX%08lX%08lX\r\n", FW_NAME, FW_VERSION, serial, (unsigned long)uid[2],
       (unsigned long)uid[1], (unsigned long)uid[0]);
  text("idcode=%08lX (device %03lX, revision %04lX)\r\nflash_kb=%lu\r\npackage=%02lX\r\n", (unsigned long)idcode,
       (unsigned long)(idcode & 0xFFFU), (unsigned long)(idcode >> 16), (unsigned long)(*(const uint32_t *)FLASHSIZE_BASE & 0xFFFFU),
       (unsigned long)(*(const uint32_t *)PACKAGE_BASE & 0x1FU));
  text("bootloader_report=BKP13 %08lX BKP14 %08lX BKP15 %08lX BKP16 %08lX BKP17 %08lX BKP18 %08lX BKP19 %08lX\r\n",
       (unsigned long)RTC->BKP13R, (unsigned long)RTC->BKP14R, (unsigned long)RTC->BKP15R, (unsigned long)RTC->BKP16R,
       (unsigned long)RTC->BKP17R, (unsigned long)RTC->BKP18R, (unsigned long)RTC->BKP19R);
  text("bootloader_version=BL_v%lu\r\nbootloader_text=%s\r\n", (unsigned long)((RTC->BKP14R >> 16) & 0xFFU), version);
  if (readable) {
    text("bootloader_crc32=%08lX\r\nbootloader_sha256=", (unsigned long)crc);
    for (int i = 0; i < SHA256_SIZE; i++) {
      text("%02x", sha[i]);
    }
    text("\r\n");
  }
  text("hse_tune=%02lX\r\nsecure_flash_start=%08lX\r\n", (unsigned long)clock_hse_tune(),
       (unsigned long)(FLASH_BASE + (FLASH->SFR & FLASH_SFR_SFSA) * 4096UL));
  text("wireless_stack=%u.%u.%u type %u, fus %u.%u.%u\r\n", b.fw_major, b.fw_minor, b.fw_sub, b.stack_type, b.fus_major,
       b.fus_minor, b.fus_sub);
  bool dev_ok = storage_write_close() && text_ok;
  console_printf("dump: option_bytes.txt %s, device.txt %s (folder %s)\r\n", ok_text(opt_ok), ok_text(dev_ok), DUMP_DIR);
  return (boot_ok || !readable) && otp_ok && opt_ok && dev_ok;
}
