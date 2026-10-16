#include "rawlog.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "board.h"
#include "ff.h"
#include "fmt.h"
#include "icm42605.h"
#include "log.h"
#include "rtc_time.h"
#include "sensors.h"
#include "storage.h"
#include "version.h"

#define DIR_PATH      "0:/raw"
/* A power of two: about 2.5 s at 6.5 kB/s (barometer at 100 Hz, GNSS at 5 Hz); the writer takes 4 KB every
 * 250 ms, a card stall longer than the ring drops records, which the closing log line counts. */
#define RING_SIZE     16384U
#define RING_MASK     (RING_SIZE - 1U)
#define WRITE_CHUNK   4096U
#define WRITE_MS      250U
#define SYNC_MS       2000U  /* data lost at a power failure: at most this */
#define FORMAT        1U

static uint8_t ring[RING_SIZE];
static volatile uint32_t head; /* next byte to write into the ring (producers) */
static volatile uint32_t tail; /* next byte to write to the card (writer) */
static volatile bool accepting;
static volatile uint32_t dropped;

static FIL file;
static bool open;
static char name[24];
static uint32_t written;
static uint32_t last_sync;
static TaskHandle_t task;

/* ---- producers ---- */

static void put(uint8_t type, const void *payload, uint32_t len)
{
  if (!accepting || len > 255U) {
    return;
  }
  const uint8_t *p = (const uint8_t *)payload;
  taskENTER_CRITICAL(); /* several producer tasks; a few microseconds */
  uint32_t used = head - tail;
  if (used + len + 2U > RING_SIZE) {
    dropped++;
  } else {
    uint32_t h = head;
    ring[h++ & RING_MASK] = type;
    ring[h++ & RING_MASK] = (uint8_t)len;
    uint32_t at = h & RING_MASK;
    uint32_t first = RING_SIZE - at < len ? RING_SIZE - at : len; /* up to the end of the ring, the rest in front */
    memcpy(&ring[at], p, first);
    memcpy(ring, p + first, len - first);
    head = h + len;
  }
  taskEXIT_CRITICAL();
}

typedef struct __attribute__((packed)) {
  uint32_t t_ms;
  int32_t p_raw;
  int32_t t_raw;
  float pressure_pa;
  float temp_c;
  float filter_alt_m;
  float filter_vario_ms;
} baro_rec_t;

typedef struct __attribute__((packed)) {
  uint32_t t_ms;
  int16_t raw[6];
} imu_rec_t;

typedef struct __attribute__((packed)) {
  char magic[4];
  uint8_t format;
  uint8_t baro_rate_hz;
  uint16_t imu_rate_hz;
  float accel_lsb_per_g;
  float gyro_lsb_per_dps;
  float dps310_kp;
  float dps310_kt;
  int32_t dps310_coef[DPS310_COEFS];
  uint32_t unix_time; /* 0 = RTC not set */
  uint32_t t_ms;      /* time stamp of the start, same clock as the records */
  char firmware[16];
} header_rec_t;

void rawlog_baro(const dps310_sample_t *s, float filter_alt_m, float filter_vario_ms)
{
  baro_rec_t r = {HAL_GetTick(), s->p_raw, s->t_raw, s->pressure_pa, s->temp_c, filter_alt_m, filter_vario_ms};
  put(RAWLOG_BARO, &r, sizeof(r));
}

void rawlog_imu(const int16_t raw[6])
{
  imu_rec_t r;
  r.t_ms = HAL_GetTick();
  memcpy(r.raw, raw, sizeof(r.raw));
  put(RAWLOG_IMU, &r, sizeof(r));
}

static void put_text(uint8_t type, const char *text)
{
  uint8_t buf[4 + 120];
  uint32_t t = HAL_GetTick();
  size_t n = strlen(text);
  if (n > sizeof(buf) - 4) {
    n = sizeof(buf) - 4;
  }
  memcpy(buf, &t, 4);
  memcpy(buf + 4, text, n);
  put(type, buf, 4U + n);
}

void rawlog_nmea(const char *line)
{
  put_text(RAWLOG_NMEA, line);
}

void rawlog_settings(const char *text)
{
  uint8_t buf[4 + 240];
  uint32_t t = HAL_GetTick();
  size_t n = strlen(text);
  if (n > sizeof(buf) - 4) {
    n = sizeof(buf) - 4;
  }
  memcpy(buf, &t, 4);
  memcpy(buf + 4, text, n);
  put(RAWLOG_SETTINGS, buf, 4U + n);
}

typedef struct __attribute__((packed)) {
  uint32_t t_ms;
  float offset[3];
  float scale[3];
} calib_rec_t;

void rawlog_calibration(const float offset[3], const float scale[3])
{
  calib_rec_t r;
  r.t_ms = HAL_GetTick();
  for (int k = 0; k < 3; k++) {
    r.offset[k] = offset[k];
    r.scale[k] = scale[k];
  }
  put(RAWLOG_CALIB, &r, sizeof(r));
}

void rawlog_event(const char *fmt, ...)
{
  if (!accepting) {
    return;
  }
  char text[100];
  va_list ap;
  va_start(ap, fmt);
  fmt_vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  put_text(RAWLOG_EVENT, text);
}

/* ---- writer (holds the storage lock) ---- */

static void write_out(bool all)
{
  while (open) {
    uint32_t used = head - tail;
    if (used == 0U || (!all && used < WRITE_CHUNK)) {
      break;
    }
    uint32_t start = tail & RING_MASK;
    uint32_t n = used > WRITE_CHUNK ? WRITE_CHUNK : used;
    if (n > RING_SIZE - start) {
      n = RING_SIZE - start; /* up to the end of the ring, the rest in the next round */
    }
    UINT bw = 0;
    if (f_write(&file, &ring[start], n, &bw) != FR_OK || bw != n) {
      log_printf("Raw log: write error, stopped");
      accepting = false;
      f_close(&file);
      open = false;
      return;
    }
    tail += n;
    written += n;
  }
}

static void writer_task(void *arg)
{
  (void)arg;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WRITE_MS));
    if (!open) {
      continue;
    }
    storage_lock();
    bool due = HAL_GetTick() - last_sync >= SYNC_MS;
    write_out(due);
    if (open && due) {
      f_sync(&file);
      last_sync = HAL_GetTick();
    }
    storage_unlock();
  }
}

/* ---- control ---- */

static uint32_t next_number(void)
{
  DIR dir;
  FILINFO fi;
  uint32_t max = 0;
  if (f_opendir(&dir, DIR_PATH) != FR_OK) {
    return 1;
  }
  while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
    unsigned n;
    if (sscanf(fi.fname, "RAW%5u.BIN", &n) == 1 && n > max) {
      max = n;
    }
  }
  f_closedir(&dir);
  return max + 1U;
}

void rawlog_init(void)
{
  xTaskCreate(writer_task, "rawlog", 256, NULL, 2, &task); /* 300 B in use (console `tasks`, 2026-10-16) */
}

bool rawlog_start(void)
{
  if (open) {
    return true;
  }
  storage_lock();
  f_mkdir(DIR_PATH);
  uint32_t n = next_number();
  snprintf(name, sizeof(name), DIR_PATH "/RAW%05lu.BIN", (unsigned long)(n % 100000U));
  bool ok = f_open(&file, name, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
  if (ok) {
    taskENTER_CRITICAL();
    head = tail = 0;
    dropped = 0;
    taskEXIT_CRITICAL();
    written = 0;
    last_sync = HAL_GetTick();
    open = true;
    accepting = true;

    float kp, kt;
    unsigned baro_hz = sensors_baro_rate();
    dps310_scale_factors(baro_hz, &kp, &kt);
    header_rec_t h = {{'U', 'B', 'R', 'W'}, FORMAT, (uint8_t)baro_hz, ICM42605_RATE_HZ,
                      ICM42605_ACCEL_LSB_G, ICM42605_GYRO_LSB_DPS, kp, kt, {0}, 0, 0, {0}};
    int32_t coef[DPS310_COEFS];
    dps310_coefficients(coef);
    memcpy(h.dps310_coef, coef, sizeof(coef)); /* packed struct: no direct pointer */
    h.unix_time = rtc_time_valid() ? rtc_time_unix() : 0U;
    h.t_ms = HAL_GetTick();
    snprintf(h.firmware, sizeof(h.firmware), "%s", FW_VERSION);
    put(RAWLOG_HEADER, &h, sizeof(h));
    float off[3], scale[3];
    sensors_accel_calibration(off, scale);
    rawlog_calibration(off, scale);
  }
  storage_unlock();
  log_printf("Raw log %s: %s", name, ok ? "recording" : "cannot create");
  return ok;
}

void rawlog_stop(void)
{
  storage_lock();
  if (open) {
    accepting = false;
    write_out(true);
    if (open) {
      f_close(&file);
      open = false;
    }
    log_printf("Raw log %s: closed, %lu bytes, %lu records dropped", name, (unsigned long)written,
               (unsigned long)dropped);
  }
  storage_unlock();
}

void rawlog_get_status(rawlog_status_t *st)
{
  st->active = open;
  snprintf(st->file, sizeof(st->file), "%s", name);
  st->bytes = written;
  st->dropped = dropped;
}
