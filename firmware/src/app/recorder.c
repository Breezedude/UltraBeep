#include "recorder.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "config.h"
#include "ff.h"
#include "biplink.h"
#include "igc.h"
#include "igc_key.h"
#include "kml.h"
#include "log.h"
#include "rtc_time.h"
#include "sha256.h"
#include "storage.h"
#include "version.h"

#define DIR_PATH    "0:/tracks_IGC"
#define KML_DIR     "0:/tracks_KML"
#define PRE_SAMPLES 15U     /* seconds before the takeoff (one sample per second) */
#define SYNC_MS     10000U  /* data lost at a power failure: at most this */

/* pre-record ring */
static recorder_sample_t pre[PRE_SAMPLES];
static unsigned pre_count;
static unsigned pre_head;

/* files: a recording is active when the IGC file, the KML file or both are open */
static FIL file;
static bool active;
static bool igc;
static bool failed;
static char path[80];
static uint32_t samples;
static uint32_t bytes;
static uint32_t last_sync;
static hmac_sha256_t mac;

/* KML: points up to kml_end, then the footer (overwritten by the next point) */
static FIL kml;
static bool kml_active;
static uint32_t kml_end;

/* time base of the data records: t = 1000 ms at the first sample */
static bool have_t0;
static uint64_t utc0_ms;
static uint64_t last_utc_ms;
static uint32_t last_rx_ms;

/* last valid position for B records while the fix is lost */
static bool have_pos;
static int32_t pos_lat_e7, pos_lon_e7;
static float pos_msl_m;

void recorder_reset(void)
{
  pre_count = 0;
  pre_head = 0;
  active = false;
  igc = false;
  failed = false;
  have_t0 = false;
  have_pos = false;
  samples = 0;
  bytes = 0;
  path[0] = 0;
  kml_active = false;
}

/* The last four digits of the serial number, as in the stock's file names (UB4360). */
static void device_serial(char out[5])
{
  char sn[20];
  board_serial_number(sn, sizeof(sn));
  memcpy(out, sn + strlen(sn) - 4U, 4);
  out[4] = 0;
}

/* 16 bytes that identify the track for the app: device id, start time, tick. */
static void track_identity(uint8_t id[IGC_TRACK_ID_SIZE], uint32_t utc)
{
  uint32_t w[4] = {HAL_GetUIDw0() ^ utc, HAL_GetUIDw1() ^ HAL_GetTick(), HAL_GetUIDw2() * 2654435761U, utc};
  memcpy(id, w, IGC_TRACK_ID_SIZE);
}

/* ---- writing (storage lock held by the caller) ---- */

static void fail(const char *what)
{
  log_printf("IGC %s: %s - IGC stopped", path, what);
  f_close(&file);
  igc = false;
  active = kml_active;
  failed = true;
}

static bool out(const char *s, size_t n)
{
  if (!igc || n == 0) {
    return false;
  }
  UINT bw = 0;
  if (f_write(&file, s, (UINT)n, &bw) != FR_OK || bw != n) {
    fail("write error");
    return false;
  }
  if (igc_key_available) {
    hmac_sha256_update(&mac, s, n);
  }
  bytes += (uint32_t)n;
  return true;
}

static void kml_stop(const char *why)
{
  log_printf("KML: %s - KML stopped, IGC goes on", why);
  f_close(&kml);
  kml_active = false;
}

static void kml_write(const char *s, size_t n, bool point)
{
  UINT bw = 0;
  if (!kml_active || n == 0) {
    return;
  }
  if (f_lseek(&kml, kml_end) != FR_OK || f_write(&kml, s, (UINT)n, &bw) != FR_OK || bw != n) {
    kml_stop("write error");
    return;
  }
  if (point) {
    kml_end += (uint32_t)n;
  }
}

static void kml_point_and_footer(int32_t lat_e7, int32_t lon_e7, float alt_m)
{
  char line[64];
  kml_write(line, kml_point(line, sizeof(line), lat_e7, lon_e7, alt_m), true);
  kml_write(line, kml_footer(line, sizeof(line)), false);
}

static uint64_t sample_utc_ms(const recorder_sample_t *s)
{
  if (s->fix.unix_time) {
    return (uint64_t)s->fix.unix_time * 1000U + s->fix.ms;
  }
  return last_utc_ms + (uint32_t)(s->fix.rx_ms - last_rx_ms); /* no time from the module: device clock */
}

static void write_sample(const recorder_sample_t *s)
{
  const gps_fix_t *f = &s->fix;
  bool valid = f->valid && f->fix_type == 3;
  if (!f->valid && !have_pos) {
    return; /* nothing before the first position (it would read 0 deg / 0 deg) */
  }
  uint64_t utc_ms = sample_utc_ms(s);
  if (!have_t0) {
    utc0_ms = utc_ms;
    have_t0 = true;
  }
  last_utc_ms = utc_ms;
  last_rx_ms = f->rx_ms;
  if (f->valid) {
    have_pos = true;
    pos_lat_e7 = f->lat_e7;
    pos_lon_e7 = f->lon_e7;
    pos_msl_m = f->alt_msl_m;
  }

  char line[IGC_LINE_MAX];
  igc_vbv_t v = {
    (uint32_t)(1000U + (utc_ms - utc0_ms)),
    (float)pos_lat_e7 * 1e-7f, (float)pos_lon_e7 * 1e-7f, pos_msl_m + f->geoid_sep_m,
    valid ? f->vel_n_ms : 0.0f, valid ? f->vel_e_ms : 0.0f, f->vel_d_valid ? f->vel_d_ms : 0.0f,
    s->alt_raw_m, pos_msl_m, s->vario_ms, s->alt_m, s->accel_ms2,
    f->sats, (uint8_t)(valid ? 1U : 0U),
  };
  out(line, igc_format_vbv(line, sizeof(line), &v));
  if (!s->second) {
    samples++;
    return; /* between the seconds: only the data record */
  }
  if (have_pos) {
    igc_fix_t b = {(uint32_t)(utc_ms / 1000U), pos_lat_e7, pos_lon_e7, valid, (int32_t)(s->alt_std_m + 0.5f),
                   (int32_t)(pos_msl_m + 0.5f), f->sats};
    out(line, igc_format_b(line, sizeof(line), &b));
  }
  if (valid && kml_active) {
    kml_point_and_footer(f->lat_e7, f->lon_e7, s->alt_m);
  }
  samples++;
}

/* ---- file ---- */

/* "0:/tracks_IGC/2026" (as the stock firmware: one folder per year) */
static void year_dir(char *out, size_t size, const char *base, uint32_t utc)
{
  uint16_t y;
  uint8_t mo, d, hh, mi, ss;
  civil_from_unix(utc, &y, &mo, &d, &hh, &mi, &ss);
  snprintf(out, size, "%s/%04u", base, y);
  f_mkdir(base);
  f_mkdir(out);
}

static unsigned next_flight_number(const char *dir_path, uint32_t utc, const char *serial)
{
  char prefix[40];
  igc_file_name(prefix, sizeof(prefix), utc, serial, 0);
  size_t plen = strlen(prefix) - strlen("00.IGC"); /* YYYY-MM-DD-XSD-UBSSSS- */
  unsigned max = 0;
  DIR dir;
  FILINFO fi;
  if (f_opendir(&dir, dir_path) == FR_OK) {
    while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
      if (strncmp(fi.fname, prefix, plen) == 0) {
        unsigned n = (unsigned)((fi.fname[plen] - '0') * 10 + (fi.fname[plen + 1] - '0'));
        if (n > max && n < 100U) {
          max = n;
        }
      }
    }
    f_closedir(&dir);
  }
  return max + 1U;
}

/* The IGC file, the KML file or both (flight_recordigc_bool, flight_recordkml_bool); same name. */
static bool open_file(uint32_t utc, uint8_t profile)
{
  const config_t *cfg = config_get();
  if (!cfg->record_igc && !cfg->record_kml) {
    return false;
  }
  char serial[5];
  char dir[32];
  device_serial(serial);
  year_dir(dir, sizeof(dir), cfg->record_igc ? DIR_PATH : KML_DIR, utc);
  unsigned number = next_flight_number(dir, utc, serial);
  char name[40];
  igc_file_name(name, sizeof(name), utc, serial, number);
  samples = 0;
  bytes = 0;
  last_sync = HAL_GetTick();
  const profile_t *p = &cfg->profile[(profile >= 1 && profile <= PROFILE_MAX ? profile : 1) - 1];
  static char header[1024];

  if (cfg->record_igc) {
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (f_open(&file, path, FA_WRITE | FA_CREATE_NEW) != FR_OK) {
      log_printf("IGC %s: cannot create", path);
      failed = true;
      return false;
    }
    igc = true;
    if (igc_key_available) {
      hmac_sha256_init(&mac, igc_key, IGC_KEY_SIZE);
    }
    uint8_t id[IGC_TRACK_ID_SIZE];
    track_identity(id, utc);
    igc_header_t h = {.utc = utc, .flight_number = number, .serial = serial, .pilot = p->pilot,
                      .glider_type = p->glider, .glider_id = NULL, .competition_id = p->compid,
                      .firmware = cfg->stock_fw_version[0] ? cfg->stock_fw_version : BIPLINK_FIRMWARE_VERSION,
                      .firmware_own = FW_VERSION,
                      .time_offset_min = (int16_t)announce_utc_offset_min(utc, p->time_offset_min, p->tz_auto, cfg->dst_rule, &cfg->tz_table),
                      .track_id = id};
    out(header, igc_format_header(header, sizeof(header), &h));
    log_printf("IGC %s: recording%s", path, igc_key_available ? "" : " (no signing key: no G record)");
  }

  if (cfg->record_kml) {
    char kpath[64], title[64], desc[96];
    year_dir(dir, sizeof(dir), KML_DIR, utc);
    snprintf(kpath, sizeof(kpath), "%s/%.*s.kml", dir, (int)(strlen(name) - 4), name);
    uint16_t y;
    uint8_t mo, d, hh, mi, ss;
    civil_from_unix(utc, &y, &mo, &d, &hh, &mi, &ss);
    snprintf(title, sizeof(title), "%04u-%02u-%02u %s", y, mo, d, p->pilot[0] ? p->pilot : "flight");
    snprintf(desc, sizeof(desc), "UltraBeep %s, %s", FW_VERSION, p->glider[0] ? p->glider : "glider unknown");
    if (f_open(&kml, kpath, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
      kml_active = true;
      kml_end = 0;
      kml_write(header, kml_header(header, sizeof(header), title, desc), true);
      kml_write(header, kml_footer(header, sizeof(header)), false);
    } else {
      log_printf("KML %s: cannot create", kpath);
    }
    if (!igc) {
      snprintf(path, sizeof(path), "%s", kpath);
    }
  }
  active = igc || kml_active;
  return active;
}

/* ---- interface ---- */

void recorder_sample(const recorder_sample_t *s)
{
  if (active) {
    storage_lock();
    write_sample(s);
    if (active && HAL_GetTick() - last_sync >= SYNC_MS) {
      if (igc) {
        f_sync(&file);
      }
      if (kml_active) {
        f_sync(&kml);
      }
      last_sync = HAL_GetTick();
    }
    storage_unlock();
    return;
  }
  if (!s->second) {
    return;
  }
  pre[pre_head] = *s;
  pre_head = (pre_head + 1U) % PRE_SAMPLES;
  if (pre_count < PRE_SAMPLES) {
    pre_count++;
  }
}

void recorder_takeoff(uint32_t utc, uint8_t profile)
{
  char line[IGC_LINE_MAX];
  if (failed) {
    return;
  }
  storage_lock();
  if (active) {
    out(line, igc_format_marker(line, sizeof(line), "TAKEOFF", utc));
    storage_unlock();
    return;
  }
  /* the file date is the one of its first record */
  const recorder_sample_t *first = pre_count ? &pre[(pre_head + PRE_SAMPLES - pre_count) % PRE_SAMPLES] : NULL;
  uint32_t file_utc = first && first->fix.unix_time ? first->fix.unix_time : utc;
  if (open_file(file_utc, profile)) {
    bool marked = false;
    for (unsigned k = pre_count; k > 0 && active; k--) {
      const recorder_sample_t *s = &pre[(pre_head + PRE_SAMPLES - k) % PRE_SAMPLES];
      if (!marked && s->fix.unix_time >= utc) {
        out(line, igc_format_marker(line, sizeof(line), "TAKEOFF", utc));
        marked = true;
      }
      write_sample(s);
    }
    if (!marked) {
      out(line, igc_format_marker(line, sizeof(line), "TAKEOFF", utc));
    }
    pre_count = 0;
    if (igc) {
      f_sync(&file);
    }
  }
  storage_unlock();
}

void recorder_landing(uint32_t utc)
{
  char line[IGC_LINE_MAX];
  storage_lock();
  out(line, igc_format_marker(line, sizeof(line), "LANDING", utc));
  if (igc) {
    f_sync(&file);
  }
  storage_unlock();
}

void recorder_wind(uint32_t utc, float kmh, float from_deg)
{
  char line[IGC_LINE_MAX];
  storage_lock();
  out(line, igc_format_wind(line, sizeof(line), utc, kmh, from_deg));
  storage_unlock();
}

void recorder_close(void)
{
  storage_lock();
  if (active) {
    if (igc && igc_key_available) {
      uint8_t digest[SHA256_SIZE];
      char line[IGC_LINE_MAX];
      hmac_sha256_final(&mac, digest);
      size_t n = igc_format_g(line, sizeof(line), digest);
      UINT bw;
      f_write(&file, line, (UINT)n, &bw); /* not part of the signed data */
      bytes += bw;
    }
    if (igc) {
      f_close(&file);
      igc = false;
    }
    active = false;
    if (kml_active) {
      f_close(&kml);
      kml_active = false;
    }
    log_printf("Track %s: closed, %lu records, %lu bytes", path, (unsigned long)samples, (unsigned long)bytes);
  }
  storage_unlock();
}

/* Word-sized fields, read without a lock: at worst one second old. */
void recorder_get_status(recorder_status_t *st)
{
  st->active = active;
  memcpy(st->file, path, sizeof(st->file) - 1);
  st->file[sizeof(st->file) - 1] = 0;
  st->samples = samples;
  st->bytes = bytes;
  st->signed_file = igc_key_available;
}
