#include "maneuver.h"

#include <math.h>
#include <string.h>

void maneuver_init(maneuver_t *m)
{
  memset(m, 0, sizeof(*m));
  m->n = 1.0f;
  m->since_last_s = MANEUVER_REVERSE_S + 1.0f;
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
  m->done_reversals = m->reversals;
  m->done_into_turn = false;
  m->active = false;
  m->kind = MANEUVER_NONE;
  m->reversals = 0;
  m->high_s = 0.0f;
  m->calm_s = 0.0f;
  return m->done;
}

/* The running turn is over: it becomes the last one (a short one is forgotten). */
static void end_turn(maneuver_t *m)
{
  if (m->turn_s >= MANEUVER_TURN_MIN_S) {
    m->last_sign = m->turn_sign;
    m->last_s = m->turn_s;
    m->last_peak_g = m->turn_peak_g;
    m->last_peak_dps = m->turn_peak_dps;
    m->since_last_s = m->turn_gap_s;
    m->between_peak_g = 0.0f;
  }
  m->turn_sign = 0;
  m->reversal_pending = false;
}

/* Turns from the signed turn rate; true when a swing the other way was just confirmed (a wingover reversal). */
static bool track_turns(maneuver_t *m, float n, float turn_dps, float dt_s)
{
  float rate = fabsf(turn_dps);
  int8_t s = turn_dps >= MANEUVER_TURN_DPS ? 1 : turn_dps <= -MANEUVER_TURN_DPS ? -1 : 0;
  if (m->turn_sign != 0) {
    if (s == m->turn_sign) {
      m->turn_s += dt_s + m->turn_gap_s; /* a short dip belongs to the turn */
      m->turn_gap_s = 0.0f;
    } else {
      m->turn_gap_s += dt_s;
      if (s != 0 || m->turn_gap_s > MANEUVER_TURN_MERGE_S) {
        end_turn(m);
      }
    }
  }
  if (m->turn_sign == 0) {
    m->since_last_s += dt_s;
    if (n > m->between_peak_g) {
      m->between_peak_g = n;
    }
    if (s != 0) {
      m->turn_started = true;
      m->turn_sign = s;
      m->turn_s = 0.0f;
      m->turn_gap_s = 0.0f;
      m->turn_peak_g = n;
      m->turn_peak_dps = rate;
      m->reversal_pending = m->last_sign == -s && m->since_last_s <= MANEUVER_REVERSE_S &&
                            m->last_s <= MANEUVER_TURN_MAX_S && m->last_peak_dps <= MANEUVER_TURN_MAX_DPS &&
                            m->flight_s >= MANEUVER_SETTLE_S;
    }
  }
  if (m->turn_sign == 0) {
    return false;
  }
  if (n > m->turn_peak_g) {
    m->turn_peak_g = n;
  }
  if (rate > m->turn_peak_dps && s == m->turn_sign) {
    m->turn_peak_dps = rate;
  }
  if (!m->reversal_pending || m->turn_s < MANEUVER_TURN_MIN_S) {
    return false;
  }
  m->reversal_pending = false;
  float peak = fmaxf(m->last_peak_g, fmaxf(m->between_peak_g, m->turn_peak_g));
  return peak >= MANEUVER_WINGOVER_G && m->turn_peak_dps <= MANEUVER_TURN_MAX_DPS;
}

maneuver_kind_t maneuver_update(maneuver_t *m, float load_g, float rate_dps, float turn_dps, float dt_s)
{
  if (!m->started) {
    m->n = load_g;
    m->started = true;
  } else {
    float alpha = dt_s / (MANEUVER_SMOOTH_S + dt_s);
    m->n += alpha * (load_g - m->n);
  }
  float n = m->n;
  m->flight_s += dt_s;
  if (n > m->g_max_flight) {
    m->g_max_flight = n;
  }
  bool spiralling = n >= MANEUVER_SPIRAL_G && rate_dps >= MANEUVER_SPIRAL_DPS;
  m->high_s = spiralling ? m->high_s + dt_s : 0.0f;
  m->turn_started = false;
  bool reversal = track_turns(m, n, turn_dps, dt_s);

  if (!m->active) {
    if (reversal) {
      m->active = true;
      m->kind = MANEUVER_WINGOVER;
      m->n_max = fmaxf(n, fmaxf(m->last_peak_g, fmaxf(m->between_peak_g, m->turn_peak_g)));
      m->peak_before_turn = fmaxf(m->last_peak_g, m->between_peak_g);
      m->turn_early_peak = fmaxf(n, m->turn_peak_g);
      m->reversals = 1;
    } else if (m->high_s >= MANEUVER_SPIRAL_S) {
      m->active = true;
      m->kind = MANEUVER_SPIRAL;
      m->n_max = n;
    }
    return MANEUVER_NONE;
  }

  if (m->kind == MANEUVER_WINGOVER) {
    if (reversal && m->reversals < 255U) {
      m->reversals++;
    }
    if (m->turn_started) {
      m->peak_before_turn = m->n_max;
      m->turn_early_peak = n;
    } else if (m->turn_sign != 0 && m->turn_s <= MANEUVER_TURN_EARLY_S && n > m->turn_early_peak) {
      m->turn_early_peak = n;
    }
    /* the swings are over when a turn goes on: report the wingover with its load up to that turn's first
     * second (not the spiral's), a spiral or circle follows on its own */
    if (m->turn_sign != 0 && m->turn_s > MANEUVER_TURN_MAX_S) {
      m->n_max = fmaxf(m->peak_before_turn, m->turn_early_peak);
      float high_s = m->high_s; /* a spiral that follows keeps its time at spiral load and rate */
      maneuver_kind_t k = finish(m);
      m->done_into_turn = true;
      m->high_s = high_s;
      return k;
    }
  }
  if (n > m->n_max) {
    m->n_max = n;
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
