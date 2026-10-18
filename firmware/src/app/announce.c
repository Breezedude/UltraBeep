#include "announce.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtc_time.h"
#include "sound.h"
#include "summary.h"

#define AUTO_MIN_GAP_S 30U   /* never two announcements of one automatic trigger closer than this */
#define HEADING_MIN_KMH 3.0f /* below this the GNSS course is meaningless */

const char *const announce_block_names[ANN_BLOCKS] = {
  "altitude", "climbrate", "speed", "heading", "duration", "time", "wind", "glide", "battery",
  "tone", "thermalclimb", "thermalgain", "maxg", "bank", "maxaltitude", "maxspeed", "maxclimb",
};

const char *const announce_trigger_names[ANN_TRIGGERS] = {
  "summary", "tap", "auto", "elevation", "thermalexit", "thermaling", "glide", "wind", "wingover",
};

const char *const announce_matrix_names[MATRIX_ITEMS] = {
  "altitude", "climbrate", "speed", "heading", "duration", "time",
};

const char *const announce_signal_names[ANN_SIGNALS] = {
  "tritone_up", "octave_leap", "bi_dup_twice", "tick_tock", "chirp_up", "trill",
  "low_octave", "low_bi_dup_twice", "low_trill", "tock_tock", "low_chirp_up",
};

/* Blocks that stay silent while a phone is connected, when nothing else is said (configurator BLOCKS). */
static bool block_ble_default(announce_block_t b)
{
  switch (b) {
  case ANN_CLIMBRATE:
  case ANN_BATTERY:
  case ANN_TONE:
  case ANN_THERMALCLIMB:
  case ANN_THERMALGAIN:
    return false;
  default:
    return true;
  }
}

static const char *const dirs[] = {"n", "ne", "e", "se", "s", "sw", "w", "nw"};

static const char *direction(float deg)
{
  float c = fmodf(deg, 360.0f);
  if (c < 0.0f) {
    c += 360.0f;
  }
  return dirs[(int)((c + 22.5f) / 45.0f) % 8];
}

void announce_config_defaults(announce_config_t *c)
{
  memset(c, 0, sizeof(*c));
  c->elevation_gain_m = -1;
  c->elevation_loss_m = -1;
  c->thermalexit_min_s = 60;
  c->thermalexit_min_gain_m = 50;
  c->climb_report_s = 20;
  c->min_gap_s = 15;
  c->glide_straight_s = 30;
  c->glide_min_gain_m = 100;
  c->glide_repeat_min = 0;
  c->summary_enable = true;
  for (int i = 0; i < 4; i++) {
    c->summary_item[i] = true;
  }
}

/* False when the list is full or has the block already. */
static bool list_add(announce_list_t *l, announce_block_t b, bool ble)
{
  if (l->count >= ANN_LIST_MAX) {
    return false;
  }
  for (unsigned i = 0; i < l->count; i++) {
    if (l->block[i] == b) {
      return false; /* each block once */
    }
  }
  if (ble) {
    l->ble_mute |= (uint16_t)(1U << l->count);
  }
  l->block[l->count++] = (uint8_t)b;
  return true;
}

/* The words after the block name, each after a ':' ("tone:ble:trill"): "ble", or a signal tone's name. */
static void parse_options(const char *opt, const char *end, bool *ble, int *signal)
{
  while (opt && opt < end) {
    const char *s = opt + 1;
    const char *e = memchr(s, ':', (size_t)(end - s));
    const char *stop = e ? e : end;
    while (s < stop && *s == ' ') {
      s++;
    }
    size_t n = (size_t)(stop - s);
    while (n && s[n - 1] == ' ') {
      n--;
    }
    if (n == 3 && strncmp(s, "ble", 3) == 0) {
      *ble = true;
    }
    for (int i = 0; i < ANN_SIGNALS; i++) {
      if (strlen(announce_signal_names[i]) == n && strncmp(announce_signal_names[i], s, n) == 0) {
        *signal = i;
      }
    }
    opt = e;
  }
}

void announce_list_parse(announce_list_t *l, const char *text)
{
  memset(l, 0, sizeof(*l));
  l->set = true;
  const char *p = text;
  while (*p) {
    while (*p == ' ' || *p == ',') {
      p++;
    }
    const char *end = p;
    while (*end && *end != ',') {
      end++;
    }
    const char *opt = memchr(p, ':', (size_t)(end - p));
    size_t name_len = (size_t)((opt ? opt : end) - p);
    while (name_len && p[name_len - 1] == ' ') {
      name_len--;
    }
    bool ble = false;
    int signal = 0;
    parse_options(opt, end, &ble, &signal);
    for (int b = 0; b < ANN_BLOCKS; b++) {
      if (strlen(announce_block_names[b]) == name_len && strncmp(announce_block_names[b], p, name_len) == 0) {
        if (list_add(l, (announce_block_t)b, ble) && b == ANN_TONE) {
          l->tone = (uint8_t)signal;
        }
        break;
      }
    }
    p = end;
  }
}

/* The stock items in the stock order, as the configurator derives the lists. */
static void list_from_matrix(announce_list_t *l, const bool items[MATRIX_ITEMS])
{
  static const uint8_t block_of[MATRIX_ITEMS] = {ANN_ALTITUDE, ANN_CLIMBRATE, ANN_SPEED,
                                                 ANN_HEADING,  ANN_DURATION,  ANN_TIME};
  memset(l, 0, sizeof(*l));
  for (int i = 0; i < MATRIX_ITEMS; i++) {
    if (items[i]) {
      list_add(l, (announce_block_t)block_of[i], block_ble_default((announce_block_t)block_of[i]));
    }
  }
}

static void list_single(announce_list_t *l, announce_block_t b, bool ble)
{
  memset(l, 0, sizeof(*l));
  list_add(l, b, ble);
}

void announce_config_resolve(announce_config_t *c)
{
  announce_list_t *l = c->list;
  if (!l[TRIG_SUMMARY].set) {
    static const uint8_t blocks[4] = {ANN_DURATION, ANN_MAXALTITUDE, ANN_MAXSPEED, ANN_MAXCLIMB};
    memset(&l[TRIG_SUMMARY], 0, sizeof(l[0]));
    for (int i = 0; i < 4; i++) {
      if (c->summary_item[i]) {
        list_add(&l[TRIG_SUMMARY], (announce_block_t)blocks[i], block_ble_default((announce_block_t)blocks[i]));
      }
    }
  }
  if (!c->enable_set[TRIG_SUMMARY]) {
    c->enable[TRIG_SUMMARY] = c->summary_enable;
  }
  if (!l[TRIG_TAP].set) {
    list_from_matrix(&l[TRIG_TAP], c->tap_item);
  }
  if (!l[TRIG_AUTO].set) {
    list_from_matrix(&l[TRIG_AUTO], c->auto_item);
  }
  if (!l[TRIG_ELEVATION].set) {
    list_from_matrix(&l[TRIG_ELEVATION], c->auto_item);
  }
  if (!c->enable_set[TRIG_ELEVATION]) {
    c->enable[TRIG_ELEVATION] = c->enable[TRIG_AUTO];
  }
  if (!l[TRIG_THERMALEXIT].set) {
    list_single(&l[TRIG_THERMALEXIT], ANN_TONE, false);
  }
  if (!c->enable_set[TRIG_THERMALEXIT]) {
    c->enable[TRIG_THERMALEXIT] = true;
  }
  if (!l[TRIG_THERMALING].set) {
    list_single(&l[TRIG_THERMALING], ANN_THERMALCLIMB, false);
  }
  if (!c->enable_set[TRIG_THERMALING]) {
    c->enable[TRIG_THERMALING] = false;
  }
  if (!l[TRIG_GLIDE].set) {
    list_single(&l[TRIG_GLIDE], ANN_GLIDE, block_ble_default(ANN_GLIDE));
  }
  if (!c->enable_set[TRIG_GLIDE]) {
    c->enable[TRIG_GLIDE] = true;
  }
  if (!l[TRIG_WIND].set) {
    list_single(&l[TRIG_WIND], ANN_WIND, block_ble_default(ANN_WIND));
  }
  if (!c->enable_set[TRIG_WIND]) {
    c->enable[TRIG_WIND] = false;
  }
  if (!l[TRIG_WINGOVER].set) {
    memset(&l[TRIG_WINGOVER], 0, sizeof(l[0]));
    list_add(&l[TRIG_WINGOVER], ANN_MAXG, true);
    list_add(&l[TRIG_WINGOVER], ANN_BANK, true);
  }
  if (!c->enable_set[TRIG_WINGOVER]) {
    c->enable[TRIG_WINGOVER] = true;
  }
}

uint16_t announce_elevation_gain(const announce_config_t *c)
{
  return c->elevation_gain_m >= 0 ? (uint16_t)c->elevation_gain_m : c->alt_trigger_m;
}

uint16_t announce_elevation_loss(const announce_config_t *c)
{
  return c->elevation_loss_m >= 0 ? (uint16_t)c->elevation_loss_m : c->alt_trigger_m;
}

/* ---------------------------------------------------------------- clips */

static void put(announcement_t *out, unsigned clip, const char *fmt, int value)
{
  snprintf(out->clip[out->groups][clip], ANN_NAME, fmt, value);
}

static void word(announcement_t *out, unsigned clip, const char *name)
{
  snprintf(out->clip[out->groups][clip], ANN_NAME, "%s", name);
}

#define G_SERIES_MAX 100 /* g/g_100: 10.0 g, above the 8 g the accelerometer measures */

/* "<n> dot <tenth>" from a value in tenths; from 20 on whole numbers (the clips go to 60). */
static unsigned tenths_clips(announcement_t *out, unsigned n, int v10)
{
  if (v10 >= 200) {
    put(out, n++, "%d", (v10 + 5) / 10 > 60 ? 60 : (v10 + 5) / 10);
  } else {
    put(out, n++, "%d", v10 / 10);
    word(out, n++, "dot");
    put(out, n++, "%d", v10 % 10);
  }
  return n;
}

static void climb_clip(announcement_t *out, unsigned n, float climb_ms)
{
  float cms = climb_ms * 100.0f;
  if (cms >= -5.0f) {
    put(out, n, "vario_lift_%d", summary_lift_clip(cms < 0.0f ? 0.0f : cms));
  } else {
    put(out, n, "vario_sink_%d", summary_sink_clip(-cms));
  }
}

static bool build_block(announce_block_t block, const announce_input_t *in, announcement_t *out)
{
  unsigned n = 0;
  if (out->groups >= ANN_GROUPS) {
    return false;
  }
  switch (block) {
  case ANN_ALTITUDE:
    put(out, n++, "al/al_%d", summary_altitude_clip(in->alt_m));
    break;
  case ANN_CLIMBRATE:
    climb_clip(out, n++, in->climb_ms);
    break;
  case ANN_SPEED:
    if (!in->fix) {
      return false;
    }
    put(out, n++, "sp/sp_%d", summary_speed_clip(in->speed_kmh));
    break;
  case ANN_HEADING:
    if (!in->fix || in->speed_kmh < HEADING_MIN_KMH) {
      return false;
    }
    word(out, n++, "nav/heading");
    snprintf(out->clip[out->groups][n++], ANN_NAME, "nav/%s", direction(in->course_deg));
    break;
  case ANN_WIND:
    if (!in->wind_valid) {
      return false;
    }
    word(out, n++, "nav/wind");
    if (in->wind_kmh >= 2.0f) {
      snprintf(out->clip[out->groups][n++], ANN_NAME, "nav/%s", direction(in->wind_from_deg));
    }
    put(out, n++, "sp/sp_%d", summary_speed_clip(in->wind_kmh));
    break;
  case ANN_GLIDE:
    if (!in->glide_valid) {
      return false;
    }
    if (in->glide_word) {
      word(out, n++, "glide_ratio");
    }
    n = tenths_clips(out, n, (int)(in->glide_ratio * 10.0f + 0.5f));
    break;
  case ANN_DURATION: {
    if (!in->in_flight && !in->stats_valid) {
      return false;
    }
    uint32_t min = (in->airtime_s + 30U) / 60U;
    uint32_t h = min / 60U;
    word(out, n++, "time/duration");
    if (h > 0U) {
      put(out, n++, "time/duration_%d", (int)(h > 23U ? 23U : h));
    }
    put(out, n++, "time/min_%d", (int)(min % 60U));
    break;
  }
  case ANN_TIME: {
    if (!in->time_valid) {
      return false;
    }
    uint32_t min = in->local_s / 60U;
    word(out, n++, "time/time");
    put(out, n++, "time/hour_%d", (int)(min / 60U % 24U));
    put(out, n++, "time/min_%d", (int)(min % 60U));
    break;
  }
  case ANN_BATTERY: {
    if (in->battery_percent < 0) {
      return false;
    }
    int p = (in->battery_percent + 12) / 25 * 25; /* clips for 0, 25, 50, 75, 100 % */
    word(out, n++, "battery");
    put(out, n++, "%d_percent", p > 100 ? 100 : p);
    break;
  }
  case ANN_TONE:
    out->count[out->groups++] = 0; /* the melody, played by the caller */
    return true;
  case ANN_THERMALCLIMB:
    if (!in->thermal_valid) {
      return false;
    }
    climb_clip(out, n++, in->thermal_climb_ms);
    break;
  case ANN_THERMALGAIN:
    if (!in->thermal_valid) {
      return false;
    }
    put(out, n++, "al/al_%d", summary_altitude_clip(in->thermal_gain_m < 0.0f ? 0.0f : in->thermal_gain_m));
    break;
  case ANN_MAXG: {
    if (in->g_load <= 0.0f) {
      return false;
    }
    if (in->g_word) {
      word(out, n++, "g_load");
    }
    int g10 = (int)(in->g_load * 10.0f + 0.5f);
    if (in->g_series) {
      put(out, n++, "g/g_%d", g10 > G_SERIES_MAX ? G_SERIES_MAX : g10); /* "2,3 G" in one go */
    } else {
      n = tenths_clips(out, n, g10);
    }
    break;
  }
  case ANN_BANK: {
    if (in->g_load <= 0.0f) {
      return false;
    }
    if (in->bank_word) {
      word(out, n++, "bank_angle");
    }
    if (in->bank_series) {
      int deg5 = (int)(in->bank_deg / 5.0f + 0.5f) * 5; /* "60 Grad" in one go */
      put(out, n++, "bank/bank_%d", deg5 < 0 ? 0 : deg5 > 180 ? 180 : deg5);
      break;
    }
    int deg = (int)(in->bank_deg + 0.5f);
    if (deg > 60) {
      deg = (deg + 5) / 10 * 10; /* the voice pack has 0..60, 70, 80, 90 */
    }
    put(out, n++, "%d", deg > 90 ? 90 : deg);
    break;
  }
  case ANN_MAXALTITUDE:
    if (!in->stats_valid) {
      return false;
    }
    word(out, n++, "altitude_max");
    put(out, n++, "al/al_%d", summary_altitude_clip(in->max_alt_m));
    break;
  case ANN_MAXSPEED:
    if (!in->stats_valid) {
      return false;
    }
    word(out, n++, "speed_max");
    put(out, n++, "sp/sp_%d", summary_speed_clip(in->max_speed_kmh));
    break;
  case ANN_MAXCLIMB:
    if (!in->stats_valid) {
      return false;
    }
    /* "max climb 2.5 ... minus 4.0": the sink clip speaks a negative number, so no "max sink" word */
    word(out, n++, "vz_max");
    put(out, n++, "vario_lift_%d", summary_lift_clip(in->max_climb_ms * 100.0f));
    word(out, n++, SOUND_PAUSE_CLIP);
    put(out, n++, "vario_sink_%d", summary_sink_clip(in->max_sink_ms * 100.0f));
    break;
  default:
    return false;
  }
  out->count[out->groups++] = n;
  return true;
}

void announce_demo_input(announce_input_t *in)
{
  bool glide_word = in->glide_word, g_word = in->g_word, bank_word = in->bank_word;
  bool g_series = in->g_series, bank_series = in->bank_series;
  memset(in, 0, sizeof(*in));
  in->in_flight = true;
  in->alt_m = 1234.0f;
  in->climb_ms = 1.5f;
  in->fix = true;
  in->speed_kmh = 42.0f;
  in->course_deg = 95.0f; /* east */
  in->airtime_s = 62U * 60U;
  in->time_valid = true;
  in->local_s = 14U * 3600U + 35U * 60U;
  in->battery_percent = 75;
  in->wind_valid = true;
  in->wind_kmh = 12.0f;
  in->wind_from_deg = 270.0f; /* from the west */
  in->glide_valid = true;
  in->glide_ratio = 8.7f;
  in->thermal_valid = true;
  in->thermal_climb_ms = 2.1f;
  in->thermal_gain_m = 350.0f;
  in->g_load = 2.3f;
  in->bank_deg = 64.0f;
  in->stats_valid = true;
  in->max_alt_m = 2345.0f;
  in->max_speed_kmh = 43.0f;
  in->max_climb_ms = 2.5f;
  in->max_sink_ms = 4.0f;
  in->glide_word = glide_word;
  in->g_word = g_word;
  in->bank_word = bank_word;
  in->g_series = g_series;
  in->bank_series = bank_series;
}

void announce_build(const announce_list_t *l, bool ble_connected, const announce_input_t *in, announcement_t *out)
{
  memset(out, 0, sizeof(*out));
  for (unsigned i = 0; i < l->count; i++) {
    if (ble_connected && (l->ble_mute & (1U << i))) {
      continue;
    }
    unsigned g = out->groups;
    if (build_block((announce_block_t)l->block[i], in, out) && l->block[i] == ANN_TONE) {
      out->signal[g] = l->tone < ANN_SIGNALS ? l->tone : 0U;
    }
  }
}

/* ---------------------------------------------------------------- automatic triggers */

void announce_auto_start(announce_auto_t *a, uint32_t now_s, float alt_m)
{
  memset(a, 0, sizeof(*a));
  a->alt_ref = alt_m;
  a->last_s = now_s;
  a->elev_s = now_s;
}

/* Distance between two positions; equirectangular, plenty for steps of one second. */
static float distance_m(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2)
{
  const float rad = 3.14159265f / 180.0f / 1e7f;
  float dlat = (float)(lat2 - lat1) * rad;
  float dlon = (float)(lon2 - lon1) * rad * cosf((float)lat1 * rad);
  return 6371000.0f * sqrtf(dlat * dlat + dlon * dlon);
}

void announce_auto_restart(announce_auto_t *a, uint32_t now_s, float alt_m)
{
  a->dist_m = 0.0f;
  a->last_s = now_s;
  a->alt_ref = alt_m;
  a->elev_s = now_s;
}

unsigned announce_auto_due(announce_auto_t *a, const announce_config_t *c, uint32_t now_s, float alt_m, bool fix,
                           int32_t lat_e7, int32_t lon_e7, bool hold)
{
  unsigned due = 0;
  if (fix) {
    if (a->have_pos) {
      a->dist_m += distance_m(a->lat_e7, a->lon_e7, lat_e7, lon_e7);
    }
    a->lat_e7 = lat_e7;
    a->lon_e7 = lon_e7;
    a->have_pos = true;
  }
  if (hold) {
    return 0; /* the distance goes on adding up; the triggers fire after the gap */
  }
  if (c->enable[TRIG_AUTO] && now_s - a->last_s >= AUTO_MIN_GAP_S &&
      ((c->dist_trigger_m && a->dist_m >= (float)c->dist_trigger_m) ||
       (c->time_trigger_min && now_s - a->last_s >= c->time_trigger_min * 60U))) {
    a->dist_m = 0.0f;
    a->last_s = now_s;
    due |= ANN_DUE_AUTO;
  }
  uint16_t gain = announce_elevation_gain(c), loss = announce_elevation_loss(c);
  if (c->enable[TRIG_ELEVATION] && now_s - a->elev_s >= AUTO_MIN_GAP_S &&
      ((gain && alt_m - a->alt_ref >= (float)gain) || (loss && a->alt_ref - alt_m >= (float)loss))) {
    a->alt_ref = alt_m;
    a->elev_s = now_s;
    due |= ANN_DUE_ELEVATION;
  }
  return due;
}

/* ---------------------------------------------------------------- local time */

static uint8_t month_days(uint16_t year, uint8_t month)
{
  static const uint8_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  bool leap = (year % 4U == 0U && year % 100U != 0U) || year % 400U == 0U;
  return (uint8_t)(days[month - 1] + (month == 2 && leap ? 1 : 0));
}

/* 00:00 of the nth Sunday (1, 2) of a month, or of the last one (nth < 0); 1970-01-01 was a Thursday. */
static uint32_t sunday(uint16_t year, uint8_t month, int nth)
{
  if (nth < 0) {
    uint32_t t = unix_from_civil(year, month, month_days(year, month), 0, 0, 0);
    return t - (t / 86400U + 4U) % 7U * 86400U;
  }
  uint32_t t = unix_from_civil(year, month, 1, 0, 0, 0);
  uint32_t first = t + (7U - (t / 86400U + 4U) % 7U) % 7U * 86400U;
  return first + (uint32_t)(nth - 1) * 7U * 86400U;
}

dst_rule_t announce_dst_rule_parse(const char *name)
{
  if (strcmp(name, "eu") == 0) {
    return DST_EU;
  }
  if (strcmp(name, "us") == 0) {
    return DST_US;
  }
  if (strcmp(name, "au") == 0) {
    return DST_AU;
  }
  if (strcmp(name, "nz") == 0) {
    return DST_NZ;
  }
  if (strcmp(name, "cl") == 0) {
    return DST_CL;
  }
  return DST_NONE;
}

bool announce_summer_time(uint32_t utc, int offset_min, dst_rule_t rule)
{
  uint16_t y;
  uint8_t mo, d, h, mi, s;
  const uint32_t hour = 3600U;
  if (rule == DST_EU) {
    civil_from_unix(utc, &y, &mo, &d, &h, &mi, &s);
    return utc >= sunday(y, 3, -1) + hour && utc < sunday(y, 10, -1) + hour;
  }
  /* the other rules switch at local standard time */
  uint32_t local = (uint32_t)((int64_t)utc + (int64_t)offset_min * 60);
  civil_from_unix(local, &y, &mo, &d, &h, &mi, &s);
  switch (rule) {
  case DST_US: return local >= sunday(y, 3, 2) + 2U * hour && local < sunday(y, 11, 1) + hour;
  case DST_AU: return local < sunday(y, 4, 1) + 2U * hour || local >= sunday(y, 10, 1) + 2U * hour;
  case DST_NZ: return local < sunday(y, 4, 1) + 2U * hour || local >= sunday(y, 9, -1) + 2U * hour;
  case DST_CL: return local < sunday(y, 4, 1) || local >= sunday(y, 9, 1);
  default: return false;
  }
}

uint32_t announce_local_seconds(uint32_t utc, int offset_min, dst_rule_t rule)
{
  int32_t off = offset_min * 60 + (announce_summer_time(utc, offset_min, rule) ? 3600 : 0);
  int64_t local = (int64_t)utc + off;
  return (uint32_t)(((local % 86400) + 86400) % 86400);
}

void announce_tz_table_parse(const char *text, tz_table_t *t)
{
  t->n = 0;
  const char *p = text;
  while (*p && t->n < TZ_TABLE_MAX) {
    while (*p == ' ' || *p == ',') {
      p++;
    }
    char *end;
    long date = strtol(p, &end, 10);
    if (end == p) {
      break;
    }
    const char *q = end;
    while (*q == ' ') {
      q++;
    }
    long off = strtol(q, &end, 10);
    if (end == q) {
      break; /* a truncated last entry */
    }
    p = end;
    unsigned y = 2000U + (unsigned)(date / 10000), mo = (unsigned)(date / 100 % 100), d = (unsigned)(date % 100);
    if (date < 0 || mo < 1 || mo > 12 || d < 1 || d > 31 || off < -720 || off > 840) {
      continue;
    }
    uint32_t at = unix_from_civil((uint16_t)y, (uint8_t)mo, (uint8_t)d, 1, 0, 0);
    if (t->n && at <= t->at[t->n - 1]) {
      continue; /* keeps the entries in order */
    }
    t->at[t->n] = at;
    t->min[t->n] = (int16_t)off;
    t->n++;
  }
}

int announce_utc_offset_min(uint32_t utc, int offset_min, bool tz_auto, dst_rule_t rule, const tz_table_t *table)
{
  if (!tz_auto) {
    return offset_min;
  }
  if (table && table->n && utc >= table->at[0] && utc < table->at[table->n - 1] + 366U * 86400U) {
    int off = table->min[0];
    for (uint8_t i = 1; i < table->n && utc >= table->at[i]; i++) {
      off = table->min[i];
    }
    return off;
  }
  return offset_min + (announce_summer_time(utc, offset_min, rule) ? 60 : 0);
}
