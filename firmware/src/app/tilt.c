#include "tilt.h"

#include <math.h>
#include <string.h>

#define G_MS2   9.80665f
#define DEG_RAD 0.017453293f

static float dot3(const float a[3], const float b[3])
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float norm3(const float a[3])
{
  return sqrtf(dot3(a, a));
}

static void cross3(const float a[3], const float b[3], float out[3])
{
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}

void tilt_init(tilt_t *t)
{
  memset(t, 0, sizeof(*t));
}

/* M, L and F from the sums, once enough straight flight and turns were seen (then they only get finer). */
static void learn_axes(tilt_t *t, float dt)
{
  if ((float)t->n_straight * dt < TILT_STRAIGHT_S || (float)t->n_turn * dt < TILT_TURN_S) {
    return;
  }
  float mn = norm3(t->m_sum);
  if (mn <= 0.0f) {
    return;
  }
  float m[3] = {t->m_sum[0] / mn, t->m_sum[1] / mn, t->m_sum[2] / mn};
  float along = dot3(t->l_sum, m);
  float l[3] = {t->l_sum[0] - along * m[0], t->l_sum[1] - along * m[1], t->l_sum[2] - along * m[2]};
  float ln = norm3(l);
  if (ln / (float)t->n_turn < TILT_MIN_LEAN) {
    return;
  }
  for (int k = 0; k < 3; k++) {
    l[k] /= ln;
    t->M[k] = m[k];
  }
  cross3(l, m, t->F);
  t->axes = true;
}

bool tilt_update(tilt_t *t, const float a[3], const float w[3], float dt, bool in_flight, float speed_ms)
{
  float wc[3] = {w[0] - t->bias[0], w[1] - t->bias[1], w[2] - t->bias[2]};
  float wcn = norm3(wc);
  if (!t->started) {
    for (int k = 0; k < 3; k++) {
      t->fs[k] = a[k];
      t->ws[k] = wc[k];
    }
    t->rs = wcn;
    t->started = true;
  } else {
    float alpha = dt / (TILT_SMOOTH_S + dt);
    for (int k = 0; k < 3; k++) {
      t->fs[k] += alpha * (a[k] - t->fs[k]);
      t->ws[k] += alpha * (wc[k] - t->ws[k]);
    }
    t->rs += alpha * (wcn - t->rs);
  }
  float fsn = norm3(t->fs), wsn = norm3(t->ws);
  float load = fsn / G_MS2;

  if (!in_flight) {
    /* lying still on the ground: the gyro bias, a plain mean at first, then a running one */
    if (t->rs < TILT_STILL_DPS * DEG_RAD && wcn < TILT_STILL_DPS * DEG_RAD && fabsf(norm3(a) / G_MS2 - 1.0f) < TILT_BIAS_G) {
      t->n_bias++;
      float k = fmaxf(1.0f / (float)t->n_bias, dt / TILT_BIAS_S);
      for (int i = 0; i < 3; i++) {
        t->bias[i] += k * (w[i] - t->bias[i]);
      }
    }
  } else if (fsn > 0.0f) {
    if (wsn < TILT_STRAIGHT_DPS * DEG_RAD && fabsf(load - 1.0f) < TILT_STRAIGHT_G) {
      for (int k = 0; k < 3; k++) {
        t->m_sum[k] += t->fs[k] / fsn;
      }
      t->n_straight++;
      learn_axes(t, dt);
    } else if (wsn > TILT_TURN_MIN_DPS * DEG_RAD && wsn < TILT_TURN_MAX_DPS * DEG_RAD &&
               fabsf(load - 1.0f) < TILT_TURN_G) {
      /* the true vertical is the rotation vector turned up (in a right turn it points down); its lean
       * across M, signed by the turn, points to the pilot's left: in both directions that is -w */
      for (int k = 0; k < 3; k++) {
        t->l_sum[k] -= t->ws[k] / wsn;
      }
      t->n_turn++;
      learn_axes(t, dt);
    }
  }

  /* the specific force less the centripetal acceleration of the flight path, w x (V F) */
  float fc[3] = {a[0], a[1], a[2]};
  if (t->axes) {
    float vf[3] = {speed_ms * t->F[0], speed_ms * t->F[1], speed_ms * t->F[2]}, cen[3];
    cross3(wc, vf, cen);
    for (int k = 0; k < 3; k++) {
      fc[k] -= cen[k];
    }
  }
  float fcn = norm3(fc);
  if (!t->u_ok) {
    if (fcn > 0.5f * G_MS2) {
      for (int k = 0; k < 3; k++) {
        t->u[k] = fc[k] / fcn;
      }
      t->u_ok = true;
    }
  } else {
    /* a vector fixed in the earth frame turns against the device: du/dt = u x w */
    float du[3];
    cross3(t->u, wc, du);
    for (int k = 0; k < 3; k++) {
      t->u[k] += du[k] * dt;
    }
    if (fabsf(fcn / G_MS2 - 1.0f) < TILT_GATE) {
      float k = dt / TILT_TAU_S;
      for (int i = 0; i < 3; i++) {
        t->u[i] += k * (fc[i] / fcn - t->u[i]);
      }
    }
    bool spin = wsn > TILT_SPIN_DPS * DEG_RAD && wsn > TILT_SPIN_COHER * t->rs;
    t->spin_s = spin ? t->spin_s + dt : 0.0f;
    float along = spin ? dot3(t->ws, t->u) / wsn : 0.0f;
    if (t->spin_s >= TILT_SPIN_HOLD_S && fabsf(along) > TILT_SPIN_ALIGN) {
      float k = dt / TILT_SPIN_TAU_S;
      float sg = along < 0.0f ? -1.0f : 1.0f; /* the axis, turned to the side of u: up */
      for (int i = 0; i < 3; i++) {
        t->u[i] += k * (sg * t->ws[i] / wsn - t->u[i]);
      }
    }
    float un = norm3(t->u);
    if (un > 0.0f) {
      for (int k = 0; k < 3; k++) {
        t->u[k] /= un;
      }
    }
  }
  if (!t->axes || !t->u_ok) {
    return false;
  }
  float c = dot3(t->u, t->M);
  t->tilt_deg = acosf(c > 1.0f ? 1.0f : c < -1.0f ? -1.0f : c) / DEG_RAD;
  return true;
}
