#ifndef NMEA_H
#define NMEA_H

#include <stdbool.h>
#include <stdint.h>

/*
 * NMEA epoch assembly without hardware access (unit-tested): the GPS module
 * sends GGA, GSA, GSV and RMC once per epoch (1 or 10 per second), RMC last.
 * GGA and RMC with the same time form one fix.
 */
typedef struct {
  bool valid;          /* RMC status A, GGA quality > 0 */
  uint8_t fix_type;    /* GSA: 1 none, 2 2D, 3 3D */
  uint32_t unix_time;  /* UTC of the fix, whole seconds */
  uint16_t ms;         /* fraction of the fix time */
  int32_t lat_e7;      /* degrees * 1e7, north positive */
  int32_t lon_e7;      /* degrees * 1e7, east positive */
  float alt_msl_m;     /* height above mean sea level (geoid) */
  float geoid_sep_m;   /* geoid above the WGS84 ellipsoid: h_ell = alt_msl + geoid_sep */
  float speed_ms;      /* ground speed */
  float course_deg;
  float vel_n_ms;      /* from speed and course */
  float vel_e_ms;
  float vel_d_ms;      /* from the height change over about 1 s, sinking positive */
  bool vel_d_valid;    /* false for the first fix or after a gap */
  uint8_t sats;
  float hdop;
  uint32_t seq;        /* counts completed fixes */
  uint32_t rx_ms;      /* device clock when the fix was complete (set by gps.c) */
} gps_fix_t;

#define NMEA_HEIGHTS 16 /* 1.6 s at 10 Hz */

typedef struct {
  gps_fix_t pending;
  bool have_gga;
  uint32_t gga_tod_ms; /* time of day of the pending GGA */
  /* ellipsoidal heights of the last valid fixes (for the vertical speed) */
  struct {
    int64_t t_ms;
    float h_m;
  } heights[NMEA_HEIGHTS];
  uint8_t heights_n;
  uint8_t heights_next;
  uint32_t seq;
} nmea_epoch_t;

void nmea_epoch_init(nmea_epoch_t *e);

/* Fields of one sentence as split at the commas (f[0] = "GPGGA" ...). */
void nmea_epoch_gga(nmea_epoch_t *e, char **f, int n);
void nmea_epoch_gsa(nmea_epoch_t *e, char **f, int n);
/* True when this RMC completed a fix (out filled). */
bool nmea_epoch_rmc(nmea_epoch_t *e, char **f, int n, gps_fix_t *out);

/*
 * Fixes at a lower rate than the GPS delivers: true for the first fix in each 1/hz s of UTC (at
 * 1 Hz the first of each second, for the users that count in seconds: flight detection, thermals,
 * announcements, summary; at 4 Hz the IGC data records). Without a valid time one fix per
 * 0.95/hz s of the device clock.
 */
typedef struct {
  bool any;
  bool timed; /* slot is a UTC slot */
  uint64_t slot;
  uint32_t rx_ms;
} nmea_rate_t;

bool nmea_rate_due(nmea_rate_t *r, const gps_fix_t *fix, unsigned hz);

/* "ddmm.mmmm" + "N"/"S"/"E"/"W" -> degrees * 1e7 (0 for an empty field). */
int32_t nmea_coord_e7(const char *value, const char *hemisphere);
/* A decimal field ("1305.3", "-46.4", "0.9"); 0 for an empty or malformed one. Plain digit scanning: no
 * strtod, whose big-number path newlib shares between the tasks (fmt.h). */
float nmea_parse_float(const char *value);
/* "hhmmss.ss" -> milliseconds of the day; false if malformed. */
bool nmea_time_of_day(const char *value, uint32_t *ms);

#endif
