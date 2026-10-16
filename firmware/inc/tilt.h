#ifndef TILT_H
#define TILT_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The pilot's tilt against the true vertical (0 upright, 90 lying on the side, 180 upside down), without
 * hardware access (unit-tested): the bank of a wingover beyond what the load tells. acos(1/n) of a
 * coordinated turn never exceeds 90 and cannot see the unloaded top of a swing, where the bank is largest.
 * A port of the analyzer's pilot attitude (tools/analyzer/js/core/attitude.js) that learns while flying:
 *
 * 1. The gyro bias, learned on the ground while the device lies still (0.5 s means of the rotation below
 *    TILT_STILL_DPS, |a| within TILT_BIAS_G of g).
 * 2. The pilot's axes in the device's frame (the device sits anywhere on the pilot), learned in flight from
 *    0.5 s means of the specific force and the rotation:
 *    - up M: the specific force direction in calm straight flight (rotation below TILT_STRAIGHT_DPS, |a|
 *      within TILT_STRAIGHT_G of g), at least TILT_STRAIGHT_S of it;
 *    - left L: in a steady turn (TILT_TURN_MIN_DPS..TILT_TURN_MAX_DPS, |a| within TILT_TURN_G of g) the
 *      rotation vector is the true vertical while the specific force stays along the pilot, so the true
 *      vertical leans to the pilot's left in a right turn: L is that lean across M, signed by the turn
 *      (at least TILT_TURN_S of turns, a mean lean of TILT_MIN_LEAN);
 *    - forward F = L x M.
 * 3. The true vertical u in the device's frame: the gyro turns it (du/dt = u x w); the accelerometer pulls
 *    it towards the specific force less the centripetal acceleration w x (V F) with TILT_TAU_S while that
 *    is within TILT_GATE of g (V: the speed along the path, GNSS ground speed and climb rate); in a fast,
 *    steady rotation (a spiral) the rotation axis pulls it with TILT_SPIN_TAU_S - only after TILT_SPIN_HOLD_S
 *    and while the axis is within TILT_SPIN_ALIGN of u: a roll (a wingover's swing, the roll into a spiral)
 *    turns about a level axis, and pulling towards it threw the analyzer's attitude off by 40 degrees in a
 *    simulated roll into a spiral.
 * The tilt is acos(u . M), valid once the axes are learned.
 *
 * Checked with the analyzer's twin on the flights of 2026-10-12 and -16, two devices on one pilot: the axes
 * were learned 30-50 s after the takeoff, the two devices' tilt differed by 0.6-1.8 deg (median) and their
 * wingover peaks (88-107 deg, acos(1/n) 67-71) mostly by 0-3 deg; in steady spiral parts the tilt was
 * -4..+12 deg (median per flight) off acos(1/n). The stricter limits of attitude.js (3 deg/s, 5 %, 20 s and
 * 15 s) gave the same results 1.5-10 min after the takeoff. tools/analyzer/js/core/tilt.js is a port of
 * this file: change both.
 */
#define TILT_SMOOTH_S      0.5f   /* the means for the axes, the bias and the spin */
#define TILT_STILL_DPS     3.0f
#define TILT_BIAS_G        0.03f
#define TILT_BIAS_S        10.0f  /* the bias is a running mean over about this long */
#define TILT_STRAIGHT_DPS  8.0f
#define TILT_STRAIGHT_G    0.08f
#define TILT_STRAIGHT_S    10.0f
#define TILT_TURN_MIN_DPS  8.0f
#define TILT_TURN_MAX_DPS  80.0f  /* faster: spirals, where the nose is down and the lean is fore and aft too */
#define TILT_TURN_G        0.5f
#define TILT_TURN_S        10.0f
#define TILT_MIN_LEAN      0.05f  /* sin of the turns' mean bank: the turns must show a side */
#define TILT_TAU_S         3.0f
#define TILT_GATE          0.25f
#define TILT_SPIN_DPS      30.0f
#define TILT_SPIN_COHER    0.85f  /* |mean rotation vector| / mean |rotation|: a steady axis */
#define TILT_SPIN_TAU_S    0.7f
#define TILT_SPIN_HOLD_S   1.5f
#define TILT_SPIN_ALIGN    0.866f /* cos 30 deg */

typedef struct {
  float bias[3];       /* gyro bias [rad/s] */
  uint32_t n_bias;
  bool started;
  float fs[3];         /* specific force, 0.5 s mean [m/s^2] */
  float ws[3];         /* rotation after the bias, 0.5 s mean [rad/s] */
  float rs;            /* |rotation| after the bias, 0.5 s mean [rad/s] */
  float m_sum[3];      /* specific force directions in straight flight */
  uint32_t n_straight;
  float l_sum[3];      /* the turns' true vertical, signed by the turn */
  uint32_t n_turn;
  float M[3], F[3];    /* up and forward of the pilot in the device's frame */
  bool axes;           /* learned */
  float u[3];          /* the true vertical in the device's frame */
  bool u_ok;
  float spin_s;        /* fast steady rotation this long */
  float tilt_deg;      /* valid while axes */
} tilt_t;

void tilt_init(tilt_t *t);
/* One IMU sample: a calibrated [m/s^2], w the gyro as measured [rad/s], dt [s], in flight, the speed along
 * the path [m/s]. Returns whether t->tilt_deg is valid. */
bool tilt_update(tilt_t *t, const float a[3], const float w[3], float dt, bool in_flight, float speed_ms);

#endif
