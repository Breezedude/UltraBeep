#ifndef FUSION_H
#define FUSION_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Barometer + IMU fusion for the vario (no hardware access, unit-tested in
 * tests/logic_tests.c):
 *
 * 1. Attitude (Mahony complementary filter): the gyro turns the attitude, the
 *    accelerometer pulls it towards the measured vertical while |a| is close
 *    to g (not in turns or bumps). Gives the vertical acceleration.
 * 2. Vertical Kalman filter, states altitude, climb rate, accelerometer bias:
 *    predicted with the vertical acceleration at the IMU rate, corrected with
 *    the barometric altitude. A pressure puff without acceleration barely
 *    moves the climb rate; a real climb starts with an acceleration and shows
 *    at once.
 * 3. At rest (gyro and |a| steady for 1 s, small turn rate, |a| within 10 % of
 *    g) the IMU is trusted more and the barometer ten times less, so drafts on
 *    a table stay silent. At rest the gyro bias and this sensor's g are
 *    learned. The rest test compares |a| with g itself, not with the learned
 *    value: on 2026-10-12 the first sample, taken in the hand at power-on, was
 *    9.18 m/s^2 (rest 10.05), the old test against the learned value never
 *    passed again, and the device showed -0.5 m/s standing still.
 * 4. Both weights adapt (lessons from a 93 min bike ride, 2026-10-07):
 *    - The barometer noise is measured from consecutive samples: airflow over
 *      the sensor raised it from 8 cm (walking) to 44 cm (30-40 km/h).
 *    - The IMU is trusted less while the horizontal specific force is large
 *      (vibration, turns, braking): there the vertical acceleration estimate
 *      carried no information, and trusting it made the vario lag 1.2 s.
 *      Vertical motion itself does not lower the trust.
 *    - The attitude is corrected by the accelerometer only within 5 % of g
 *      and below 10 deg/s, so turns do not tilt the estimated vertical.
 *    Replayed on the ride: lag 0.6 s instead of 1.2 s, best tracking error.
 * 5. Knocks and button presses (lesson from a 39 min car ride, 2026-10-08):
 *    together with them the barometric altitude jumped by up to 14 m within a
 *    second while the IMU saw almost no motion, and the vario showed up to
 *    6 m/s. After a button press or a tap the barometer is held off for
 *    FUSION_HOLD_S (fusion_hold_baro), the IMU acceleration is not integrated
 *    either (the climb rate stays where it was), and the altitude is taken
 *    over again afterwards without touching the climb rate.
 * 6. Barometer rate (32, 64 or 100 Hz): the variance limits are per sample at
 *    32 Hz and grow with the rate (variance per sample = noise density^2 /
 *    interval), the noise estimate keeps its 1.5 s: the filter weighs the
 *    barometer per second as before.
 * 7. Spirals (flights of 2026-10-09 and 2026-10-12, two devices on one flight):
 *    at 2-4 g and 60-150 deg/s the barometer carries a pressure modulation
 *    synchronous with the rotation, +-4 m at 2 g up to +-13 m at 4 g, the same
 *    on both devices, and the IMU vertical acceleration is unusable (every
 *    attitude error leaks 3-4 g into the vertical; the gyro-only attitude
 *    drifts with the gyro scale error over thousands of degrees). Spiral mode
 *    (load, rotation rate and a steady rotation axis, 1 s means): the IMU
 *    acceleration is not used and the bias state is frozen, the filter keeps
 *    following the raw altitude, and the climb rate shown is the altitude
 *    change over exactly one rotation period (scaled down with the load below
 *    FUSION_SPIRAL_FULL_G), which cancels the modulation without any model.
 *    Inherent lag half a rotation (1.5-3 s); it removes +-5..10 m/s of noise.
 *    The mode ends when the rotation slows or at the pull-out; the output
 *    cross-fades back to the filter, the attitude is seeded again from the
 *    accelerometer and re-acquired for a few seconds.
 * 8. Fast or unsteady rotation: the launch run, the landing and handling on the
 *    ground are fast rotations about changing axes (|mean gyro vector| / mean
 *    |gyro| over 1 s is 0.1-0.4 there, 0.93-0.99 in a spiral). While the
 *    rotation is fast, or fast and unsteady, the IMU acceleration is not used;
 *    the barometer keeps its normal weight. Replayed on the launch of
 *    2026-10-12: false lift on the ground before the launch 11.5 % -> 0.
 * 9. The bias integrator of the attitude filter is clamped to a plausible gyro
 *    bias, so it cannot wind up during long periods with a wrong attitude.
 *
 * Tuned on simulations and recorded flights (tools/analyzer: replay, metrics
 * per flight phase, parameter sweeps). The analyzer's
 * tools/analyzer/js/algorithms/fusion.js is a port of this file: change both.
 */
#define FUSION_ACCEL_NOISE      0.5f  /* vertical acceleration noise in calm motion [m/s^2] */
#define FUSION_ACCEL_NOISE_REST 0.1f  /* ... at rest: no turns, no attitude errors */
#define FUSION_ACCEL_NOISE_SPIRAL 1.5f /* ... in spiral mode (the filter follows the barometer alone) */
#define FUSION_DYN_GAIN         1.0f  /* added per m/s^2 of horizontal specific force (0.2 s mean) */
#define FUSION_BIAS_WALK        0.01f /* accelerometer bias random walk [m/s^2 / sqrt(s)] */
#define FUSION_BARO_VAR_MIN     0.04f /* barometric altitude variance, lower limit [m^2] */
#define FUSION_BARO_VAR_MAX     2.0f  /* ... upper limit */
#define FUSION_BARO_VAR_REST    1.0f  /* ... at least this at rest */
#define FUSION_BARO_VAR_HOLD    50.0f /* ... while held off after handling */
#define FUSION_HOLD_S           1.5f  /* hold after a button press or a tap */
#define FUSION_BARO_RATE_REF    32.0f /* Hz: the variance limits above are per sample at this rate */

/* manoeuvre modes (1 s means of the load, the rotation rate and the gyro vector) */
#define FUSION_MAN_TAU_S        1.0f
#define FUSION_SPIRAL_G         1.2f  /* spiral mode starts above this load ... */
#define FUSION_SPIRAL_RATE_DPS  45.0f /* ... and this rotation rate (ends below 3/4 of it) ... */
#define FUSION_SPIRAL_COHER     0.6f  /* ... about a steady axis: |mean gyro vector| / mean |gyro| */
#define FUSION_SPIRAL_ENTER_S   0.5f  /* the conditions must hold this long */
#define FUSION_SPIRAL_PULLOUT_G 2.0f  /* the pull-out rule once the spiral reached this load: */
#define FUSION_SPIRAL_EXIT_LOAD 0.5f  /* the load fell to this share of its highest */
#define FUSION_SPIRAL_EXIT_S    0.5f  /* spiral mode ends this long after the rotation or the load drop */
#define FUSION_SPIRAL_RAMP_S    1.5f  /* cross-fade from the one-rotation climb back to the filter */
#define FUSION_SPIRAL_FULL_G    1.5f  /* full averaging window from this load on, none at 1 g */
#define FUSION_SPIRAL_LOCKOUT_S 3.0f  /* no new spiral mode this long after one that lasted ... */
#define FUSION_SPIRAL_LOCKOUT_MIN_S 3.0f /* ... at least this long (a short false start may be followed at once) */
#define FUSION_WINDOW_MIN_S     0.5f  /* the averaging window: one rotation period within these limits */
#define FUSION_WINDOW_MAX_S     8.0f
#define FUSION_FAST_RATE_DPS    45.0f /* IMU acceleration off while the rotation is above this */
#define FUSION_HANDLING_RATE_DPS 100.0f /* handling: rotation above this ... */
#define FUSION_HANDLING_COHER   0.5f  /* ... about an unsteady axis */
#define FUSION_HANDLING_HOLD_S  1.0f  /* IMU acceleration off this long after handling */
#define FUSION_REACQ_S          3.0f  /* attitude re-acquisition after a spiral or handling */
#define FUSION_REACQ_GATE       0.3f  /* ... the accelerometer corrects within this share of g, at any rate */
#define FUSION_BC_MAX_DPS       3.0f  /* the attitude filter's bias integrator never exceeds this */
#define FUSION_HIST_STEP_S      0.1f  /* altitude history for the one-rotation climb: 10 per second ... */
#define FUSION_HIST_N           84U   /* ... for 8.4 s */

typedef enum {
  FUSION_MODE_NORMAL = 0,
  FUSION_MODE_SPIRAL,   /* the one-rotation climb is shown */
  FUSION_MODE_HANDLING, /* fast, unsteady rotation: IMU acceleration off */
  FUSION_MODE_HOLD,     /* after a button press or a tap: barometer held off, IMU acceleration off */
  FUSION_MODE_RAMP,     /* cross-fade after a spiral */
  FUSION_MODE_REACQ,    /* attitude re-acquisition after a spiral or handling: IMU acceleration off */
} fusion_mode_t;

typedef struct {
  /* attitude */
  float q[4];          /* body -> world (z up) */
  float gyro_bias_corr[3];
  float g_ref;         /* |specific force| at rest, learned (absorbs the accelerometer scale error) */
  bool attitude_ok;
  /* rest detection */
  float gyro_mean[3];  /* running means (0.5 s) to measure fluctuations */
  float accel_mean;
  float gyro_bias[3];  /* learned at rest [rad/s] */
  uint32_t still_ms;
  bool at_rest;
  /* adaptive weights */
  float hdyn;          /* horizontal specific force, 0.2 s mean [m/s^2] */
  float baro_last;
  float baro_var;      /* measured barometric altitude variance [m^2] */
  bool baro_have_last;
  /* vertical filter */
  float h, v, b;       /* altitude [m], climb rate [m/s], vertical accel bias [m/s^2] */
  float P[3][3];
  bool started;
  float last_up_accel; /* last vertical acceleration (bias not removed) */
  float vz;            /* the climb rate to use: v, or the one-rotation climb in a spiral */
  /* handling */
  float t;             /* IMU time [s] */
  float hold_until;    /* barometer held off until this IMU time */
  bool held;
  float baro_scale;    /* FUSION_BARO_RATE_REF / barometer rate: 1 at 32 Hz, 0.32 at 100 Hz */
  /* manoeuvres */
  float load_mean;     /* |a| / g_ref, 1 s mean */
  float rate_mean;     /* |gyro| after the bias [rad/s], 1 s mean */
  float w_mean[3];     /* gyro vector after the bias, 1 s mean */
  float coher;         /* |w_mean| / rate_mean: 1 for a steady rotation axis */
  bool spiral;
  float spiral_on_s, spiral_calm_s, spiral_start, load_max;
  float ramp_left;     /* cross-fade left after a spiral [s] */
  bool reseed;         /* attitude to be seeded again from the accelerometer */
  float handling_until, reacq_until, lockout_until;
  float window;        /* averaging window of the one-rotation climb [s] */
  uint8_t mode;        /* fusion_mode_t */
  /* altitude history for the one-rotation climb */
  float hist_t[FUSION_HIST_N], hist_h[FUSION_HIST_N];
  uint8_t hist_head, hist_count;
  float hist_last;
} fusion_t;

void fusion_init(fusion_t *f);
/* IMU sample: gyro [rad/s], accelerometer [m/s^2], both in sensor axes. */
void fusion_imu(fusion_t *f, const float gyro[3], const float accel[3], float dt);
/* Barometric altitude [m]. Afterwards f->vz is the climb rate to use. */
void fusion_baro(fusion_t *f, float altitude_m);
/* Barometer samples per second (default 32). */
void fusion_set_baro_rate(fusion_t *f, float hz);
/* Handling (button, tap): the barometer counts hardly for `seconds`, then the altitude is taken over. */
void fusion_hold_baro(fusion_t *f, float seconds);
/* Turn rate about the vertical from this gyro sample [rad/s], clockwise seen from above positive;
 * 0 before the attitude is known. The thermal detector integrates it per second. */
float fusion_yaw_rate(const fusion_t *f, const float gyro[3]);
const char *fusion_mode_name(uint8_t mode);

#endif
