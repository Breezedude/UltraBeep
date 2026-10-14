#ifndef TAP_H
#define TAP_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Double-tap detection on the 100 Hz accelerometer, in software: the ICM-42605's
 * own tap detection (APEX) needs an accelerometer rate of at least 200 Hz and
 * its DMP, while the sensor task reads 100 Hz samples for the fusion.
 *
 * A tap is a jerk spike: the change of the acceleration vector from one sample
 * to the next stands out against the recent level of such changes (turbulence
 * and harness vibration raise that level, so they need a harder tap). The
 * samples of one knock and its ringing (60 ms) count as one tap.
 *
 * Double tap: quiet for 300 ms, a tap, a second tap 120..500 ms later, then
 * quiet for 250 ms (a third tap or ongoing shaking cancels it).
 *
 * Calm gate: the device must be calm around the taps - rotation below
 * TAP_CALM_GYRO_DPS and the load between TAP_CALM_G_MIN and TAP_CALM_G_MAX in
 * every sample outside the 60 ms of a knock. Launch preparation, the launch run,
 * a landing impact or a wingover knock the device about with 300-1000 °/s and
 * loads of 0.1-9 g; those produced double taps (a landing at 7.8 g was one).
 */
#define TAP_CALM_GYRO_DPS 250.0f
#define TAP_CALM_G_MIN    0.4f
#define TAP_CALM_G_MAX    1.8f

/* system_tapsensitivity_int as in the stock configurator: 0 = extremely easy to trigger,
 * 1 very easy, 2 easy, 3 balanced (default), 4 difficult. */
#define TAP_SENSITIVITY_MIN     0
#define TAP_SENSITIVITY_MAX     4
#define TAP_SENSITIVITY_DEFAULT 3

typedef enum { TAP_IDLE, TAP_FIRST, TAP_SECOND } tap_state_t;

typedef struct {
  float threshold;     /* jerk in g a tap must exceed at rest (from the sensitivity) */
  float floor;         /* recent mean jerk outside taps, g */
  float last[3];       /* previous sample, g */
  bool have_last;
  uint32_t n;          /* samples seen */
  uint32_t last_spike; /* sample index of the last jerk above the threshold */
  uint32_t first;      /* sample index of the pending first tap */
  tap_state_t state;
  uint32_t calm;       /* samples in a row that were calm (outside knocks) */
  /* Last single tap, for logging: set by tap_update, cleared by the caller. */
  bool tapped;
  float tap_jerk;      /* g */
  float tap_threshold; /* g, the threshold it exceeded */
} tap_t;

void tap_init(tap_t *t, int sensitivity);
void tap_set_sensitivity(tap_t *t, int sensitivity);
/* One accelerometer sample (100 Hz) in g with the rotation rate magnitude in °/s; true once a
 * double tap is complete. */
bool tap_update(tap_t *t, const float a[3], float gyro_dps);

#endif
