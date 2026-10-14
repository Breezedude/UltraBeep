#include "summary.h"
#include "sound.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void summary_reset(flight_stats_t *s)
{
  memset(s, 0, sizeof(*s));
  s->alt_max_m = -1e6f;
}

void summary_update(flight_stats_t *s, float alt_m, float speed_ms, float vario_ms)
{
  s->flown = true;
  s->airtime_s++;
  if (alt_m > s->alt_max_m) {
    s->alt_max_m = alt_m;
  }
  if (speed_ms > s->speed_max_ms) {
    s->speed_max_ms = speed_ms;
  }
  s->vario_hist[s->vario_i] = vario_ms;
  s->vario_i = (s->vario_i + 1U) % 5U;
  if (s->vario_n < 5U) {
    s->vario_n++;
  }
  if (s->vario_n == 5U) {
    float m = 0.0f;
    for (int i = 0; i < 5; i++) {
      m += s->vario_hist[i];
    }
    m /= 5.0f;
    if (m > s->climb_max_ms) {
      s->climb_max_ms = m;
    }
    if (-m > s->sink_max_ms) {
      s->sink_max_ms = -m;
    }
  }
}

/* Clip values: ranges of [first, last, step]; the nearest value wins. */
typedef struct {
  int first, last, step;
} clip_range_t;

static int nearest(float v, const clip_range_t *r, unsigned n)
{
  int best = r[0].first;
  float best_d = 1e9f;
  for (unsigned i = 0; i < n; i++) {
    float x = v < (float)r[i].first ? (float)r[i].first : v > (float)r[i].last ? (float)r[i].last : v;
    int k = (int)floorf((x - (float)r[i].first) / (float)r[i].step + 0.5f);
    int c = r[i].first + k * r[i].step;
    float d = fabsf(v - (float)c);
    if (d < best_d) {
      best_d = d;
      best = c;
    }
  }
  return best;
}

int summary_altitude_clip(float m)
{
  static const clip_range_t r[] = {{-100, 5000, 10}, {5100, 7000, 100}};
  return nearest(m, r, 2);
}

int summary_speed_clip(float kmh)
{
  static const clip_range_t r[] = {{0, 79, 1}, {85, 200, 5}};
  return nearest(kmh, r, 2);
}

int summary_lift_clip(float cms)
{
  static const clip_range_t r[] = {{0, 50, 10}, {100, 1000, 50}};
  return nearest(cms, r, 2);
}

int summary_sink_clip(float cms)
{
  static const clip_range_t r[] = {{10, 50, 10}, {100, 1050, 50}, {1150, 1950, 100}};
  return nearest(cms, r, 3);
}

unsigned summary_clips(const flight_stats_t *s, const summary_items_t *items,
                       char clips[SUMMARY_GROUPS][SUMMARY_CLIPS][SUMMARY_NAME], unsigned counts[SUMMARY_GROUPS])
{
  unsigned g = 0;
  if (!s->flown || !items->enable) {
    return 0;
  }
  if (items->duration) {
    uint32_t min = (s->airtime_s + 30U) / 60U;
    uint32_t h = min / 60U;
    unsigned c = 0;
    snprintf(clips[g][c++], SUMMARY_NAME, "time/duration");
    if (h > 0U) {
      snprintf(clips[g][c++], SUMMARY_NAME, "time/duration_%lu", (unsigned long)(h > 23U ? 23U : h));
    }
    snprintf(clips[g][c++], SUMMARY_NAME, "time/min_%lu", (unsigned long)(min % 60U));
    counts[g++] = c;
  }
  if (items->altitude) {
    snprintf(clips[g][0], SUMMARY_NAME, "altitude_max");
    snprintf(clips[g][1], SUMMARY_NAME, "al/al_%d", summary_altitude_clip(s->alt_max_m));
    counts[g++] = 2;
  }
  if (items->speed) {
    snprintf(clips[g][0], SUMMARY_NAME, "speed_max");
    snprintf(clips[g][1], SUMMARY_NAME, "sp/sp_%d", summary_speed_clip(s->speed_max_ms * 3.6f));
    counts[g++] = 2;
  }
  if (items->climbrate) {
    snprintf(clips[g][0], SUMMARY_NAME, "vz_max");
    snprintf(clips[g][1], SUMMARY_NAME, "vario_lift_%d", summary_lift_clip(s->climb_max_ms * 100.0f));
    snprintf(clips[g][2], SUMMARY_NAME, SOUND_PAUSE_CLIP);
    snprintf(clips[g][3], SUMMARY_NAME, "vario_sink_%d", summary_sink_clip(s->sink_max_ms * 100.0f));
    counts[g++] = 4;
  }
  return g;
}
