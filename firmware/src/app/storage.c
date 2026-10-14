#include "storage.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "disk.h"
#include "ff.h"
#include "log.h"
#include "rtc_time.h"
#include "system.h"

static SemaphoreHandle_t lock;
static FATFS fs_sd;
static FATFS fs_pack;
static FATFS fs_custom;
static bool sd_mounted;
static bool pack_mounted;
static bool custom_mounted; /* 2: = 0:/voices/custom.ubpack */

void storage_init(void)
{
  lock = xSemaphoreCreateRecursiveMutex();
}

void storage_lock(void)
{
  if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
  }
}

/* FatFs file times: local time from the RTC (2026-01-01 00:00 before the first GNSS time). */
DWORD get_fattime(void)
{
  uint16_t y = 2026;
  uint8_t mo = 1, d = 1, h = 0, mi = 0, s = 0;
  if (rtc_time_valid()) {
    civil_from_unix(system_local_unix(rtc_time_unix()), &y, &mo, &d, &h, &mi, &s);
  }
  return ((DWORD)(y - 1980U) << 25) | ((DWORD)mo << 21) | ((DWORD)d << 16) | ((DWORD)h << 11) | ((DWORD)mi << 5) |
         ((DWORD)s >> 1);
}

bool storage_try_lock(void)
{
  if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
    return true;
  }
  return xSemaphoreTakeRecursive(lock, 0) == pdTRUE;
}

void storage_unlock(void)
{
  if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
    xSemaphoreGiveRecursive(lock);
  }
}

bool storage_mount(void)
{
  storage_lock();
  if (!sd_mounted) {
    FRESULT fr = f_mount(&fs_sd, "0:", 1);
    sd_mounted = fr == FR_OK;
    log_printf("SD card mount: %s", sd_mounted ? "ok" : "FAILED");
  }
  storage_unlock();
  return sd_mounted;
}

void storage_unmount(void)
{
  storage_lock();
  if (pack_mounted) {
    f_mount(NULL, "1:", 0);
    pack_mounted = false;
  }
  image_detach(0);
  if (custom_mounted) {
    f_mount(NULL, "2:", 0);
    custom_mounted = false;
  }
  image_detach(1);
  if (sd_mounted) {
    f_mount(NULL, "0:", 0);
    sd_mounted = false;
  }
  sd_power_off();
  storage_unlock();
}

bool storage_is_mounted(void)
{
  return sd_mounted;
}

static bool ends_with_nocase(const char *s, const char *suffix)
{
  size_t a = strlen(s);
  size_t b = strlen(suffix);
  if (a < b) {
    return false;
  }
  for (size_t i = 0; i < b; i++) {
    if (tolower((unsigned char)s[a - b + i]) != tolower((unsigned char)suffix[i])) {
      return false;
    }
  }
  return true;
}

bool storage_open_voice_pack(const char *lang)
{
  char path[64] = "";
  storage_lock();
  if (pack_mounted) {
    f_mount(NULL, "1:", 0);
    pack_mounted = false;
  }
  image_detach(0);
  if (custom_mounted) {
    f_mount(NULL, "2:", 0);
    custom_mounted = false;
  }
  image_detach(1);
  if (sd_mounted) {
    FILINFO fi;
    snprintf(path, sizeof(path), "0:/voices/%s.ubpack", lang && lang[0] ? lang : "de");
    if (f_stat(path, &fi) != FR_OK) {
      path[0] = 0;
      DIR dir;
      if (f_opendir(&dir, "0:/voices") == FR_OK) {
        while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
          if (!(fi.fattrib & AM_DIR) && ends_with_nocase(fi.fname, ".ubpack")) {
            snprintf(path, sizeof(path), "0:/voices/%.40s", fi.fname);
            break;
          }
        }
        f_closedir(&dir);
      }
    }
    if (path[0] && image_attach(0, path)) {
      pack_mounted = f_mount(&fs_pack, "1:", 1) == FR_OK;
      if (!pack_mounted) {
        image_detach(0);
      }
    }
    /* the extension pack: the user's clips packed up (replacements and own names); a single file in
     * voices/custom wins over both packs */
    if (f_stat(CUSTOM_PACK_PATH, &fi) == FR_OK && image_attach(1, CUSTOM_PACK_PATH)) {
      custom_mounted = f_mount(&fs_custom, "2:", 1) == FR_OK;
      if (!custom_mounted) {
        image_detach(1);
      }
    }
  }
  storage_unlock();
  log_printf("Voice pack %s: %s%s", path[0] ? path : "(none)", pack_mounted ? "ok" : "not available",
             custom_mounted ? ", extension pack ok" : "");
  return pack_mounted;
}

bool storage_voice_pack_ready(void)
{
  return pack_mounted;
}

/* "takeoff" -> <vol>/voices/takeoff.qoa, "al/al_1230" -> <vol>/al/al_1230.qoa */
static void pack_path(char *out, size_t size, const char *vol, const char *clip)
{
  snprintf(out, size, strchr(clip, '/') ? "%s/%.40s.qoa" : "%s/voices/%.40s.qoa", vol, clip);
}

bool storage_voice_locate(const char *clip, char *out, size_t size)
{
  FILINFO fi;
  bool ok = false;
  storage_lock();
  /* the user's single file first (it replaces the pack's clip of that name, which is what the
   * configurator promises), then the extension pack, then the main pack */
  if (sd_mounted) {
    snprintf(out, size, CUSTOM_DIR "/%.40s.qoa", clip);
    ok = f_stat(out, &fi) == FR_OK;
  }
  if (!ok && custom_mounted) {
    pack_path(out, size, "2:", clip);
    ok = f_stat(out, &fi) == FR_OK;
  }
  if (!ok && pack_mounted) {
    pack_path(out, size, "1:", clip);
    ok = f_stat(out, &fi) == FR_OK;
  }
  storage_unlock();
  return ok;
}

bool storage_voice_exists(const char *clip)
{
  char path[64];
  return storage_voice_locate(clip, path, sizeof(path));
}

bool storage_mkdir(const char *path)
{
  storage_lock();
  FRESULT fr = sd_mounted ? f_mkdir(path) : FR_NOT_READY;
  storage_unlock();
  return fr == FR_OK || fr == FR_EXIST;
}

bool storage_remove(const char *path)
{
  storage_lock();
  bool ok = sd_mounted && f_unlink(path) == FR_OK;
  storage_unlock();
  return ok;
}

static FIL stream_file;
static bool stream_open, stream_ok;

bool storage_write_open(const char *path)
{
  storage_lock();
  if (stream_open) {
    f_close(&stream_file);
    stream_open = false;
  }
  stream_ok = sd_mounted && f_open(&stream_file, path, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
  stream_open = stream_ok;
  storage_unlock();
  return stream_ok;
}

bool storage_write_chunk(const void *data, size_t n)
{
  UINT bw = 0;
  if (!stream_open) {
    return false;
  }
  storage_lock();
  bool ok = f_write(&stream_file, data, (UINT)n, &bw) == FR_OK && bw == n;
  stream_ok = stream_ok && ok;
  storage_unlock();
  return ok;
}

bool storage_write_close(void)
{
  if (!stream_open) {
    return false;
  }
  storage_lock();
  bool ok = f_close(&stream_file) == FR_OK && stream_ok;
  stream_open = false;
  storage_unlock();
  return ok;
}

bool storage_rename(const char *from, const char *to)
{
  storage_lock();
  f_unlink(to);
  bool ok = sd_mounted && f_rename(from, to) == FR_OK;
  storage_unlock();
  return ok;
}

int storage_read_text(const char *path, char *buf, size_t size)
{
  FIL f;
  UINT br = 0;
  int result = -1;
  if (size == 0) {
    return -1;
  }
  storage_lock();
  if (sd_mounted && f_open(&f, path, FA_READ) == FR_OK) {
    if (f_read(&f, buf, (UINT)(size - 1U), &br) == FR_OK) {
      buf[br] = 0;
      result = (int)br;
    }
    f_close(&f);
  }
  storage_unlock();
  return result;
}

bool storage_write_text(const char *path, const char *data, size_t len)
{
  char tmp[80];
  FIL f;
  UINT bw = 0;
  bool ok = false;
  snprintf(tmp, sizeof(tmp), "%.70s.tmp", path);
  storage_lock();
  if (sd_mounted && f_open(&f, tmp, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
    ok = f_write(&f, data, (UINT)len, &bw) == FR_OK && bw == len;
    ok = f_close(&f) == FR_OK && ok;
    if (ok) {
      f_unlink(path);
      ok = f_rename(tmp, path) == FR_OK;
    } else {
      f_unlink(tmp);
    }
  }
  storage_unlock();
  log_printf("Write %s: %s", path, ok ? "ok" : "FAILED");
  return ok;
}
