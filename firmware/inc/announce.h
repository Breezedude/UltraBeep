#ifndef ANNOUNCE_H
#define ANNOUNCE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Announcements: triggers, each with an ordered list of blocks
 * (docs/config-ext-format.md, "Announcements"). A block becomes one group of
 * voice clips - or the signal tone - spoken one after the other; blocks
 * without data (no fix, no wind estimate yet) are left out. The lists come
 * from announce_<trigger>_items_string; while that key is missing,
 * announce_config_resolve() derives the list from the stock keys BipLink
 * writes (voiceauto_<item>_bool, voicetap_<item>_bool, flightsummary_*) and
 * fixed defaults for the triggers the stock does not have.
 */

typedef enum {
  ANN_ALTITUDE,     /* al/al_<m> */
  ANN_CLIMBRATE,    /* vario_lift_<cm/s> / vario_sink_<cm/s>, averaged over 10 s */
  ANN_SPEED,        /* sp/sp_<km/h> */
  ANN_HEADING,      /* nav/heading nav/<n|ne|e|se|s|sw|w|nw> */
  ANN_DURATION,     /* time/duration [time/duration_<h>] time/min_<m> */
  ANN_TIME,         /* time/time time/hour_<h> time/min_<m>, local time */
  ANN_WIND,         /* nav/wind nav/<from> sp/sp_<km/h> */
  ANN_GLIDE,        /* [glide_ratio] <n> dot <tenth> */
  ANN_BATTERY,      /* battery <n>_percent */
  ANN_TONE,         /* the signal tone on the piezo (MELODY_THERMAL_EXIT), no clips */
  ANN_THERMALCLIMB, /* vario_lift_<cm/s>: mean climb since the circling began */
  ANN_THERMALGAIN,  /* al/al_<m>: height gained since the circling began */
  ANN_MAXG,         /* [g_load] g/g_<tenths> (without that series: <n> dot <tenth>): peak load factor */
  ANN_BANK,         /* [bank_angle] <deg>: bank of a coordinated turn at that load */
  ANN_MAXALTITUDE,  /* altitude_max al/al_<m> (flight summary) */
  ANN_MAXSPEED,     /* speed_max sp/sp_<km/h> */
  ANN_MAXCLIMB,     /* vz_max vario_lift_<cm/s> pause vario_sink_<cm/s> (the sink clip says the minus) */
  ANN_BLOCKS
} announce_block_t;

extern const char *const announce_block_names[ANN_BLOCKS]; /* key part: "altitude", ... */

typedef enum {
  TRIG_SUMMARY,     /* at power off after a flight (flightsummary_enable_bool) */
  TRIG_TAP,         /* double tap (voicetap_enable_bool) */
  TRIG_AUTO,        /* time or distance (voiceauto_enable_bool, voiceauto_time/distancetrigger_int) */
  TRIG_ELEVATION,   /* height gained or lost (announce_elevation_*) */
  TRIG_THERMALEXIT, /* a climb phase ends (announce_thermalexit_*) */
  TRIG_THERMALING,  /* every announce_thermalclimb_int s while circling with a mean climb */
  TRIG_GLIDE,       /* after announce_glide_straighttime_int s of straight flight */
  TRIG_WIND,        /* circling ends with a new wind estimate */
  TRIG_WINGOVER,    /* after a wingover or a spiral (maneuver.c) */
  ANN_TRIGGERS
} announce_trigger_t;

extern const char *const announce_trigger_names[ANN_TRIGGERS]; /* key part: "summary", ... */

/* The items of the stock keys voiceauto_<item>_bool and voicetap_<item>_bool, for the derived lists. */
typedef enum {
  MATRIX_ALTITUDE, MATRIX_CLIMBRATE, MATRIX_SPEED, MATRIX_HEADING, MATRIX_DURATION, MATRIX_TIME, MATRIX_ITEMS
} announce_matrix_t;

extern const char *const announce_matrix_names[MATRIX_ITEMS];

#define ANN_LIST_MAX 10

typedef struct {
  bool set;                  /* the items key was present (an empty value counts) */
  uint8_t count;
  uint8_t block[ANN_LIST_MAX];
  uint16_t ble_mute;         /* bit i: block i is not spoken while a phone is connected */
} announce_list_t;

typedef struct {
  announce_list_t list[ANN_TRIGGERS];
  bool enable[ANN_TRIGGERS];
  bool enable_set[ANN_TRIGGERS];
  /* trigger settings */
  bool tap_before_takeoff;        /* voicetap_enablebeforetakeoff_bool */
  bool tap_when_silent;           /* voicetap_enablewhensilent_bool: also at the flight level mute */
  uint16_t time_trigger_min;      /* voiceauto_timetrigger_int, 0 = off */
  uint32_t dist_trigger_m;        /* voiceauto_distancetrigger_int, 0 = off */
  uint16_t alt_trigger_m;         /* voiceauto_altitudetrigger_int: the default of the elevation trigger */
  int32_t elevation_gain_m;       /* announce_elevation_gain_int, -1 = not set (alt_trigger_m), 0 = off */
  int32_t elevation_loss_m;       /* announce_elevation_loss_int */
  uint16_t thermalexit_min_s;     /* announce_thermalexit_mintime_int */
  uint16_t thermalexit_min_gain_m; /* announce_thermalexit_mingain_int */
  uint16_t climb_report_s;        /* announce_thermalclimb_int */
  uint16_t glide_straight_s;      /* announce_glide_straighttime_int */
  uint16_t glide_min_gain_m;      /* announce_glidemingain_int, 0 = also without a climb before */
  uint16_t glide_repeat_min;      /* announce_glide_repeat_int, 0 = once per glide */
  /* the stock keys, for the derived lists */
  bool auto_item[MATRIX_ITEMS];   /* voiceauto_<item>_bool */
  bool tap_item[MATRIX_ITEMS];    /* voicetap_<item>_bool */
  bool summary_enable;            /* flightsummary_enable_bool */
  bool summary_item[4];           /* flightsummary_duration/altitude/speed/climbrate_bool */
} announce_config_t;

void announce_config_defaults(announce_config_t *c);
/* "block[:ble],block,..." -> list; unknown blocks are skipped, at most ANN_LIST_MAX. */
void announce_list_parse(announce_list_t *l, const char *text);
/* Lists and enables without a key: from the stock keys and defaults. Once after the files are read. */
void announce_config_resolve(announce_config_t *c);
/* The thresholds of the elevation trigger (the stock altitude trigger when not set). */
uint16_t announce_elevation_gain(const announce_config_t *c);
uint16_t announce_elevation_loss(const announce_config_t *c);

/* What the device knows now. */
typedef struct {
  bool in_flight;
  float alt_m;            /* GNSS-referenced barometric altitude */
  float climb_ms;         /* averaged climb rate */
  bool fix;               /* 3D fix */
  float speed_kmh;
  float course_deg;
  uint32_t airtime_s;
  bool time_valid;
  uint32_t local_s;       /* local time, seconds since midnight */
  int battery_percent;    /* < 0: unknown */
  bool wind_valid;
  float wind_kmh;
  float wind_from_deg;    /* direction the wind comes from */
  bool glide_valid;
  float glide_ratio;
  bool thermal_valid;     /* circling: the climb phase so far, or the one that just ended */
  float thermal_climb_ms;
  float thermal_gain_m;
  float g_load;           /* peak load factor of the manoeuvre or the flight; <= 0: none */
  float bank_deg;
  bool stats_valid;       /* flight summary */
  float max_alt_m;
  float max_speed_kmh;
  float max_climb_ms;
  float max_sink_ms;      /* positive */
  bool glide_word, g_word, bank_word; /* extension clips glide_ratio, g_load, bank_angle in the voice pack */
  bool g_series;          /* extension clips g/g_0 ... g/g_100: the load factor as one clip per tenth */
} announce_input_t;

#define ANN_CLIPS  4
#define ANN_NAME   24
#define ANN_GROUPS ANN_LIST_MAX /* one group per block */

typedef struct {
  unsigned groups;
  unsigned count[ANN_GROUPS];            /* 0: the signal tone instead of clips */
  char clip[ANN_GROUPS][ANN_CLIPS][ANN_NAME];
} announcement_t;

/* Plausible fixed values for a demo of a list (configurator play button); the word flags stay as they are. */
void announce_demo_input(announce_input_t *in);
/* Clip groups for the blocks of `l`; with ble_connected, blocks marked :ble are left out. */
void announce_build(const announce_list_t *l, bool ble_connected, const announce_input_t *in, announcement_t *out);

/* Automatic triggers (time or distance; height gained or lost): state from the takeoff on. */
typedef struct {
  float alt_ref;      /* altitude at the last elevation announcement */
  float dist_m;       /* distance since the last time/distance announcement */
  int32_t lat_e7, lon_e7;
  bool have_pos;
  uint32_t last_s;    /* time of the last time/distance announcement */
  uint32_t elev_s;    /* time of the last elevation announcement */
} announce_auto_t;

#define ANN_DUE_AUTO      1U
#define ANN_DUE_ELEVATION 2U

void announce_auto_start(announce_auto_t *a, uint32_t now_s, float alt_m);
/* Once per second in flight; ANN_DUE_* bits of the triggers that fire (they restart then). */
unsigned announce_auto_due(announce_auto_t *a, const announce_config_t *c, uint32_t now_s, float alt_m, bool fix,
                           int32_t lat_e7, int32_t lon_e7);

/* Summer time rules of the time zones (system_dstrule_string, 0.33). */
typedef enum {
  DST_NONE = 0,
  DST_EU, /* last Sunday of March 01:00 UTC .. last Sunday of October 01:00 UTC */
  DST_US, /* second Sunday of March 02:00 .. first Sunday of November 02:00 local (USA, Canada) */
  DST_AU, /* first Sunday of October 02:00 .. first Sunday of April 03:00 local (south-east Australia) */
  DST_NZ, /* last Sunday of September 02:00 .. first Sunday of April 03:00 local (New Zealand) */
  DST_CL, /* first Sunday of September 00:00 .. first Sunday of April 00:00 local (Chile) */
} dst_rule_t;
/* "eu", "us", "au", "nz", "cl", "none"; anything else: DST_NONE. */
dst_rule_t announce_dst_rule_parse(const char *name);
/* True while the rule's summer time is in force; offset_min is the zone's standard offset from UTC. */
bool announce_summer_time(uint32_t utc, int offset_min, dst_rule_t rule);
/* Local time of day in seconds: UTC + offset, plus one hour while the summer time is in force. */
uint32_t announce_local_seconds(uint32_t utc, int offset_min, dst_rule_t rule);

/* The stock's time change table (global_timechanges_longstring in config.ultrabip, written by the BipLink app
 * for the phone's time zone): "yymmdd offset," entries, the UTC offset in minutes from that day on. Entries
 * take effect at 01:00 UTC of their day (the EU switch moment). */
#define TZ_TABLE_MAX 32
typedef struct {
  uint8_t n;
  uint32_t at[TZ_TABLE_MAX]; /* unix time the entry takes effect */
  int16_t min[TZ_TABLE_MAX]; /* the offset from then on */
} tz_table_t;
void announce_tz_table_parse(const char *text, tz_table_t *t);
/* The offset from UTC in minutes at utc. tz_auto set: the table's entry in force when the table covers utc
 * (from its first entry to a year after its last), otherwise offset_min plus the rule's summer hour.
 * tz_auto clear: offset_min as it is. With the stock app's files offset_min already holds the summer
 * offset, so the table must decide, not the rule on top of it. */
int announce_utc_offset_min(uint32_t utc, int offset_min, bool tz_auto, dst_rule_t rule, const tz_table_t *table);

#endif
