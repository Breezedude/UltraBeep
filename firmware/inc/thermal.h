#ifndef THERMAL_H
#define THERMAL_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Thermal detector with wind and glide estimates, fed once per second in flight
 * with the GNSS velocity and the GNSS-referenced altitude.
 *
 * Circling: at least one full turn within 30 s, of the ground track or of
 * the heading integrated from the gyro's turn rate about the vertical, which
 * also works at low ground speed or without a fix; it ends when both turn less
 * than 60 deg in 10 s. The climb phase starts with the turn that was detected
 * (30 s back).
 *
 * Events:
 *   THERMAL_CLIMB  every climb_report_s while circling: the mean climb since the
 *                  phase began (thermal_t.thermal_ms)
 *   THERMAL_EXIT   after a climb phase of at least exit_min_s and exit_min_gain_m:
 *                  the 30 s mean drops below 0.2 m/s, or the circling stops
 *                  (thermal_t.exit_climb_ms, exit_gain_m)
 *   THERMAL_GLIDE  after glide_straight_s of straight flight, when the climb before
 *                  gained glide_min_gain_m (0: always, also right after the takeoff):
 *                  ground speed / sink rate over the last glide_window_s of straight
 *                  flight with a fix (thermal_t.glide) - so never before that much of
 *                  it; again every glide_repeat_min, or once per glide
 *   THERMAL_WIND   when circling stops with a wind estimate that is new or
 *                  changed by more than 5 km/h or 45 deg (thermal_t.wind_*)
 *
 * Wind: while circling, the ground velocity vectors of the last full turn lie
 * on a circle around the wind vector (radius = airspeed); a least-squares
 * circle fit is accepted for an airspeed of 20..60 km/h and a small residual.
 */

#define THERMAL_WINDOW 64 /* seconds of history: the glide ratio and the height gained over up to 60 s */

enum {
  THERMAL_CLIMB = 1,
  THERMAL_EXIT = 2,
  THERMAL_GLIDE = 4,
  THERMAL_WIND = 8,
};

typedef struct {
  uint16_t climb_report_s;   /* announce_thermalclimb_int */
  uint16_t glide_min_gain_m; /* announce_glidemingain_int, 0 = the glide report also without a climb before */
  uint16_t exit_min_s;       /* announce_thermalexit_mintime_int: a climb phase counts from this long ... */
  uint16_t exit_min_gain_m;  /* announce_thermalexit_mingain_int: ... and this much height */
  uint16_t glide_straight_s; /* announce_glide_straighttime_int: straight flight before the glide report */
  uint16_t glide_repeat_min; /* announce_glide_repeat_int: again at this interval, 0 = once per glide */
  uint16_t glide_window_s;   /* the span of the glide ratio the report speaks (the glide block's option; 20) */
} thermal_config_t;

typedef struct {
  /* history, one entry per second */
  float turn[THERMAL_WINDOW];  /* track change in deg, 0 without a valid track */
  float gturn[THERMAL_WINDOW]; /* heading change from the gyro in deg, 0 while the attitude is unknown */
  float ve[THERMAL_WINDOW], vn[THERMAL_WINDOW];
  bool vel_ok[THERMAL_WINDOW];
  float alt[THERMAL_WINDOW];
  unsigned head, count;
  float last_course;
  bool have_course;

  bool circling;
  unsigned since_end;   /* seconds since the circling ended: older turns do not start it again */
  uint32_t phase_t;     /* start of the climb phase */
  float phase_alt;
  uint32_t next_report;
  bool exit_done;
  float thermal_ms;     /* mean climb since the phase began */

  float exit_climb_ms;  /* the phase that ended (THERMAL_EXIT) */
  float exit_gain_m;
  bool glide_armed;
  uint32_t straight_t;  /* circling ended (0: not yet known) */
  bool glide_said;      /* reported on this glide */
  uint32_t glide_last_s;
  float glide;          /* last glide ratio (THERMAL_GLIDE) */

  bool fit_ok;          /* wind fit of the current circling phase */
  float fit_e, fit_n;
  bool wind_valid;      /* smoothed over the thermals */
  float wind_e, wind_n; /* velocity of the air mass, m/s (east, north): the wind blows towards it */
  bool wind_said;
  float said_e, said_n;
} thermal_t;

void thermal_init(thermal_t *t);
/* Once per second in flight; returns THERMAL_* flags. gyro_dps: the turn rate about the vertical over
 * the second (clockwise positive) while gyro_valid, from the IMU fusion. */
unsigned thermal_update(thermal_t *t, const thermal_config_t *c, uint32_t now_s, bool vel_valid, float ve, float vn,
                        float alt_m, bool gyro_valid, float gyro_dps);
/* Glide ratio over the last span_s (at most THERMAL_WINDOW - 1) of straight flight with a fix: ground speed over
 * sink (false: circling, or it ended less than span_s ago, the track turned 90 deg or more within the span, a
 * second without a fix or slower than 2 m/s in it, less than 0.3 m/s of sink, or a ratio beyond 60). */
bool thermal_glide_over(const thermal_t *t, unsigned span_s, float *ratio);
/* Wind: speed in km/h and the direction it comes from (deg); false without an estimate. */
bool thermal_wind(const thermal_t *t, float *kmh, float *from_deg);

#endif
