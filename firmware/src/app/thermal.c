#include "thermal.h"

#include <math.h>
#include <string.h>

#define RAD2DEG          57.29578f
#define MIN_TRACK_MS     2.0f   /* slower: the ground track is meaningless */
#define MAX_TURN_DEG     90.0f  /* more per second: a glitch */
#define CIRCLE_WINDOW_S  30U    /* a full turn within this starts circling */
#define STRAIGHT_S       10U    /* less than STRAIGHT_DEG within this ends it */
#define STRAIGHT_DEG     60.0f
#define THERMALING_MS 0.1f /* climb reports only while the mean climb is at least this */
#define EXIT_AVG_S       30U
#define EXIT_CLIMB_MS    0.2f
#define GLIDE_AVG_S      20U
#define GLIDE_MIN_SINK   0.3f   /* m/s: below, the glide ratio is meaningless */
#define GLIDE_MAX        60.0f  /* numbers in the voice pack */
#define FIT_TURN_DEG     330.0f /* the wind fit needs nearly a full turn */
#define FIT_MIN_POINTS   8U
#define FIT_AIRSPEED_MIN 5.5f   /* 20 km/h */
#define FIT_AIRSPEED_MAX 16.7f  /* 60 km/h */
#define FIT_MAX_RMS      1.5f   /* m/s */
#define WIND_NEW_MS      1.39f  /* 5 km/h */
#define WIND_NEW_DEG     45.0f

void thermal_init(thermal_t *t)
{
  memset(t, 0, sizeof(*t));
  t->since_end = THERMAL_WINDOW;
}

/* Index of the sample k seconds ago (k = 0: the newest). */
static unsigned idx(const thermal_t *t, unsigned k)
{
  return (t->head + THERMAL_WINDOW - 1U - k) % THERMAL_WINDOW;
}

static float sum_turn(const thermal_t *t, unsigned n)
{
  float s = 0.0f;
  for (unsigned k = 0; k < n && k < t->count; k++) {
    s += t->turn[idx(t, k)];
  }
  return s;
}

static float sum_gturn(const thermal_t *t, unsigned n)
{
  float s = 0.0f;
  for (unsigned k = 0; k < n && k < t->count; k++) {
    s += t->gturn[idx(t, k)];
  }
  return s;
}

static float wrap180(float d)
{
  while (d > 180.0f) {
    d -= 360.0f;
  }
  while (d <= -180.0f) {
    d += 360.0f;
  }
  return d;
}

/*
 * Least-squares circle through the velocity points of the last full turn
 * (Kasa fit on centred coordinates): its centre is the wind.
 */
static bool wind_fit(const thermal_t *t, float *we, float *wn)
{
  unsigned n = 0;
  float turned = 0.0f;
  while (n < t->count && turned < FIT_TURN_DEG) {
    unsigned i = idx(t, n);
    if (!t->vel_ok[i]) {
      return false;
    }
    turned += fabsf(t->turn[i]);
    n++;
  }
  if (turned < FIT_TURN_DEG || n < FIT_MIN_POINTS) {
    return false;
  }
  float mx = 0.0f, my = 0.0f;
  for (unsigned k = 0; k < n; k++) {
    mx += t->ve[idx(t, k)];
    my += t->vn[idx(t, k)];
  }
  mx /= (float)n;
  my /= (float)n;
  float suu = 0.0f, svv = 0.0f, suv = 0.0f, suz = 0.0f, svz = 0.0f, sz = 0.0f;
  for (unsigned k = 0; k < n; k++) {
    float u = t->ve[idx(t, k)] - mx;
    float v = t->vn[idx(t, k)] - my;
    float z = u * u + v * v;
    suu += u * u;
    svv += v * v;
    suv += u * v;
    suz += u * z;
    svz += v * z;
    sz += z;
  }
  float det = suu * svv - suv * suv;
  if (det < 1e-3f) {
    return false;
  }
  float uc = 0.5f * (suz * svv - svz * suv) / det;
  float vc = 0.5f * (svz * suu - suz * suv) / det;
  float r = sqrtf(sz / (float)n + uc * uc + vc * vc);
  float res = 0.0f;
  for (unsigned k = 0; k < n; k++) {
    float du = t->ve[idx(t, k)] - mx - uc;
    float dv = t->vn[idx(t, k)] - my - vc;
    float e = sqrtf(du * du + dv * dv) - r;
    res += e * e;
  }
  if (r < FIT_AIRSPEED_MIN || r > FIT_AIRSPEED_MAX || sqrtf(res / (float)n) > FIT_MAX_RMS) {
    return false;
  }
  *we = mx + uc;
  *wn = my + vc;
  return true;
}

static void circling_ended(thermal_t *t, const thermal_config_t *c, uint32_t now, unsigned *ev)
{
  t->circling = false;
  t->since_end = 0;
  unsigned back = STRAIGHT_S < t->count ? STRAIGHT_S : t->count - 1U; /* the turning ended about then */
  float gain = t->alt[idx(t, back)] - t->phase_alt;
  uint32_t duration = now - back - t->phase_t;
  if (duration >= c->exit_min_s && gain >= (float)c->exit_min_gain_m && !t->exit_done) {
    *ev |= THERMAL_EXIT;
    t->exit_climb_ms = t->thermal_ms;
    t->exit_gain_m = gain;
  }
  t->glide_armed = c->glide_min_gain_m == 0U || gain >= (float)c->glide_min_gain_m;
  t->straight_t = now;
  t->glide_said = false;
  if (t->fit_ok) {
    if (t->wind_valid) {
      t->wind_e = 0.5f * (t->wind_e + t->fit_e);
      t->wind_n = 0.5f * (t->wind_n + t->fit_n);
    } else {
      t->wind_e = t->fit_e;
      t->wind_n = t->fit_n;
      t->wind_valid = true;
    }
    float de = t->wind_e - t->said_e, dn = t->wind_n - t->said_n;
    float dir = fabsf(wrap180(atan2f(t->wind_e, t->wind_n) * RAD2DEG - atan2f(t->said_e, t->said_n) * RAD2DEG));
    if (!t->wind_said || sqrtf(de * de + dn * dn) > WIND_NEW_MS || dir > WIND_NEW_DEG) {
      *ev |= THERMAL_WIND;
      t->wind_said = true;
      t->said_e = t->wind_e;
      t->said_n = t->wind_n;
    }
  }
}

unsigned thermal_update(thermal_t *t, const thermal_config_t *c, uint32_t now, bool vel_valid, float ve, float vn,
                        float alt, bool gyro_valid, float gyro_dps)
{
  unsigned ev = 0;
  bool ok = vel_valid && sqrtf(ve * ve + vn * vn) >= MIN_TRACK_MS;
  float turn = 0.0f;
  if (ok) {
    float course = atan2f(ve, vn) * RAD2DEG;
    if (t->have_course) {
      turn = wrap180(course - t->last_course);
      if (fabsf(turn) > MAX_TURN_DEG) {
        turn = 0.0f;
      }
    }
    t->last_course = course;
  }
  t->have_course = ok;
  unsigned i = t->head;
  t->turn[i] = turn;
  t->gturn[i] = gyro_valid ? gyro_dps : 0.0f; /* one second of it */
  t->ve[i] = ve;
  t->vn[i] = vn;
  t->vel_ok[i] = ok;
  t->alt[i] = alt;
  t->head = (t->head + 1U) % THERMAL_WINDOW;
  if (t->count < THERMAL_WINDOW) {
    t->count++;
  }

  if (!t->circling && t->since_end < THERMAL_WINDOW) {
    t->since_end++;
  }
  unsigned window = t->since_end < CIRCLE_WINDOW_S ? t->since_end : CIRCLE_WINDOW_S;
  bool full_track = !t->circling && fabsf(sum_turn(t, window)) >= 360.0f;
  bool full_gyro = !t->circling && !full_track && fabsf(sum_gturn(t, window)) >= 360.0f;
  if (full_track || full_gyro) {
    /* The climb phase starts where the full turn began (in the series that saw it). */
    const float *series = full_track ? t->turn : t->gturn;
    unsigned back = 0;
    float acc = 0.0f;
    while (back + 1U < t->count && back < window && fabsf(acc) < 360.0f) {
      acc += series[idx(t, back)];
      back++;
    }
    t->circling = true;
    t->phase_t = now - back;
    t->phase_alt = t->alt[idx(t, back)];
    t->next_report = now + c->climb_report_s;
    t->exit_done = false;
    t->fit_ok = false;
    t->glide_armed = false;
  } else if (t->circling && t->count > STRAIGHT_S && fabsf(sum_turn(t, STRAIGHT_S)) < STRAIGHT_DEG &&
             fabsf(sum_gturn(t, STRAIGHT_S)) < STRAIGHT_DEG) {
    circling_ended(t, c, now, &ev);
  }

  if (t->circling) {
    if (now > t->phase_t) {
      t->thermal_ms = (alt - t->phase_alt) / (float)(now - t->phase_t);
    }
    if (c->climb_report_s && now >= t->next_report) {
      t->next_report = now + c->climb_report_s;
      if (t->thermal_ms >= THERMALING_MS) {
        ev |= THERMAL_CLIMB; /* thermaling, not just turning */
      }
    }
    if (!t->exit_done && now - t->phase_t >= c->exit_min_s && alt - t->phase_alt >= (float)c->exit_min_gain_m &&
        t->count > EXIT_AVG_S &&
        (alt - t->alt[idx(t, EXIT_AVG_S)]) / (float)EXIT_AVG_S < EXIT_CLIMB_MS) {
      ev |= THERMAL_EXIT; /* still circling, but the lift has gone */
      t->exit_done = true;
      t->exit_climb_ms = t->thermal_ms;
      t->exit_gain_m = alt - t->phase_alt;
    }
    float we, wn;
    if (wind_fit(t, &we, &wn)) {
      t->fit_ok = true;
      t->fit_e = we;
      t->fit_n = wn;
    }
  } else {
    if (t->straight_t == 0U) {
      t->straight_t = now; /* straight from the start: a glide report without a climb before, if configured */
      t->glide_armed = c->glide_min_gain_m == 0U;
      t->glide_said = false;
    }
    if (t->glide_armed) {
      float ratio;
      bool due = !t->glide_said ? now - t->straight_t >= c->glide_straight_s
                                : c->glide_repeat_min && now - t->glide_last_s >= c->glide_repeat_min * 60U;
      if (due && thermal_glide_now(t, &ratio)) {
        ev |= THERMAL_GLIDE;
        t->glide = ratio;
        t->glide_said = true;
        t->glide_last_s = now;
        if (!c->glide_repeat_min) {
          t->glide_armed = false; /* once per glide */
        }
      }
    }
  }
  return ev;
}

bool thermal_glide_now(const thermal_t *t, float *ratio)
{
  if (t->circling || t->count <= GLIDE_AVG_S || fabsf(sum_turn(t, GLIDE_AVG_S)) >= 90.0f) {
    return false;
  }
  float speed = 0.0f;
  for (unsigned k = 0; k < GLIDE_AVG_S; k++) {
    unsigned i = idx(t, k);
    if (!t->vel_ok[i]) {
      return false;
    }
    speed += sqrtf(t->ve[i] * t->ve[i] + t->vn[i] * t->vn[i]);
  }
  speed /= (float)GLIDE_AVG_S;
  float sink = (t->alt[idx(t, GLIDE_AVG_S)] - t->alt[idx(t, 0)]) / (float)GLIDE_AVG_S;
  if (sink < GLIDE_MIN_SINK || speed / sink > GLIDE_MAX) {
    return false;
  }
  *ratio = speed / sink;
  return true;
}

bool thermal_wind(const thermal_t *t, float *kmh, float *from_deg)
{
  if (!t->wind_valid) {
    return false;
  }
  *kmh = sqrtf(t->wind_e * t->wind_e + t->wind_n * t->wind_n) * 3.6f;
  float from = atan2f(-t->wind_e, -t->wind_n) * RAD2DEG;
  *from_deg = from < 0.0f ? from + 360.0f : from;
  return true;
}
