/*
 * Environment of src/app/recorder.c in the emulator: clock, device id,
 * configuration, signing key, and a FatFs replacement that writes the one
 * file into RAM. run_logic_tests.py reads test_igc_file / test_igc_len after
 * the run and checks the file with tools/vbv_igc.py (--check, --verify).
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "config.h"
#include "ff.h"
#include "igc_key.h"
#include "log.h"
#include "storage.h"

uint32_t test_tick;
char test_igc_file[32768];
uint32_t test_igc_len;
char test_igc_path[64];
bool test_igc_closed;
unsigned test_dir_entries; /* how many earlier flights of the day f_readdir reports */
char test_dir_serial[5];   /* device serial in their names (taken from a first recording) */

uint32_t HAL_GetTick(void) { return test_tick; }
uint32_t HAL_GetUIDw0(void) { return 0x00270026U; }
uint32_t HAL_GetUIDw1(void) { return 0x32565008U; }
uint32_t HAL_GetUIDw2(void) { return 0x20343741U; }
void board_serial_number(char *out, size_t size) { snprintf(out, size, "0080E12600A74360"); }

void storage_lock(void) {}
void storage_unlock(void) {}
void log_printf(const char *fmt, ...) { (void)fmt; }

/* key of the test: bytes 0..31 (tools side uses the same) */
const bool igc_key_available = true;
const uint8_t igc_key[IGC_KEY_SIZE] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
                                       16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};

static config_t test_config;

config_t *config_get(void)
{
  if (!test_config.profile[0].present) {
    test_config.profile[0].present = true;
    test_config.profile[0].enabled = true;
    strcpy(test_config.profile[0].pilot, "Test Pilot");
    strcpy(test_config.profile[0].glider, "Test Wing");
    test_config.record_igc = true;
    test_config.record_kml = true;
  }
  return &test_config;
}

/* Two files at a time: the IGC file and the KML file (by extension). */
char test_kml_file[16384];
uint32_t test_kml_len;
static FIL *igc_fp, *kml_fp;
static uint32_t igc_pos, kml_pos;

static bool is_kml(const TCHAR *path)
{
  size_t n = strlen(path);
  return n > 4 && strcmp(path + n - 4, ".kml") == 0;
}

FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode)
{
  (void)mode;
  if (is_kml(path)) {
    kml_fp = fp;
    kml_pos = 0;
    test_kml_len = 0;
    return FR_OK;
  }
  igc_fp = fp;
  igc_pos = 0;
  strncpy(test_igc_path, path, 63);
  test_igc_len = 0;
  test_igc_closed = false;
  return FR_OK;
}

FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw)
{
  bool k = fp == kml_fp;
  char *buf = k ? test_kml_file : test_igc_file;
  uint32_t cap = k ? sizeof(test_kml_file) : sizeof(test_igc_file);
  uint32_t *pos = k ? &kml_pos : &igc_pos;
  uint32_t *len = k ? &test_kml_len : &test_igc_len;
  if (*pos + btw > cap) {
    *bw = 0;
    return FR_DISK_ERR;
  }
  memcpy(buf + *pos, buff, btw);
  *pos += btw;
  if (*pos > *len) {
    *len = *pos;
  }
  *bw = btw;
  return FR_OK;
}

FRESULT f_lseek(FIL *fp, FSIZE_t ofs)
{
  if (fp == kml_fp) {
    kml_pos = (uint32_t)ofs;
  } else {
    igc_pos = (uint32_t)ofs;
  }
  return FR_OK;
}

/* A shorter rewrite leaves no tail: the KML footer is always rewritten at the end. */
FRESULT f_sync(FIL *fp)
{
  if (fp == kml_fp) {
    test_kml_len = kml_pos;
  }
  return FR_OK;
}

FRESULT f_close(FIL *fp)
{
  if (fp == kml_fp) {
    test_kml_len = kml_pos;
    kml_fp = NULL;
  } else {
    test_igc_closed = true;
    igc_fp = NULL;
  }
  return FR_OK;
}

FRESULT f_mkdir(const TCHAR *path) { (void)path; return FR_EXIST; }

static unsigned dir_pos;
FRESULT f_opendir(DIR *dp, const TCHAR *path) { (void)dp; (void)path; dir_pos = 0; return FR_OK; }
FRESULT f_closedir(DIR *dp) { (void)dp; return FR_OK; }

/* Lists earlier flights of the test day for the flight number. */
FRESULT f_readdir(DIR *dp, FILINFO *fno)
{
  (void)dp;
  memset(fno, 0, sizeof(*fno));
  if (dir_pos < test_dir_entries) {
    dir_pos++;
    char *p = fno->fname;
    memcpy(p, "2026-10-07-XSD-UB", 17);
    memcpy(p + 17, test_dir_serial, 4);
    p[21] = '-';
    p[22] = (char)('0' + dir_pos / 10U);
    p[23] = (char)('0' + dir_pos % 10U);
    strcpy(p + 24, ".igc");
  }
  return FR_OK;
}
