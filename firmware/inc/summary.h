#ifndef SUMMARY_H
#define SUMMARY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Flight statistics and the flight summary at power off, without hardware
 * access (unit-tested). Statistics are collected once per second while
 * airborne (all flights of one power-on together); the summary is spoken
 * with the clips of the stock voice pack, values rounded to the clips that
 * exist (e.g. altitudes in 10 m steps up to 5000 m).
 * Stock profile keys: flightsummary_enable/duration/altitude/speed/climbrate_bool.
 */
typedef struct {
  bool enable, duration, altitude, speed, climbrate;
} summary_items_t;

typedef struct {
  bool flown;
  uint32_t airtime_s;
  float alt_max_m;
  float speed_max_ms;
  float climb_max_ms;  /* 5 s mean */
  float sink_max_ms;   /* 5 s mean, positive */
  float g_max;         /* peak load factor (maneuver.c), set by the caller */
  float vario_hist[5];
  unsigned vario_n;
  unsigned vario_i;
} flight_stats_t;

void summary_reset(flight_stats_t *s);
/* Once per second while airborne; speed only from a valid fix (< 0 = none). */
void summary_update(flight_stats_t *s, float alt_m, float speed_ms, float vario_ms);

#define SUMMARY_GROUPS 5
#define SUMMARY_CLIPS  4
#define SUMMARY_NAME   24

/* Clip groups to announce (each group one request of up to 4 clips); returns the group count. */
unsigned summary_clips(const flight_stats_t *s, const summary_items_t *items,
                       char clips[SUMMARY_GROUPS][SUMMARY_CLIPS][SUMMARY_NAME], unsigned counts[SUMMARY_GROUPS]);

/* Nearest value that has a clip (exported for the tests). */
int summary_altitude_clip(float m);
int summary_speed_clip(float kmh);
int summary_lift_clip(float cms);
int summary_sink_clip(float cms);
/* glide/glide_<tenths> of the extension clips: 0 to 5 in halves, 5 to 13 in tenths, 13 to 22 in halves, 22 to 30 whole. */
int summary_glide_clip(float ratio);

#endif
