#include "flight.h"

#include <math.h>
#include <string.h>

#define SPEED_TAKEOFF_MS  4.1667f /* 15 km/h */
#define SPEED_HOLD_S      5U
#define DISTANCE_M        50.0f  /* from the launch point */
#define STILL_MS          0.5556f /* 2 km/h: standing (the Doppler speed of a standing receiver stays far below) */
#define HDOP_GOOD         3.0f   /* at the launch of 2026-10-12: 1.1 typical, 1.8 at most */
#define ANCHOR_N          30U    /* the launch point: mean of up to 30 standing seconds, then following */
#define AWAY_SHARE        0.7f   /* the ground speed must account for this share of the distance */
#define AWAY_HOLD_S       2U
#define ALT_ONSET_M       2.0f   /* the climb or sink of the altitude indicator began where it left this band */
#define ALTITUDE_M        15.0f
#define VARIO_TAKEOFF_MS  1.5f
#define VARIO_HOLD_S      5U
#define SPEED_LANDED_MS   0.8333f /* 3 km/h */
#define VARIO_LANDED_MS   0.3f
#define IMPACT_G          4.0f
#define SLOW_MS           2.7778f /* 10 km/h */
#define IMPACT_HOLD_S     10U
#define CLIMB5_LANDED_MS  0.5f  /* barometer over 5 s; on the ground it stayed below 0.3 */
#define SHOCK_LOW_G       1.0f  /* a shock: the load also drops below this within the second */
#define SHOCK_RECENT_S    30U
#define HANDLING_DPS      250.0f
#define HANDLING_LOW_G    0.5f
#define HANDLING_COUNT    3
#define HANDLING_GROUND_S 20U
#define M_PER_E7_DEG      0.0111319f /* metres per 1e-7 degree of latitude */

void flight_init(flight_detector_t *d)
{
  memset(d, 0, sizeof(*d));
  d->state = FLIGHT_GROUND;
  d->landing_enabled = true;
  d->landing_time_s = FLIGHT_LANDING_DEFAULT_S;
  d->landing = (flight_landing_t){true, IMPACT_G, SLOW_MS, IMPACT_HOLD_S, true};
  d->takeoff = (flight_takeoff_t){SPEED_TAKEOFF_MS, SPEED_HOLD_S, DISTANCE_M, ALTITUDE_M, VARIO_TAKEOFF_MS,
                                  VARIO_HOLD_S};
}

static float clampf(float v, float lo, float hi)
{
  return v < lo ? lo : v > hi ? hi : v;
}

static uint8_t clamp_s(uint8_t v, uint8_t lo, uint8_t hi)
{
  return v < lo ? lo : v > hi ? hi : v;
}

void flight_set_takeoff(flight_detector_t *d, const flight_takeoff_t *t)
{
  d->takeoff.speed_ms = clampf(t->speed_ms, 5.0f / 3.6f, 50.0f / 3.6f);
  d->takeoff.speed_s = clamp_s(t->speed_s, 1, 30);
  d->takeoff.distance_m = clampf(t->distance_m, 10.0f, 500.0f);
  d->takeoff.altitude_m = clampf(t->altitude_m, 5.0f, 100.0f);
  d->takeoff.climb_ms = clampf(t->climb_ms, 0.5f, 5.0f);
  d->takeoff.climb_s = clamp_s(t->climb_s, 1, 30);
}

void flight_set_landing(flight_detector_t *d, bool enabled, uint32_t seconds)
{
  d->landing_enabled = enabled;
  d->landing_time_s = seconds < 10U ? 10U : seconds > 600U ? 600U : seconds;
}

void flight_set_landing_rules(flight_detector_t *d, const flight_landing_t *l)
{
  d->landing.impact = l->impact;
  d->landing.impact_g = clampf(l->impact_g, 3.0f, 15.0f);
  d->landing.slow_ms = clampf(l->slow_ms, 3.0f / 3.6f, 30.0f / 3.6f);
  d->landing.impact_s = clamp_s(l->impact_s, 3, 60);
  d->landing.handling = l->handling;
}

const char *flight_state_name(flight_state_t s)
{
  switch (s) {
  case FLIGHT_GROUND: return "ground";
  case FLIGHT_AIRBORNE: return "flight";
  case FLIGHT_LANDED: return "landed";
  default: return "?";
  }
}

static float distance_e7(int32_t lat_a, int32_t lon_a, int32_t lat_b, int32_t lon_b)
{
  float lat = (float)lat_a * 1e-7f * 0.017453293f;
  float dy = (float)(lat_a - lat_b) * M_PER_E7_DEG;
  float dx = (float)(lon_a - lon_b) * M_PER_E7_DEG * cosf(lat);
  return sqrtf(dx * dx + dy * dy);
}

/* Oldest sample of the window (at most FLIGHT_WINDOW_S old), optionally with a fix. */
static const flight_input_t *oldest(const flight_detector_t *d, uint32_t now, bool need_fix)
{
  const unsigned size = FLIGHT_WINDOW_S + 1;
  for (unsigned k = d->count; k > 0; k--) {
    const flight_input_t *s = &d->hist[(d->head + size - k) % size];
    if (now - s->t_s <= FLIGHT_WINDOW_S && (!need_fix || s->fix)) {
      return s;
    }
  }
  return NULL;
}

/*
 * The launch point, before the first takeoff only: the standing position (good fixes, below 2 km/h),
 * the mean of the first standing seconds, then following the device whenever it stands again. True
 * once the device is the takeoff distance away from it for AWAY_HOLD_S seconds of good fixes and the
 * ground speed since it last stood accounts for most of that distance: a GPS jump, or a position that
 * wanders while the receiver stands, moves no speed. Called once per second.
 */
static bool left_launch_point(flight_detector_t *d, const flight_input_t *in, bool good)
{
  if (d->state != FLIGHT_GROUND || !good) {
    d->away_s = 0;
    return false;
  }
  if (in->speed_ms < STILL_MS) {
    if (d->anchor_n < ANCHOR_N) {
      d->anchor_n++;
    }
    if (d->anchor_n == 1U) {
      d->anchor_lat_e7 = in->lat_e7;
      d->anchor_lon_e7 = in->lon_e7;
    } else {
      d->anchor_lat_e7 += (int32_t)lroundf((float)(in->lat_e7 - d->anchor_lat_e7) / (float)d->anchor_n);
      d->anchor_lon_e7 += (int32_t)lroundf((float)(in->lon_e7 - d->anchor_lon_e7) / (float)d->anchor_n);
    }
    d->still_s = in->t_s;
    d->away_m = 0.0f;
  } else {
    d->away_m += in->speed_ms; /* one second at this ground speed */
  }
  if (!d->anchor_n) {
    return false;
  }
  float dist = distance_e7(in->lat_e7, in->lon_e7, d->anchor_lat_e7, d->anchor_lon_e7);
  bool out = dist >= d->takeoff.distance_m && d->away_m >= AWAY_SHARE * dist;
  d->away_s = out ? (uint8_t)(d->away_s < 255U ? d->away_s + 1U : 255U) : 0U;
  return d->away_s >= AWAY_HOLD_S;
}

/* When the altitude change of the window began: after the last sample still within ALT_ONSET_M of its start. */
static uint32_t altitude_onset(const flight_detector_t *d, const flight_input_t *o, uint32_t now)
{
  const unsigned size = FLIGHT_WINDOW_S + 1;
  uint32_t onset = o->t_s;
  for (unsigned k = d->count; k > 0; k--) { /* oldest first */
    const flight_input_t *s = &d->hist[(d->head + size - k) % size];
    if (s->t_s >= o->t_s && fabsf(s->alt_m - o->alt_m) < ALT_ONSET_M) {
      onset = s->t_s;
    }
  }
  return onset + 1U < now ? onset + 1U : now;
}

static uint32_t earliest(uint32_t a, uint32_t b)
{
  return (a == 0U || (b != 0U && b < a)) ? b : a;
}

/* Climb rate from the barometer over the last `span` seconds (the window); false if no sample. */
static bool climb_over(const flight_detector_t *d, const flight_input_t *in, uint32_t span, float *out)
{
  const unsigned size = FLIGHT_WINDOW_S + 1;
  for (unsigned k = 1; k <= d->count; k++) { /* newest first: the sample closest to the span */
    const flight_input_t *s = &d->hist[(d->head + size - k) % size];
    uint32_t age = in->t_s - s->t_s;
    if (age >= span && age <= span + 2U) {
      *out = (in->alt_m - s->alt_m) / (float)age;
      return true;
    }
  }
  return false;
}

static unsigned bits(uint32_t v)
{
  unsigned n = 0;
  for (; v; v &= v - 1U) {
    n++;
  }
  return n;
}

static void reset_ground(flight_detector_t *d)
{
  d->calm_since = 0;
  d->ground_since = 0;
  d->shock_s = 0;
  d->first_shock_s = 0;
  d->handling_bits = 0;
}

void flight_force_airborne(flight_detector_t *d, uint32_t now_s)
{
  d->state = FLIGHT_AIRBORNE;
  d->takeoff_s = now_s;
  reset_ground(d);
}

flight_event_t flight_update(flight_detector_t *d, const flight_input_t *in)
{
  const unsigned size = FLIGHT_WINDOW_S + 1;
  uint32_t now = in->t_s;

  /* takeoff indicators; the GPS ones on good fixes only */
  bool good = in->fix && in->hdop > 0.0f && in->hdop <= HDOP_GOOD;
  bool fast = good && in->speed_ms >= d->takeoff.speed_ms;
  d->fast_since = fast ? (d->fast_since ? d->fast_since : now) : 0U;
  bool strong_vario = fabsf(in->vario_ms) >= d->takeoff.climb_ms;
  d->vario_since = strong_vario ? (d->vario_since ? d->vario_since : now) : 0U;
  bool speed = d->fast_since && now - d->fast_since + 1U >= d->takeoff.speed_s;
  bool vario_held = d->vario_since && now - d->vario_since + 1U >= d->takeoff.climb_s;
  const flight_input_t *o = oldest(d, now, false);
  bool altitude = o && fabsf(in->alt_m - o->alt_m) >= d->takeoff.altitude_m;
  uint32_t altitude_from = altitude ? altitude_onset(d, o, now) : 0U;
  bool away = left_launch_point(d, in, good);

  d->hist[d->head] = *in;
  d->head = (d->head + 1U) % size;
  if (d->count < size) {
    d->count++;
  }

  flight_event_t ev = FLIGHT_EVENT_NONE;
  if (d->state != FLIGHT_AIRBORNE) {
    if (in->fix && (away || speed || vario_held || altitude)) {
      /* when it began: the end of standing at the launch point (if the device stood there within the
       * window), the start of the fast phase or of the climb or sink */
      uint32_t first = 0;
      bool stood = d->state == FLIGHT_GROUND && d->still_s && now - d->still_s <= FLIGHT_WINDOW_S;
      first = earliest(first, away || stood ? d->still_s + 1U : 0U);
      first = earliest(first, speed ? d->fast_since : 0U);
      first = earliest(first, vario_held ? d->vario_since : 0U);
      first = earliest(first, altitude_from);
      d->state = FLIGHT_AIRBORNE;
      reset_ground(d);
      d->takeoff_s = first && first <= now ? first : now;
      ev = FLIGHT_EVENT_TAKEOFF;
    }
  } else if (d->landing_enabled) {
    bool calm = in->fix && in->speed_ms < SPEED_LANDED_MS && fabsf(in->vario_ms) < VARIO_LANDED_MS;
    d->calm_since = calm ? (d->calm_since ? d->calm_since : now) : 0U;

    /* on the ground: slow (when the speed is known) and no constant climb or sink */
    float c5 = 0.0f;
    bool ground = climb_over(d, in, 5U, &c5) && fabsf(c5) < CLIMB5_LANDED_MS &&
                  (!in->fix || in->speed_ms < d->landing.slow_ms);
    d->ground_since = ground ? (d->ground_since ? d->ground_since : now) : 0U;
    uint32_t ground_s = d->ground_since ? now - d->ground_since + 1U : 0U;

    bool shock = in->motion && in->acc_max_g >= d->landing.impact_g && in->acc_min_g < SHOCK_LOW_G;
    if (shock) {
      if (!d->shock_s || now - d->shock_s > SHOCK_RECENT_S) {
        d->first_shock_s = now;
      }
      d->shock_s = now;
    }
    bool handling = in->motion && in->rate_max_dps >= HANDLING_DPS && in->acc_min_g < HANDLING_LOW_G;
    d->handling_bits = (d->handling_bits << 1) | (handling ? 1U : 0U);

    uint32_t at = 0;
    if (d->calm_since && now - d->calm_since >= d->landing_time_s) {
      at = d->calm_since;
    } else if (d->landing.impact && d->shock_s && now - d->shock_s <= SHOCK_RECENT_S &&
               ground_s >= d->landing.impact_s) {
      at = d->first_shock_s;
    } else if (d->landing.handling && bits(d->handling_bits & 0x3FFFFFFFU) >= HANDLING_COUNT &&
               ground_s >= HANDLING_GROUND_S) {
      at = d->ground_since;
    }
    if (at) {
      d->state = FLIGHT_LANDED;
      d->landing_s = at;
      reset_ground(d);
      /* the window starts afresh: the impact and handling rules land within 30 s of the touchdown,
       * and the approach still in the window would at once be the altitude change of a new takeoff */
      d->count = 0;
      ev = FLIGHT_EVENT_LANDING;
    }
  }
  return ev;
}
