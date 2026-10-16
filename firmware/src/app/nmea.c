#include "nmea.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rtc_time.h"

#define KNOTS_TO_MS 0.514444f
#define MAX_GAP_MS  2500  /* vertical speed only between fixes at most this far apart */
#define SPAN_MS     900   /* ... and at least this far: at 10 Hz the height of 0.1 s ago would only give noise */
#define SLOT_MS     950U  /* 1/hz s without a valid time: device clock, some jitter allowed */

void nmea_epoch_init(nmea_epoch_t *e)
{
  memset(e, 0, sizeof(*e));
}

bool nmea_rate_due(nmea_rate_t *r, const gps_fix_t *fix, unsigned hz)
{
  if (hz < 1U) {
    hz = 1U;
  }
  if (fix->unix_time) {
    uint64_t slot = ((uint64_t)fix->unix_time * 1000U + fix->ms) * hz / 1000U;
    if (r->any && r->timed && slot == r->slot) {
      return false;
    }
    r->slot = slot;
    r->timed = true;
  } else {
    if (r->any && fix->rx_ms - r->rx_ms < SLOT_MS / hz) {
      return false;
    }
    r->timed = false;
  }
  r->any = true;
  r->rx_ms = fix->rx_ms;
  return true;
}

static int digits(const char *s, int n)
{
  int v = 0;
  for (int i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') {
      return -1;
    }
    v = v * 10 + (s[i] - '0');
  }
  return v;
}

bool nmea_time_of_day(const char *value, uint32_t *ms)
{
  if (strlen(value) < 6) {
    return false;
  }
  int h = digits(value, 2), m = digits(value + 2, 2), s = digits(value + 4, 2);
  if (h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 60) {
    return false;
  }
  uint32_t frac = 0;
  if (value[6] == '.') {
    /* up to three decimals, missing ones are zero */
    uint32_t scale = 100;
    for (const char *p = value + 7; *p >= '0' && *p <= '9' && scale > 0; p++, scale /= 10) {
      frac += (uint32_t)(*p - '0') * scale;
    }
  }
  *ms = (uint32_t)((h * 60 + m) * 60 + s) * 1000U + frac;
  return true;
}

float nmea_parse_float(const char *value)
{
  const char *p = value;
  bool neg = false;
  if (*p == '-' || *p == '+') {
    neg = *p == '-';
    p++;
  }
  uint32_t mant = 0, scale = 1;
  unsigned digits = 0; /* up to 9 significant digits: NMEA fields are shorter */
  for (; *p >= '0' && *p <= '9' && digits < 9U; p++, digits++) {
    mant = mant * 10U + (uint32_t)(*p - '0');
  }
  if (*p == '.') {
    for (p++; *p >= '0' && *p <= '9' && digits < 9U; p++, digits++) {
      mant = mant * 10U + (uint32_t)(*p - '0');
      scale *= 10U;
    }
  }
  float v = (float)mant / (float)scale;
  return neg ? -v : v;
}

int32_t nmea_coord_e7(const char *value, const char *hemisphere)
{
  const char *dot = strchr(value, '.');
  size_t int_len = dot ? (size_t)(dot - value) : strlen(value);
  if (int_len < 3) {
    return 0;
  }
  /* degrees are the digits before the last two integer digits (ddmm or dddmm) */
  int deg = digits(value, (int)(int_len - 2));
  if (deg < 0) {
    return 0;
  }
  /* minutes * 1e5 in integers: no rounding through float */
  int mm = digits(value + int_len - 2, 2);
  uint32_t frac = 0;
  uint32_t scale = 10000;
  if (dot) {
    for (const char *p = dot + 1; *p >= '0' && *p <= '9' && scale > 0; p++, scale /= 10) {
      frac += (uint32_t)(*p - '0') * scale;
    }
  }
  int64_t minutes_e5 = (int64_t)mm * 100000 + frac;
  int64_t e7 = (int64_t)deg * 10000000 + (minutes_e5 * 100 + 30) / 60; /* 1e7 deg = minutes * 1e7 / 60 */
  if (hemisphere[0] == 'S' || hemisphere[0] == 'W') {
    e7 = -e7;
  }
  return (int32_t)e7;
}

void nmea_epoch_gga(nmea_epoch_t *e, char **f, int n)
{
  uint32_t tod;
  if (n < 12 || !nmea_time_of_day(f[1], &tod)) {
    e->have_gga = false;
    return;
  }
  gps_fix_t *p = &e->pending;
  memset(p, 0, sizeof(*p));
  e->have_gga = true;
  e->gga_tod_ms = tod;
  int quality = atoi(f[6]);
  p->sats = (uint8_t)atoi(f[7]);
  p->hdop = nmea_parse_float(f[8]);
  if (quality > 0) {
    p->lat_e7 = nmea_coord_e7(f[2], f[3]);
    p->lon_e7 = nmea_coord_e7(f[4], f[5]);
    p->alt_msl_m = nmea_parse_float(f[9]);
    p->geoid_sep_m = nmea_parse_float(f[11]);
    p->valid = true; /* confirmed by the RMC status */
  }
}

void nmea_epoch_gsa(nmea_epoch_t *e, char **f, int n)
{
  if (n >= 3 && e->have_gga) {
    int type = atoi(f[2]);
    /* one GSA per satellite system with several systems: the best one counts */
    if (type >= 1 && type <= 3 && type > e->pending.fix_type) {
      e->pending.fix_type = (uint8_t)type;
    }
  }
}

bool nmea_epoch_rmc(nmea_epoch_t *e, char **f, int n, gps_fix_t *out)
{
  uint32_t tod;
  if (n < 10 || !e->have_gga || !nmea_time_of_day(f[1], &tod) || tod != e->gga_tod_ms) {
    return false;
  }
  e->have_gga = false;
  gps_fix_t *p = &e->pending;
  const char *d = f[9];
  int day = strlen(d) == 6 ? digits(d, 2) : -1;
  int month = day >= 0 ? digits(d + 2, 2) : -1;
  int year = month >= 0 ? digits(d + 4, 2) : -1;
  if (day < 1 || month < 1 || month > 12 || year < 24 || year > 79) {
    p->valid = false; /* e.g. the module's default date 06.01.1980 before it knows the time */
  } else {
    p->unix_time = unix_from_civil((uint16_t)(2000 + year), (uint8_t)month, (uint8_t)day, 0, 0, 0) + tod / 1000U;
    p->ms = (uint16_t)(tod % 1000U);
  }
  if (f[2][0] != 'A') {
    p->valid = false;
  }
  if (p->valid) {
    p->speed_ms = nmea_parse_float(f[7]) * KNOTS_TO_MS;
    p->course_deg = nmea_parse_float(f[8]);
    float c = p->course_deg * 0.017453293f;
    p->vel_n_ms = p->speed_ms * cosf(c);
    p->vel_e_ms = p->speed_ms * sinf(c);
    int64_t t_ms = (int64_t)p->unix_time * 1000 + p->ms;
    float h_now = p->alt_msl_m + p->geoid_sep_m;
    /* the newest height at least SPAN_MS old (the previous one at 1 Hz, nine fixes back at 10 Hz) */
    for (unsigned k = 1; k <= e->heights_n; k++) {
      unsigned i = (e->heights_next + NMEA_HEIGHTS - k) % NMEA_HEIGHTS;
      int64_t dt_ms = t_ms - e->heights[i].t_ms;
      if (dt_ms > MAX_GAP_MS || dt_ms <= 0) {
        break;
      }
      if (dt_ms >= SPAN_MS) {
        p->vel_d_ms = -(h_now - e->heights[i].h_m) * 1000.0f / (float)dt_ms;
        p->vel_d_valid = true;
        break;
      }
    }
    e->heights[e->heights_next].t_ms = t_ms;
    e->heights[e->heights_next].h_m = h_now;
    e->heights_next = (uint8_t)((e->heights_next + 1U) % NMEA_HEIGHTS);
    if (e->heights_n < NMEA_HEIGHTS) {
      e->heights_n++;
    }
  }
  p->seq = ++e->seq;
  *out = *p;
  return true;
}
