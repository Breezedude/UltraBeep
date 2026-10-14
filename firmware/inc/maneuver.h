#ifndef MANEUVER_H
#define MANEUVER_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Wingover and spiral detection from the IMU, without hardware access
 * (unit-tested), fed every dt seconds with the load factor |a|/g and the
 * rotation rate. Smoothed over 0.2 s.
 *   wingover  the load rises to MANEUVER_HIGH_G within MANEUVER_LOW_WINDOW_S
 *             after an unloaded moment (<= MANEUVER_LOW_G at the top)
 *   spiral    >= MANEUVER_SPIRAL_G and >= MANEUVER_SPIRAL_DPS for
 *             MANEUVER_SPIRAL_S (a wingover that goes on this way becomes one)
 * The manoeuvre ends after MANEUVER_END_S below MANEUVER_END_G; then its peak
 * load and the bank angle of a coordinated turn with that load, acos(1/n),
 * are reported once. The peak load of the whole flight is kept for the
 * flight summary.
 */
#define MANEUVER_SMOOTH_S      0.2f
#define MANEUVER_LOW_G         0.5f
#define MANEUVER_HIGH_G        2.0f
#define MANEUVER_LOW_WINDOW_S  4.0f
#define MANEUVER_SPIRAL_G      1.6f
#define MANEUVER_SPIRAL_DPS    50.0f
#define MANEUVER_SPIRAL_S      4.0f
#define MANEUVER_END_G         1.3f
#define MANEUVER_END_S         2.0f

typedef enum { MANEUVER_NONE, MANEUVER_WINGOVER, MANEUVER_SPIRAL } maneuver_kind_t;

typedef struct {
  float n;           /* smoothed load factor */
  bool started;
  float since_low_s; /* time since the load was last at or below MANEUVER_LOW_G */
  float high_s;      /* time at spiral load and rate in a row */
  float calm_s;      /* time below MANEUVER_END_G in a row during a manoeuvre */
  bool active;
  maneuver_kind_t kind;
  float n_max;       /* peak of the running manoeuvre */
  float g_max_flight; /* peak load factor since init */
  /* the last manoeuvre that ended */
  maneuver_kind_t done;
  float done_n_max;
  float done_bank_deg;
} maneuver_t;

void maneuver_init(maneuver_t *m);
/* Returns the kind of a manoeuvre that has just ended (done_* hold its values), else NONE. */
maneuver_kind_t maneuver_update(maneuver_t *m, float load_g, float rate_dps, float dt_s);
/* Bank angle of a coordinated turn at this load factor, degrees (0 below 1 g). */
float maneuver_bank_deg(float load_g);
const char *maneuver_kind_name(maneuver_kind_t k);

#endif
