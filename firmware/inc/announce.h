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
  ANN_CLIMBRATE,    /* [climb] vario_lift_<cm/s> / vario_sink_<cm/s>: the mean over the span (10 s unless chosen) */
  ANN_SPEED,        /* sp/sp_<km/h> */
  ANN_HEADING,      /* nav/heading nav/<n|ne|e|se|s|sw|w|nw> */
  ANN_DURATION,     /* time/duration [time/duration_<h>] time/min_<m> */
  ANN_TIME,         /* time/time time/hour_<h> time/min_<m>, local time */
  ANN_WIND,         /* wind sp/sp_<km/h> nav/from_<dir> (without those clips: nav/wind nav/<dir> sp/sp_<km/h>) */
  ANN_GLIDE,        /* [glide_ratio] glide/glide_<tenths> (without the series: <n> dot <tenth>), over the span (20 s) */
  ANN_BATTERY,      /* battery <n>_percent */
  ANN_TONE,         /* a signal tone on the piezo (announce_signal_names, the list's choice), no clips */
  ANN_THERMALGAIN,  /* [gain|height_gain] al/al_<m>: height gained since the circling began, or over the span */
  ANN_MAXG,         /* [g_load] g/g_<tenths> (without that series: <n> dot <tenth>): peak load factor */
  ANN_BANK,         /* [bank_angle] bank/bank_<deg, steps of 5> (without them: <deg>): bank of a coordinated turn */
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

/*
 * The signal tones of the block "tone" (the notes are in sound.c, in this order): chosen per list with the
 * option ":<name>" (`tone:ble:trill`); without one the first. Firmware before 0.57 reads the name as no option
 * and plays its own tone.
 */
#define ANN_SIGNALS 11
extern const char *const announce_signal_names[ANN_SIGNALS];

/*
 * Options of a block, words after colons in the items string (`climbrate:ble:30s:climb`), stored per list
 * entry in a byte: the low nibble the span the value covers, as an index into announce_span_s (0 = the
 * block's own default: 10 s for climbrate, 20 s for glide, the climb phase for thermalgain), the high nibble
 * the variant - the signal of "tone", or the word spoken before the number (announce_word_names: "climb"
 * for climbrate, "gain" and "heightgain" for thermalgain; 0 = the number alone). Unknown words are ignored,
 * so older firmware reads the ":ble" and speaks its usual form.
 */
#define ANN_SPANS 6 /* index 0: the default */
extern const uint8_t announce_span_s[ANN_SPANS]; /* 0, 10, 15, 20, 30, 60 */
#define ANN_WORDS 4
extern const char *const announce_word_names[ANN_WORDS]; /* "", "climb", "gain", "heightgain" (the option words) */
#define ANN_OPT(span, variant) ((uint8_t)((span) | ((variant) << 4)))
#define ANN_OPT_SPAN(o)        ((unsigned)((o) & 0x0F))
#define ANN_OPT_VARIANT(o)     ((unsigned)((o) >> 4))

typedef struct {
  bool set;                  /* the items key was present (an empty value counts) */
  uint8_t count;
  uint8_t block[ANN_LIST_MAX];
  uint16_t ble_mute;         /* bit i: block i is not spoken while a phone is connected */
  uint8_t opt[ANN_LIST_MAX]; /* ANN_OPT of block i */
} announce_list_t;

/* The span in seconds the block covers in this list: its option, or def_s without one (0: not in the list). */
uint8_t announce_list_span_s(const announce_list_t *l, announce_block_t b, uint8_t def_s);

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
  uint16_t min_gap_s;             /* announce_mingap_int: automatic announcements at least this far apart, measured
                                     from the end of the last one (a double tap's too); 0 = no limit */
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

/* What the device knows now. The arrays go by announce_span_s (index 0: the block's default span). */
typedef struct {
  bool in_flight;
  float alt_m;                /* GNSS-referenced barometric altitude */
  float climb_ms[ANN_SPANS];  /* mean climb rate over each span ([0]: 10 s) */
  float gain_m[ANN_SPANS];    /* height gained over each span ([0] unused: the climb phase is thermal_gain_m) */
  bool fix;                   /* 3D fix */
  float speed_kmh;
  float course_deg;
  uint32_t airtime_s;
  bool time_valid;
  uint32_t local_s;           /* local time, seconds since midnight */
  int battery_percent;        /* < 0: unknown */
  bool wind_valid;
  float wind_kmh;
  float wind_from_deg;        /* direction the wind comes from */
  bool glide_valid[ANN_SPANS]; /* glide ratio over each span of straight flight ([0]: 20 s) */
  float glide_ratio[ANN_SPANS];
  bool thermal_valid;         /* circling: the climb phase so far, or the one that just ended */
  float thermal_gain_m;
  float g_load;               /* peak load factor of the manoeuvre or the flight; <= 0: none */
  float bank_deg;
  bool stats_valid;           /* flight summary */
  float max_alt_m;
  float max_speed_kmh;
  float max_climb_ms;
  float max_sink_ms;          /* positive */
  bool glide_word, g_word, bank_word; /* extension clips glide_ratio, g_load, bank_angle in the voice pack */
  bool g_series;              /* extension clips g/g_0 ... g/g_100: the load factor as one clip per tenth */
  bool bank_series;           /* extension clips bank/bank_0 ... bank/bank_180: the bank angle in steps of 5 degrees */
  bool glide_series;          /* extension clips glide/glide_0 ... glide/glide_300: the glide ratio as one clip */
  bool wind_words;            /* extension clips wind and nav/from_<dir>: "wind, 12 km/h, from the west" */
} announce_input_t;

#define ANN_CLIPS  4
#define ANN_NAME   24
#define ANN_GROUPS ANN_LIST_MAX /* one group per block */

typedef struct {
  unsigned groups;
  unsigned count[ANN_GROUPS];            /* 0: the signal tone instead of clips */
  uint8_t signal[ANN_GROUPS];            /* ... which one (announce_signal_names) */
  char clip[ANN_GROUPS][ANN_CLIPS][ANN_NAME];
} announcement_t;

/* Plausible fixed values for a demo of a list (configurator play button); the clip flags stay as they are. */
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
/* Once per second in flight; ANN_DUE_* bits of the triggers that fire (they restart then). With hold (the
 * minimum gap after the last announcement is not over) nothing fires: a due trigger fires once it is. */
unsigned announce_auto_due(announce_auto_t *a, const announce_config_t *c, uint32_t now_s, float alt_m, bool fix,
                           int32_t lat_e7, int32_t lon_e7, bool hold);
/* A double tap told the pilot everything: the time/distance and the elevation triggers start over. */
void announce_auto_restart(announce_auto_t *a, uint32_t now_s, float alt_m);

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
