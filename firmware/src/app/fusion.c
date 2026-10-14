#include "fusion.h"

#include <math.h>
#include <string.h>

#define G0            9.80665f
#define DEG           0.017453292f
#define KP            1.0f   /* attitude: pull towards the measured vertical [1/s] */
#define KI            0.02f  /* attitude: gyro bias correction */
#define INIT_GATE     0.10f  /* first sample taken as this sensor's g within +-10 % of g */
#define ACCEL_GATE    0.05f  /* accelerometer corrects the attitude only within +-5 % of g ... */
#define ATT_RATE_MAX  0.1745f /* ... and below 10 deg/s (no correction in turns) */
#define DYN_ALPHA     0.05f  /* horizontal specific force mean: 0.2 s at 100 Hz */
#define BARO_VAR_ALPHA 0.02f /* barometer variance: 1.5 s at 32 Hz */
#define REST_GYRO_DEV 0.0087f /* rad/s (0.5 deg/s): fluctuation around the running mean */
#define REST_GYRO_MAX 0.087f  /* rad/s (5 deg/s) after the bias: a steady turn is no rest */
#define REST_ACCEL_DEV 0.02f  /* relative fluctuation of |a| */
#define REST_G0_TOL   0.10f   /* relative deviation of |a| from g (not from g_ref: that could never recover) */
#define REST_MS       1000U
#define MEAN_ALPHA    0.02f   /* running means: 0.5 s at 100 Hz */
#define BIAS_ALPHA    0.01f   /* gyro bias learning at rest: 1 s */
#define G_REF_ALPHA   0.002f  /* g_ref learning at rest: 5 s */
#define SPIRAL_RATE   (FUSION_SPIRAL_RATE_DPS * DEG)
#define FAST_RATE     (FUSION_FAST_RATE_DPS * DEG)
#define HANDLING_RATE (FUSION_HANDLING_RATE_DPS * DEG)
#define BC_MAX        (FUSION_BC_MAX_DPS * DEG)

void fusion_init(fusion_t *f)
{
  memset(f, 0, sizeof(*f));
  f->q[0] = 1.0f;
  f->g_ref = G0;
  f->P[0][0] = 1.0f;
  f->P[1][1] = 1.0f;
  f->P[2][2] = 0.25f; /* accelerometer bias +-0.5 m/s^2 */
  f->baro_scale = 1.0f;
  f->load_mean = 1.0f;
  f->hold_until = -1.0f;
  f->handling_until = -1.0f;
  f->reacq_until = -1.0f;
  f->lockout_until = -1.0f;
}

void fusion_set_baro_rate(fusion_t *f, float hz)
{
  f->baro_scale = hz > 1.0f ? FUSION_BARO_RATE_REF / hz : 1.0f;
}

const char *fusion_mode_name(uint8_t mode)
{
  static const char *const names[] = {"normal", "spiral", "handling", "hold", "ramp", "reacquisition"};
  return mode < sizeof(names) / sizeof(names[0]) ? names[mode] : "?";
}

/* Direction of "up" in sensor axes: third row of the body -> world rotation. */
static void up_vector(const float q[4], float v[3])
{
  v[0] = 2.0f * (q[1] * q[3] - q[0] * q[2]);
  v[1] = 2.0f * (q[0] * q[1] + q[2] * q[3]);
  v[2] = q[0] * q[0] - q[1] * q[1] - q[2] * q[2] + q[3] * q[3];
}

/* Attitude from one accelerometer sample: roll and pitch, yaw 0. */
static void attitude_from_accel(fusion_t *f, const float a[3])
{
  float roll = atan2f(a[1], a[2]);
  float pitch = atan2f(-a[0], sqrtf(a[1] * a[1] + a[2] * a[2]));
  float cr = cosf(roll * 0.5f), sr = sinf(roll * 0.5f);
  float cp = cosf(pitch * 0.5f), sp = sinf(pitch * 0.5f);
  f->q[0] = cr * cp;
  f->q[1] = sr * cp;
  f->q[2] = cr * sp;
  f->q[3] = -sr * sp;
  f->attitude_ok = true;
}

static float clampf(float x, float lim)
{
  return x < -lim ? -lim : x > lim ? lim : x;
}

static void attitude_update(fusion_t *f, const float w[3], const float a[3], float norm, float dt)
{
  float gx = w[0], gy = w[1], gz = w[2];
  float rate2 = gx * gx + gy * gy + gz * gz;
  /* re-acquisition after a spiral or handling: the accelerometer pulls through a wide gate at any
   * rotation rate, without feeding the bias integrator */
  bool reacq = f->t < f->reacq_until;
  float gate = reacq ? FUSION_REACQ_GATE : ACCEL_GATE;
  if (norm > 0.0f && fabsf(norm / f->g_ref - 1.0f) < gate && (reacq || rate2 < ATT_RATE_MAX * ATT_RATE_MAX)) {
    float ax = a[0] / norm, ay = a[1] / norm, az = a[2] / norm;
    float v[3];
    up_vector(f->q, v);
    float ex = ay * v[2] - az * v[1];
    float ey = az * v[0] - ax * v[2];
    float ez = ax * v[1] - ay * v[0];
    if (!reacq) {
      /* clamped: a gyro bias beyond this is not plausible, so a long stretch with a wrong attitude
       * cannot wind the integrator up */
      f->gyro_bias_corr[0] = clampf(f->gyro_bias_corr[0] + KI * ex * dt, BC_MAX);
      f->gyro_bias_corr[1] = clampf(f->gyro_bias_corr[1] + KI * ey * dt, BC_MAX);
      f->gyro_bias_corr[2] = clampf(f->gyro_bias_corr[2] + KI * ez * dt, BC_MAX);
    }
    gx += KP * ex + f->gyro_bias_corr[0];
    gy += KP * ey + f->gyro_bias_corr[1];
    gz += KP * ez + f->gyro_bias_corr[2];
  } else {
    gx += f->gyro_bias_corr[0];
    gy += f->gyro_bias_corr[1];
    gz += f->gyro_bias_corr[2];
  }
  float *q = f->q;
  float hx = 0.5f * dt * gx, hy = 0.5f * dt * gy, hz = 0.5f * dt * gz;
  float q0 = q[0], q1 = q[1], q2 = q[2], q3 = q[3];
  q[0] = q0 - q1 * hx - q2 * hy - q3 * hz;
  q[1] = q1 + q0 * hx + q2 * hz - q3 * hy;
  q[2] = q2 + q0 * hy - q1 * hz + q3 * hx;
  q[3] = q3 + q0 * hz + q1 * hy - q2 * hx;
  float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  for (int i = 0; i < 4; i++) {
    q[i] /= n;
  }
}

/*
 * Rest: the gyro and |a| hardly fluctuate (their absolute values carry the
 * sensor's bias and scale error), the turn rate after the learned bias is
 * small and |a| is close to g. At rest the gyro bias and g_ref are learned;
 * the vertical filter's bias moves with g_ref, so learning does not show as
 * a climb.
 */
static void rest_update(fusion_t *f, const float w[3], float norm, float dt)
{
  float dev2 = 0.0f, rate2 = 0.0f;
  for (int i = 0; i < 3; i++) {
    f->gyro_mean[i] += MEAN_ALPHA * (w[i] - f->gyro_mean[i]);
    float d = w[i] - f->gyro_mean[i];
    float r = w[i] - f->gyro_bias[i];
    dev2 += d * d;
    rate2 += r * r;
  }
  f->accel_mean += MEAN_ALPHA * (norm - f->accel_mean);
  bool still = dev2 < REST_GYRO_DEV * REST_GYRO_DEV && rate2 < REST_GYRO_MAX * REST_GYRO_MAX &&
               fabsf(norm / f->accel_mean - 1.0f) < REST_ACCEL_DEV && fabsf(norm / G0 - 1.0f) < REST_G0_TOL;
  f->still_ms = still ? f->still_ms + (uint32_t)(dt * 1000.0f + 0.5f) : 0U;
  f->at_rest = f->still_ms >= REST_MS;
  if (f->at_rest) {
    for (int i = 0; i < 3; i++) {
      f->gyro_bias[i] += BIAS_ALPHA * (w[i] - f->gyro_bias[i]);
    }
    float g_old = f->g_ref;
    f->g_ref += G_REF_ALPHA * (norm - f->g_ref);
    f->b -= f->g_ref - g_old;
  }
}

/*
 * Manoeuvre means and the modes. Spiral: a steady fast rotation under load; it ends when the
 * rotation slows down or at the pull-out of a real spiral (the load alone does not end it: while
 * a spiral builds up the load swings between 1.1 and 1.6 g). Handling: fast, unsteady rotation.
 */
static void maneuver_update(fusion_t *f, const float w[3], float norm, float dt)
{
  float alpha = dt / (FUSION_MAN_TAU_S + dt);
  float rate = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
  f->load_mean += alpha * (norm / f->g_ref - f->load_mean);
  f->rate_mean += alpha * (rate - f->rate_mean);
  for (int i = 0; i < 3; i++) {
    f->w_mean[i] += alpha * (w[i] - f->w_mean[i]);
  }
  float wm = sqrtf(f->w_mean[0] * f->w_mean[0] + f->w_mean[1] * f->w_mean[1] + f->w_mean[2] * f->w_mean[2]);
  f->coher = f->rate_mean > 1e-3f ? wm / f->rate_mean : 0.0f;

  if (f->rate_mean > HANDLING_RATE && f->coher < FUSION_HANDLING_COHER) {
    f->handling_until = f->t + FUSION_HANDLING_HOLD_S;
    f->reacq_until = f->handling_until + FUSION_REACQ_S;
  }
  bool loaded = f->load_mean > FUSION_SPIRAL_G && f->rate_mean > SPIRAL_RATE && f->coher > FUSION_SPIRAL_COHER;
  if (!f->spiral) {
    f->spiral_on_s = loaded && f->t >= f->lockout_until ? f->spiral_on_s + dt : 0.0f;
    if (f->spiral_on_s >= FUSION_SPIRAL_ENTER_S) {
      f->spiral = true;
      f->spiral_calm_s = 0.0f;
      f->ramp_left = 0.0f;
      f->load_max = f->load_mean;
      f->spiral_start = f->t;
    }
    return;
  }
  if (f->load_mean > f->load_max) {
    f->load_max = f->load_mean;
  }
  bool calm = f->rate_mean < 0.75f * SPIRAL_RATE ||
              (f->load_max >= FUSION_SPIRAL_PULLOUT_G && f->load_mean < FUSION_SPIRAL_EXIT_LOAD * f->load_max);
  f->spiral_calm_s = calm ? f->spiral_calm_s + dt : 0.0f;
  if (f->spiral_calm_s >= FUSION_SPIRAL_EXIT_S) {
    f->spiral = false;
    f->spiral_on_s = 0.0f;
    f->ramp_left = FUSION_SPIRAL_RAMP_S; /* the window stays as it was: the cross-fade goes back to the filter */
    f->reseed = true;
    f->reacq_until = f->t + FUSION_REACQ_S;
    if (f->t - f->spiral_start >= FUSION_SPIRAL_LOCKOUT_MIN_S) {
      f->lockout_until = f->t + FUSION_SPIRAL_LOCKOUT_S; /* no flapping at the pull-out */
    }
  }
}

/* IMU acceleration not used: fast rotation (the specific force leaks into the vertical with every
 * attitude error), spiral and the ramp after it, handling and the re-acquisition, the hold after a tap. */
static bool imu_off(const fusion_t *f)
{
  return f->spiral || f->ramp_left > 0.0f || f->t < f->handling_until || f->t < f->reacq_until ||
         f->t < f->hold_until || f->rate_mean > FAST_RATE;
}

/* Kalman prediction with the vertical acceleration a (bias not removed). */
static void vertical_predict(fusion_t *f, float a, float dt)
{
  if (!f->started) {
    return;
  }
  bool off = imu_off(f);
  float ac = off ? 0.0f : a - f->b;
  f->h += f->v * dt + 0.5f * ac * dt * dt;
  f->v += ac * dt;

  /* P = F P F' + Q, F = [1 dt -dt^2/2; 0 1 -dt; 0 0 1]; with the IMU off the bias state is frozen:
   * no coupling, no walk */
  float (*P)[3] = f->P;
  float c1 = off ? 0.0f : -0.5f * dt * dt, c2 = off ? 0.0f : -dt;
  float F[3][3] = {{1.0f, dt, c1}, {0.0f, 1.0f, c2}, {0.0f, 0.0f, 1.0f}};
  float FP[3][3], N[3][3];
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      FP[i][j] = F[i][0] * P[0][j] + F[i][1] * P[1][j] + F[i][2] * P[2][j];
    }
  }
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      N[i][j] = FP[i][0] * F[j][0] + FP[i][1] * F[j][1] + FP[i][2] * F[j][2];
    }
  }
  float g[3] = {0.5f * dt * dt, dt, 0.0f};
  float sa = f->spiral || f->ramp_left > 0.0f ? FUSION_ACCEL_NOISE_SPIRAL
             : f->at_rest                     ? FUSION_ACCEL_NOISE_REST
                                              : FUSION_ACCEL_NOISE + FUSION_DYN_GAIN * f->hdyn;
  float sa2 = sa * sa;
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      P[i][j] = N[i][j] + sa2 * g[i] * g[j];
    }
  }
  if (off) {
    /* the barometer updates meanwhile say nothing about the bias: no correlation with h and v
     * (left alone, P02 would grow with P12 * t and give the bias a huge gain when the IMU returns) */
    P[0][2] = P[2][0] = P[1][2] = P[2][1] = 0.0f;
  } else {
    P[2][2] += FUSION_BIAS_WALK * FUSION_BIAS_WALK * dt;
  }
}

void fusion_imu(fusion_t *f, const float gyro[3], const float accel[3], float dt)
{
  float norm = sqrtf(accel[0] * accel[0] + accel[1] * accel[1] + accel[2] * accel[2]);
  if (!f->attitude_ok) {
    if (norm > 0.5f * G0) {
      attitude_from_accel(f, accel);
      /* Usually still at power on: start with this sensor's own g (scale error). */
      if (fabsf(norm / G0 - 1.0f) < INIT_GATE) {
        f->g_ref = norm;
      }
      f->accel_mean = norm;
      for (int i = 0; i < 3; i++) {
        f->gyro_mean[i] = gyro[i];
      }
    }
    return;
  }
  f->t += dt;
  rest_update(f, gyro, norm, dt);
  float w[3] = {gyro[0] - f->gyro_bias[0], gyro[1] - f->gyro_bias[1], gyro[2] - f->gyro_bias[2]};
  maneuver_update(f, w, norm, dt);
  if (f->ramp_left > 0.0f) {
    f->ramp_left -= dt;
    if (f->ramp_left <= 0.0f) {
      f->window = 0.0f;
    }
  }
  if (f->reseed && !f->spiral && fabsf(norm / f->g_ref - 1.0f) < ACCEL_GATE) {
    attitude_from_accel(f, accel); /* the gyro-only attitude of the spiral has drifted: start over */
    memset(f->gyro_bias_corr, 0, sizeof(f->gyro_bias_corr));
    f->reseed = false;
  }
  attitude_update(f, w, accel, norm, dt);
  float v[3];
  up_vector(f->q, v);
  float up = accel[0] * v[0] + accel[1] * v[1] + accel[2] * v[2];
  float hor2 = norm * norm - up * up;
  f->hdyn += DYN_ALPHA * (sqrtf(hor2 > 0.0f ? hor2 : 0.0f) - f->hdyn);
  f->last_up_accel = up - f->g_ref;
  vertical_predict(f, f->last_up_accel, dt);
}

void fusion_hold_baro(fusion_t *f, float seconds)
{
  if (f->t + seconds > f->hold_until) {
    f->hold_until = f->t + seconds;
  }
}

/* Altitude history, one entry per FUSION_HIST_STEP_S. */
static void hist_push(fusion_t *f)
{
  if (f->hist_count > 0U && f->t - f->hist_last < FUSION_HIST_STEP_S) {
    return;
  }
  f->hist_t[f->hist_head] = f->t;
  f->hist_h[f->hist_head] = f->h;
  f->hist_head = (uint8_t)((f->hist_head + 1U) % FUSION_HIST_N);
  if (f->hist_count < FUSION_HIST_N) {
    f->hist_count++;
  }
  f->hist_last = f->t;
}

/* Climb rate as the change of the filter altitude over the last `win` seconds. */
static bool period_climb(const fusion_t *f, float win, float *out)
{
  if (win < 0.05f || f->hist_count < 2U) {
    return false;
  }
  float t_old = f->t - win;
  unsigned i = (f->hist_head + FUSION_HIST_N - 1U) % FUSION_HIST_N; /* newest */
  int newer = -1;
  for (unsigned steps = 0; steps + 1U < f->hist_count && f->hist_t[i] > t_old; steps++) {
    newer = (int)i;
    i = (i + FUSION_HIST_N - 1U) % FUSION_HIST_N;
  }
  if (f->hist_t[i] > t_old + FUSION_HIST_STEP_S) {
    return false; /* not enough history yet */
  }
  float h_old = f->hist_h[i];
  if (newer >= 0 && f->hist_t[newer] > f->hist_t[i]) {
    float frac = (t_old - f->hist_t[i]) / (f->hist_t[newer] - f->hist_t[i]);
    frac = frac < 0.0f ? 0.0f : frac > 1.0f ? 1.0f : frac;
    h_old += (f->hist_h[newer] - h_old) * frac;
  }
  *out = (f->h - h_old) / win;
  return true;
}

void fusion_baro(fusion_t *f, float altitude_m)
{
  /* noise from consecutive samples (the true change in 31 ms is negligible) */
  if (f->baro_have_last) {
    float d = altitude_m - f->baro_last;
    float e = 0.5f * d * d;
    f->baro_var = f->baro_var > 0.0f ? f->baro_var + BARO_VAR_ALPHA * f->baro_scale * (e - f->baro_var) : e;
  }
  f->baro_last = altitude_m;
  f->baro_have_last = true;
  if (!f->started) {
    f->h = altitude_m;
    f->v = 0.0f;
    f->started = true;
    hist_push(f);
    return;
  }
  float (*P)[3] = f->P;
  float k_rate = 1.0f / f->baro_scale; /* limits per sample at this rate */
  float r_min = FUSION_BARO_VAR_MIN * k_rate, r_max = FUSION_BARO_VAR_MAX * k_rate;
  float r = f->baro_var < r_min ? r_min : f->baro_var > r_max ? r_max : f->baro_var;
  if (f->at_rest && r < FUSION_BARO_VAR_REST * k_rate) {
    r = FUSION_BARO_VAR_REST * k_rate;
  }
  if (f->t < f->hold_until) {
    r = FUSION_BARO_VAR_HOLD * k_rate; /* knock or button: the climb rate stays where it was */
    f->held = true;
  } else if (f->held) {
    f->h = altitude_m; /* a pressure offset may remain: the altitude jumps, the climb rate does not */
    f->held = false;
  }
  /* the filter always sees the raw altitude (no step and no bias at the end of a spiral); with the
   * IMU off the bias state gets no gain and keeps its covariance */
  int n = imu_off(f) ? 2 : 3;
  float s = P[0][0] + r;
  float k[3] = {P[0][0] / s, P[1][0] / s, n > 2 ? P[2][0] / s : 0.0f};
  float y = altitude_m - f->h;
  f->h += k[0] * y;
  f->v += k[1] * y;
  f->b += k[2] * y;
  float row0[3] = {P[0][0], P[0][1], P[0][2]};
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < n; j++) {
      P[i][j] -= k[i] * row0[j];
    }
  }
  hist_push(f);

  /* spiral: the climb rate is the altitude change over exactly one rotation period (the pressure
   * modulation synchronous with the rotation cancels), cross-faded back to the filter afterwards */
  float vz = f->v;
  if (f->spiral || f->ramp_left > 0.0f) {
    if (f->spiral && f->rate_mean >= 0.75f * SPIRAL_RATE) {
      /* the window follows the rotation while it is fast; at the pull-out it keeps its last value */
      float period = 2.0f * (float)M_PI / f->rate_mean;
      f->window = period < FUSION_WINDOW_MIN_S ? FUSION_WINDOW_MIN_S : period > FUSION_WINDOW_MAX_S ? FUSION_WINDOW_MAX_S : period;
    }
    /* the modulation goes with the load: below FUSION_SPIRAL_FULL_G the window shrinks with it */
    float share = (f->load_mean - 1.0f) / (FUSION_SPIRAL_FULL_G - 1.0f);
    share = share < 0.0f ? 0.0f : share > 1.0f ? 1.0f : share;
    float vs;
    if (period_climb(f, f->window * share, &vs)) {
      float wgt = f->spiral ? 1.0f : f->ramp_left / FUSION_SPIRAL_RAMP_S;
      vz = wgt * vs + (1.0f - wgt) * f->v;
    }
  }
  f->vz = vz;
  f->mode = f->spiral            ? FUSION_MODE_SPIRAL
            : f->ramp_left > 0.0f ? FUSION_MODE_RAMP
            : f->t < f->handling_until ? FUSION_MODE_HANDLING
            : f->t < f->hold_until     ? FUSION_MODE_HOLD
            : f->t < f->reacq_until    ? FUSION_MODE_REACQ
                                       : FUSION_MODE_NORMAL;
}

float fusion_yaw_rate(const fusion_t *f, const float gyro[3])
{
  if (!f->attitude_ok) {
    return 0.0f;
  }
  float v[3];
  up_vector(f->q, v);
  float yaw = 0.0f;
  for (int i = 0; i < 3; i++) {
    yaw += v[i] * (gyro[i] - f->gyro_bias[i] + f->gyro_bias_corr[i]);
  }
  return -yaw; /* right-hand rule about "up" is counter-clockwise */
}
