#include "tap.h"

#include <math.h>
#include <string.h>

#define SHOCK_SAMPLES    6U    /* 60 ms: the ringing of one knock belongs to it */
#define QUIET_BEFORE     30U   /* 300 ms without a spike before the first tap */
#define GAP_MIN          12U   /* second tap 120 ms .. */
#define GAP_MAX          50U   /* .. 500 ms after the first */
#define QUIET_AFTER      25U   /* 250 ms without a spike after the second tap */
#define FLOOR_FACTOR     8.0f  /* a tap stands this far above the recent jerk level */
#define FLOOR_ALPHA      0.02f /* 100 Hz: time constant 0.5 s */
#define FLOOR_START      0.005f

/* Jerk threshold at rest per sensitivity 0..4, g per sample. 0.14 used 0.7 g for the
 * user's "easy" and that triggered far too easily. */
static const float thresholds[TAP_SENSITIVITY_MAX + 1] = {0.8f, 1.1f, 1.5f, 2.0f, 2.8f};

void tap_set_sensitivity(tap_t *t, int sensitivity)
{
  if (sensitivity < TAP_SENSITIVITY_MIN || sensitivity > TAP_SENSITIVITY_MAX) {
    sensitivity = TAP_SENSITIVITY_DEFAULT;
  }
  t->threshold = thresholds[sensitivity];
}

void tap_init(tap_t *t, int sensitivity)
{
  memset(t, 0, sizeof(*t));
  t->floor = FLOOR_START;
  tap_set_sensitivity(t, sensitivity);
}

/* A new knock starts at sample n (the gap is the time since the previous spike). */
static void on_tap(tap_t *t, uint32_t gap)
{
  switch (t->state) {
  case TAP_IDLE:
    if (gap > QUIET_BEFORE && t->calm >= QUIET_BEFORE) {
      t->state = TAP_FIRST;
      t->first = t->n;
    }
    break;
  case TAP_FIRST: {
    uint32_t d = t->n - t->first;
    t->state = d >= GAP_MIN && d <= GAP_MAX ? TAP_SECOND : TAP_IDLE;
    break;
  }
  case TAP_SECOND:
    t->state = TAP_IDLE; /* a third tap: no double tap */
    break;
  }
}

bool tap_update(tap_t *t, const float a[3], float gyro_dps)
{
  t->n++;
  if (!t->have_last) {
    memcpy(t->last, a, sizeof(t->last));
    t->have_last = true;
    return false;
  }
  float dx = a[0] - t->last[0];
  float dy = a[1] - t->last[1];
  float dz = a[2] - t->last[2];
  memcpy(t->last, a, sizeof(t->last));
  float jerk = sqrtf(dx * dx + dy * dy + dz * dz);
  float thr = t->threshold;
  if (FLOOR_FACTOR * t->floor > thr) {
    thr = FLOOR_FACTOR * t->floor;
  }
  /* Spikes count only up to the threshold: a knock hardly moves the level, ongoing
   * vibration above the threshold raises it until it no longer counts as taps. */
  t->floor += FLOOR_ALPHA * ((jerk < thr ? jerk : thr) - t->floor);

  if (jerk > thr) {
    uint32_t gap = t->n - t->last_spike;
    t->last_spike = t->n;
    if (gap > SHOCK_SAMPLES) {
      t->tapped = true;
      t->tap_jerk = jerk;
      t->tap_threshold = thr;
      on_tap(t, gap);
    }
    return false;
  }
  /* calm gate: every sample outside a knock must be calm, or the device is being handled */
  if (t->n - t->last_spike > SHOCK_SAMPLES) {
    float norm = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    bool calm = gyro_dps < TAP_CALM_GYRO_DPS && norm > TAP_CALM_G_MIN && norm < TAP_CALM_G_MAX;
    t->calm = calm ? t->calm + 1U : 0U;
    if (!calm && t->state != TAP_IDLE) {
      t->state = TAP_IDLE;
    }
  }

  if (t->state == TAP_FIRST && t->n - t->first > GAP_MAX) {
    t->state = TAP_IDLE; /* a single tap */
  } else if (t->state == TAP_SECOND && t->n - t->last_spike > QUIET_AFTER) {
    t->state = TAP_IDLE;
    return true;
  }
  return false;
}
