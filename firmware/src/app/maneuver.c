#include "maneuver.h"

#include <math.h>
#include <string.h>

void maneuver_init(maneuver_t *m)
{
  memset(m, 0, sizeof(*m));
  m->n = 1.0f;
  m->since_low_s = MANEUVER_LOW_WINDOW_S + 1.0f;
}

float maneuver_bank_deg(float load_g)
{
  if (load_g <= 1.0f) {
    return 0.0f;
  }
  return acosf(1.0f / load_g) * (180.0f / (float)M_PI);
}

const char *maneuver_kind_name(maneuver_kind_t k)
{
  return k == MANEUVER_WINGOVER ? "wingover" : k == MANEUVER_SPIRAL ? "spiral" : "none";
}

static maneuver_kind_t finish(maneuver_t *m)
{
  m->done = m->kind;
  m->done_n_max = m->n_max;
  m->done_bank_deg = maneuver_bank_deg(m->n_max);
  m->active = false;
  m->kind = MANEUVER_NONE;
  m->high_s = 0.0f;
  m->calm_s = 0.0f;
  return m->done;
}

maneuver_kind_t maneuver_update(maneuver_t *m, float load_g, float rate_dps, float dt_s)
{
  if (!m->started) {
    m->n = load_g;
    m->started = true;
  } else {
    float alpha = dt_s / (MANEUVER_SMOOTH_S + dt_s);
    m->n += alpha * (load_g - m->n);
  }
  float n = m->n;
  if (n > m->g_max_flight) {
    m->g_max_flight = n;
  }
  m->since_low_s = n <= MANEUVER_LOW_G ? 0.0f : m->since_low_s + dt_s;
  bool spiralling = n >= MANEUVER_SPIRAL_G && rate_dps >= MANEUVER_SPIRAL_DPS;
  m->high_s = spiralling ? m->high_s + dt_s : 0.0f;

  if (!m->active) {
    if (n >= MANEUVER_HIGH_G && m->since_low_s <= MANEUVER_LOW_WINDOW_S) {
      m->active = true;
      m->kind = MANEUVER_WINGOVER;
      m->n_max = n;
    } else if (m->high_s >= MANEUVER_SPIRAL_S) {
      m->active = true;
      m->kind = MANEUVER_SPIRAL;
      m->n_max = n;
    }
    return MANEUVER_NONE;
  }

  if (n > m->n_max) {
    m->n_max = n;
  }
  if (m->kind == MANEUVER_WINGOVER && m->high_s >= MANEUVER_SPIRAL_S) {
    m->kind = MANEUVER_SPIRAL; /* the wingover went on into a spiral */
  }
  if (n < MANEUVER_END_G) {
    m->calm_s += dt_s;
    if (m->calm_s >= MANEUVER_END_S) {
      return finish(m);
    }
  } else {
    m->calm_s = 0.0f;
  }
  return MANEUVER_NONE;
}
