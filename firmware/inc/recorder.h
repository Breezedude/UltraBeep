#ifndef RECORDER_H
#define RECORDER_H

#include <stdbool.h>
#include <stdint.h>

#include "nmea.h"

/*
 * Track recorder (docs/igc-vbv-records.md, "Files written by
 * UltraBeep"): per power-on with a takeoff an IGC file in 0:/tracks_IGC/<year>
 * and/or a KML file of the same name in 0:/tracks_KML/<year>
 * (flight_recordigc_bool, flight_recordkml_bool).
 * The last 15 seconds before the takeoff stay in RAM (one sample per second)
 * and start the file; then VBV data records at flight_recordrate_int (4 Hz)
 * and a B record and a KML point every second until the device is switched
 * off. Landings and further takeoffs only add markers, so a top landing
 * does not split the track. At the end a G record (HMAC-SHA-256 over the
 * file) if the firmware was built with a key.
 * Called from the system task only.
 */
typedef struct {
  gps_fix_t fix;
  float alt_std_m; /* pressure altitude (standard atmosphere): B record */
  float alt_raw_m; /* barometric altitude, GNSS-referenced, unfiltered */
  float alt_m;     /* filtered altitude */
  float vario_ms;
  float accel_ms2;
  bool second;     /* first sample of a second: also a B record and a KML point; kept before the takeoff */
} recorder_sample_t;

void recorder_reset(void);
void recorder_sample(const recorder_sample_t *s);
/* Starts the file on the first call (profile = 1..10 for the header), later only a marker. */
void recorder_takeoff(uint32_t utc, uint8_t profile);
void recorder_landing(uint32_t utc);
/* A new wind estimate: "LVBVWINDhhmmss <km/h> <from deg>" in the IGC file. */
void recorder_wind(uint32_t utc, float kmh, float from_deg);
/* G record and close; also before an update and at a power off. */
void recorder_close(void);

typedef struct {
  bool active;
  char file[48];
  uint32_t samples;
  uint32_t bytes;
  bool signed_file; /* a G record will be written */
} recorder_status_t;

void recorder_get_status(recorder_status_t *st);

#endif
