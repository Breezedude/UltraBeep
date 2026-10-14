#include "vario.h"

#include <stdio.h>
#include <string.h>

/* ---- vertical filter ---- */

void vario_filter_init(vario_filter_t *f, float q, float r)
{
  memset(f, 0, sizeof(*f));
  f->q = q;
  f->r = r;
}

void vario_filter_update(vario_filter_t *f, float altitude, float dt)
{
  if (!f->started) {
    f->h = altitude;
    f->v = 0.0f;
    f->p00 = f->r;
    f->p01 = 0.0f;
    f->p11 = 1.0f; /* climb rate unknown: about +-1 m/s */
    f->started = true;
    return;
  }
  /* predict: h += v dt; acceleration noise q enters as dt^4/4, dt^3/2, dt^2 */
  float dt2 = dt * dt;
  f->h += f->v * dt;
  float p00 = f->p00 + dt * 2.0f * f->p01 + dt2 * f->p11 + f->q * dt2 * dt2 * 0.25f;
  float p01 = f->p01 + dt * f->p11 + f->q * dt2 * dt * 0.5f;
  float p11 = f->p11 + f->q * dt2;

  /* update with the measured altitude */
  float s = p00 + f->r;
  float k0 = p00 / s;
  float k1 = p01 / s;
  float y = altitude - f->h;
  f->h += k0 * y;
  f->v += k1 * y;
  f->p00 = (1.0f - k0) * p00;
  f->p01 = (1.0f - k0) * p01;
  f->p11 = p11 - k1 * p01;
}

/* ---- tone map ---- */

void vario_tones_defaults(vario_tones_t *t)
{
  /* Stock "custom" preset of the original configuration. */
  static const vario_tone_point_t stock[VARIO_TONE_POINTS] = {
    {-500, 62, 450, 50}, {-350, 75, 450, 100}, {-50, 100, 450, 100}, {5, 440, 875, 50},
    {80, 513, 770, 26},  {120, 675, 500, 27},  {300, 845, 370, 30},  {800, 1000, 230, 45},
  };
  memcpy(t->point, stock, sizeof(stock));
  t->lift_start_cms = 10;
  t->sink_start_cms = -250;
  t->sink_alarm = true;
  t->sweep = true;
  t->lifty = true;
  t->integration_pct = 25; /* the stock default presets use 25 or 50 % */
}

static float lerp(float a, float b, float x)
{
  return a + (b - a) * x;
}

/* The curve at vz [cm/s]: linear between the points, flat beyond them. */
static void interpolate(const vario_tones_t *t, float vz, float *freq, float *period, float *duty)
{
  const vario_tone_point_t *p = t->point;
  if (vz <= p[0].vz_cms) {
    *freq = p[0].freq_hz;
    *period = p[0].period_ms;
    *duty = p[0].duty;
  } else if (vz >= p[VARIO_TONE_POINTS - 1].vz_cms) {
    *freq = p[VARIO_TONE_POINTS - 1].freq_hz;
    *period = p[VARIO_TONE_POINTS - 1].period_ms;
    *duty = p[VARIO_TONE_POINTS - 1].duty;
  } else {
    int i = 0;
    while (i < VARIO_TONE_POINTS - 2 && vz > p[i + 1].vz_cms) {
      i++;
    }
    float span = (float)(p[i + 1].vz_cms - p[i].vz_cms);
    float x = span > 0.0f ? (vz - p[i].vz_cms) / span : 0.0f;
    *freq = lerp(p[i].freq_hz, p[i + 1].freq_hz, x);
    *period = lerp(p[i].period_ms, p[i + 1].period_ms, x);
    *duty = lerp(p[i].duty, p[i + 1].duty, x);
  }
}

vario_tone_t vario_tone(const vario_tones_t *t, float climb_ms)
{
  vario_tone_t out = {false, 0, 0, 0};
  float vz = climb_ms * 100.0f;
  bool lift = vz >= (float)t->lift_start_cms;
  bool sink = t->sink_alarm && vz <= (float)t->sink_start_cms;
  if (!lift && !sink) {
    return out;
  }

  float freq, period, duty;
  interpolate(t, vz, &freq, &period, &duty);
  if (period < 20.0f) {
    period = 20.0f;
  }
  out.sound = freq >= 10.0f;
  out.freq_hz = (uint16_t)(freq + 0.5f);
  out.period_ms = (uint16_t)(period + 0.5f);
  out.on_ms = duty >= 99.5f ? out.period_ms : (uint16_t)(period * duty / 100.0f + 0.5f);
  return out;
}

vario_tone_t vario_tone_weak(const vario_tones_t *t, float climb_ms)
{
  vario_tone_t out = {false, 0, 0, 0};
  float vz = climb_ms * 100.0f;
  if (!t->lifty || vz >= (float)t->lift_start_cms || vz < (float)(t->lift_start_cms - VARIO_WEAK_BAND_CMS)) {
    return out;
  }
  float freq, period, duty;
  interpolate(t, vz, &freq, &period, &duty);
  out.sound = freq >= 10.0f;
  out.freq_hz = (uint16_t)(freq + 0.5f);
  out.period_ms = VARIO_WEAK_PERIOD_MS;
  out.on_ms = VARIO_WEAK_ON_MS;
  return out;
}

float vario_integration_tau_s(uint8_t pct)
{
  static const float tau[5] = {0.1f, 0.5f, 1.2f, 2.6f, 5.5f}; /* at 0, 25, 50, 75, 100 % */
  if (pct >= 100U) {
    return tau[4];
  }
  unsigned i = pct / 25U;
  return lerp(tau[i], tau[i + 1], (float)(pct % 25U) / 25.0f);
}

bool vario_tone_cut_short(const vario_tones_t *t, float climb_ms, bool lift_running, uint16_t on_ms,
                          uint32_t elapsed_ms)
{
  if (climb_ms * 100.0f < (float)t->lift_start_cms) {
    return false;
  }
  vario_tone_t next = vario_tone(t, climb_ms);
  if (!next.sound) {
    return false;
  }
  if (!lift_running) {
    return true;
  }
  return elapsed_ms >= on_ms && elapsed_ms >= next.period_ms;
}

bool vario_tones_parse(const char *args, vario_tones_t *t)
{
  int lift, sink, alarm, used;
  if (sscanf(args, "%d %d %d%n", &lift, &sink, &alarm, &used) != 3) {
    return false;
  }
  args += used;
  t->lift_start_cms = (int16_t)lift;
  t->sink_start_cms = (int16_t)sink;
  t->sink_alarm = alarm != 0;
  for (int i = 0; i < VARIO_TONE_POINTS; i++) {
    int vz, f, per, duty;
    if (sscanf(args, " %d,%d,%d,%d%n", &vz, &f, &per, &duty, &used) != 4) {
      return false;
    }
    args += used;
    t->point[i].vz_cms = (int16_t)vz;
    t->point[i].freq_hz = (uint16_t)(f < 0 ? 0 : f > 5000 ? 5000 : f);
    t->point[i].period_ms = (uint16_t)(per < 20 ? 20 : per > 5000 ? 5000 : per);
    t->point[i].duty = (uint8_t)(duty < 0 ? 0 : duty > 100 ? 100 : duty);
  }
  int weak, sweep, integ;
  int n = sscanf(args, " %d %d %d", &weak, &sweep, &integ);
  if (n >= 1) {
    t->lifty = weak != 0;
  }
  if (n >= 2) {
    t->sweep = sweep != 0;
  }
  if (n >= 3) {
    t->integration_pct = (uint8_t)(integ < 0 ? 0 : integ > 100 ? 100 : integ);
  }
  return true;
}
