#ifndef MANEUVER_H
#define MANEUVER_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Wingover and spiral detection from the IMU, without hardware access
 * (unit-tested), fed every dt seconds with the load factor |a|/g, the
 * magnitude of the rotation rate and the signed turn rate (clockwise
 * positive, 0.5 s mean; sensors.c). The load is smoothed over 0.2 s.
 *   wingover  turns that change direction: a turn of at least
 *             MANEUVER_TURN_MIN_S beyond MANEUVER_TURN_DPS, then within
 *             MANEUVER_REVERSE_S one the other way, both shorter than
 *             MANEUVER_TURN_MAX_S, with a load peak of MANEUVER_WINGOVER_G
 *             around them. A series of swings is one wingover; it ends when
 *             a turn goes on longer (a circle or a spiral follows) or the
 *             load stays low. Built on the flights of 2026-10-12 to -16: the
 *             swings before the spirals unloaded only to 0.8-1.2 g, so the
 *             stock rule (below 0.5 g, then 2 g within 4 s) found none.
 *   spiral    >= MANEUVER_SPIRAL_G and >= MANEUVER_SPIRAL_DPS for
 *             MANEUVER_SPIRAL_S
 * A manoeuvre ends after MANEUVER_END_S below MANEUVER_END_G; then its peak
 * load and its bank angle are reported once. The bank of a wingover is the
 * peak of the pilot's measured tilt (tilt.h) over its swings, from the start
 * of the turn before the first reversal on, when the tilt was known all that
 * time; else, and for a spiral, the bank of a coordinated turn at the peak
 * load, acos(1/n), which never exceeds 90 and misses the unloaded top of a
 * swing. The peak load of the whole flight is kept for the flight summary.
 */
#define MANEUVER_SMOOTH_S      0.2f
#define MANEUVER_TURN_DPS      40.0f  /* a turn: the signed turn rate beyond this */
#define MANEUVER_TURN_MIN_S    0.5f   /* ... at least this long */
#define MANEUVER_TURN_MAX_S    5.0f   /* a longer turn is a circle or a spiral, no wingover swing */
#define MANEUVER_TURN_MERGE_S  0.6f   /* a dip of the rate shorter than this does not end a turn */
#define MANEUVER_TURN_MAX_DPS  200.0f /* faster: knocks and handling, not flying */
#define MANEUVER_REVERSE_S     2.5f   /* the opposite turn begins within this after the last one ended */
#define MANEUVER_WINGOVER_G    1.8f   /* load peak of the two turns and the reversal between them */
#define MANEUVER_SETTLE_S      30.0f  /* no wingover this soon after the takeoff (the launch) */
#define MANEUVER_TURN_EARLY_S  1.0f   /* a wingover ending in a long turn counts that turn's load this long */
#define MANEUVER_SPIRAL_G      1.6f
#define MANEUVER_SPIRAL_DPS    50.0f
#define MANEUVER_SPIRAL_S      4.0f
#define MANEUVER_END_G         1.3f
#define MANEUVER_END_S         2.0f

typedef enum { MANEUVER_NONE, MANEUVER_WINGOVER, MANEUVER_SPIRAL } maneuver_kind_t;

typedef struct {
  float n;           /* smoothed load factor */
  bool started;
  float flight_s;    /* time since init (the takeoff) */
  float high_s;      /* time at spiral load and rate in a row */
  float calm_s;      /* time below MANEUVER_END_G in a row during a manoeuvre */
  bool active;
  maneuver_kind_t kind;
  float n_max;       /* peak of the running manoeuvre */
  uint8_t reversals; /* swings of the running wingover */
  float g_max_flight; /* peak load factor since init */
  /* the running turn and the one before it */
  int8_t turn_sign;  /* +1 right, -1 left, 0 none */
  float turn_s, turn_gap_s, turn_peak_g, turn_peak_dps;
  int8_t last_sign;
  float last_s, last_peak_g, last_peak_dps, since_last_s, between_peak_g;
  bool reversal_pending; /* the running turn reversed the last one: decided once it lasted MANEUVER_TURN_MIN_S */
  bool turn_started;     /* a turn began in this update */
  float peak_before_turn, turn_early_peak; /* the wingover's peak before the running turn, and that turn's early peak */
  /* the measured tilt [deg] alongside the load: peaks and whether the tilt was known throughout */
  float tilt_max, turn_tilt, last_tilt, between_tilt, tilt_before_turn, tilt_early;
  bool tilt_ok, turn_tilt_ok, last_tilt_ok, between_tilt_ok, tilt_before_ok, tilt_early_ok;
  /* the last manoeuvre that ended */
  maneuver_kind_t done;
  float done_n_max;
  float done_bank_deg;  /* a wingover's measured tilt peak (done_tilt_deg), else acos(1/done_n_max) */
  float done_tilt_deg;  /* the measured tilt peak, -1 when the tilt was not known throughout */
  uint8_t done_reversals;
  bool done_into_turn; /* the wingover ran into a long turn (a spiral or a circle follows) */
} maneuver_t;

void maneuver_init(maneuver_t *m);
/* Returns the kind of a manoeuvre that has just ended (done_* hold its values), else NONE. tilt_deg: the
 * pilot's tilt (its peak since the last update), negative while it is not known. */
maneuver_kind_t maneuver_update(maneuver_t *m, float load_g, float rate_dps, float turn_dps, float tilt_deg,
                                float dt_s);
/* Bank angle of a coordinated turn at this load factor, degrees (0 below 1 g). */
float maneuver_bank_deg(float load_g);
const char *maneuver_kind_name(maneuver_kind_t k);

#endif
