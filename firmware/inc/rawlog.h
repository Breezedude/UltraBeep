#ifndef RAWLOG_H
#define RAWLOG_H

#include <stdbool.h>
#include <stdint.h>

#include "dps310.h"

/*
 * Raw data log for offline filter work (docs/raw-log-format.md, analyzer
 * tools/analyzer): every barometer and IMU sample, every line from the GPS
 * module and system events, with millisecond time stamps, in
 * 0:/raw/RAWnnnnn.BIN. Producers only copy into a RAM ring; a low-priority
 * task writes it to the SD card, so a slow card never delays the vario.
 */
typedef enum {
  RAWLOG_HEADER = 0,
  RAWLOG_BARO = 1,
  RAWLOG_IMU = 2,
  RAWLOG_NMEA = 3,
  RAWLOG_EVENT = 4,
  RAWLOG_CALIB = 5, /* accelerometer calibration in use (after the header and on every change) */
  RAWLOG_SETTINGS = 6, /* the settings a log needs to be read, as text (system.c log_*_settings) */
} rawlog_type_t;

void rawlog_init(void);
/* The SD card must be mounted. Creates the next file; false if that failed. */
bool rawlog_start(void);
/* Writes what is buffered and closes the file. */
void rawlog_stop(void);

void rawlog_baro(const dps310_sample_t *s, float filter_alt_m, float filter_vario_ms);
void rawlog_imu(const int16_t raw[6]);
void rawlog_nmea(const char *line);
void rawlog_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rawlog_calibration(const float offset[3], const float scale[3]);
/* One settings record: a group word, then "key=value" pairs (up to 240 characters). */
void rawlog_settings(const char *text);

typedef struct {
  bool active;
  char file[24];
  uint32_t bytes;   /* written to the card */
  uint32_t dropped; /* records lost because the ring was full */
} rawlog_status_t;

void rawlog_get_status(rawlog_status_t *st);

#endif
