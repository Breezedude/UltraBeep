#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "announce.h"
#include "summary.h"
#include "vario.h"

/*
 * Configuration files on the SD card (docs/config-ext-format.md):
 *   config.ultrabip      stock format, profiles 1-3 + [global]; written by
 *                        BipLink and the stock configurator, read-only here.
 *   config_ext.ultrabip  same INI format: profiles 4-10 and every setting the
 *                        stock tools do not know.
 */
#define CONFIG_PATH            "0:/config.ultrabip"
#define CONFIG_NEW_PATH        "0:/config.new.ultrabip" /* written by BipLink, adopted at the next start */
/* Our own files live in a folder: the stock firmware deletes unknown files in the top folder. */
#define CONFIG_EXT_DIR         "0:/ultrabeep"
/* The owner's note for a finder: the configurator writes the master in the extension folder, the firmware
 * keeps a copy in the top folder (the stock firmware deletes unknown files there; this one comes back). */
#define IF_FOUND_MASTER_PATH   CONFIG_EXT_DIR "/if_found.txt"
#define IF_FOUND_COPY_PATH     "0:/IF_FOUND_PLEASE_READ.txt"
#define CONFIG_EXT_PATH        "0:/ultrabeep/config_ext.ultrabip"
#define PROFILE_MAX     10

/* led_userled_int (stock numbering): the LED once the GPS has a fix, see system.c update_user_led. */
typedef enum {
  LED_MODE_OFF = 0,
  LED_MODE_VARIO = 1,       /* green with the lift beeps, red with the sink tone (in flight) */
  LED_MODE_VARIO_MUTED = 2, /* ... only at the flight level mute */
  LED_MODE_POWER = 3,       /* battery: n blinks every 3 s; USB: breathing while charging, dim when full */
  LED_MODE_POWER_MUTED = 4, /* ... only at the flight level mute */
} led_mode_t;

typedef struct {
  bool present; /* section exists in one of the files */
  bool enabled;
  char type[16]; /* identity_profilename_string: paragliding, cross, hike, ... */
  char pilot[40];  /* identity_pilotname_string (IGC header) */
  char glider[40]; /* identity_wingname_string */
  char compid[12]; /* identity_compid_string */
  char custom_name[32]; /* identity_customname_string: own profile name, spoken by the clip below */
  char voice_clip[25];  /* identity_voiceclip_string: that name's clip in voices/custom ("profile_<name>", the
                           configurator names it after the profile); empty: the older profile_custom_<n> */
  uint8_t tap_sensitivity; /* system_tapsensitivity_int: 0 (extremely easy) .. 4 (difficult) */
  announce_config_t announce; /* announce_<trigger>_* and the matrix keys (announce.h) */
  int16_t time_offset_min; /* system_timeoffsetminutes_int: local time = UTC + offset */
  bool tz_auto;            /* system_timezoneauto_bool: plus European summer time */
  uint16_t auto_off_min;   /* system_autopowerofftime_int: off after this long still on the ground (0 = never) */
  uint8_t led_mode;        /* led_userled_int: led_mode_t */
  bool takeoff_detect;     /* gps_takeoffdetection_bool; false: flight mode from the first fix on, no landing detection */
  bool bt_remote;          /* bluetooth_remoteenable_bool: a double tap is a key press on the phone (remote.h) */
  uint16_t alarm_min_m;    /* alarm_min2_int: altitude alert below this (0 = off) */
  uint16_t alarm_max_m;    /* alarm_max2_int: altitude alert above this (0 = off) */
  vario_tones_t tones; /* vario_tones_*, vario_liftstart_int, vario_sinkstart_int, ... */
} profile_t;

/* Volume levels; in flight a short press cycles LOW -> MED -> HIGH -> MUTE -> LOW. */
typedef enum { LEVEL_LOW, LEVEL_MED, LEVEL_HIGH, LEVEL_GROUND, LEVEL_COUNT, LEVEL_MUTE = LEVEL_COUNT } level_t;

typedef struct {
  profile_t profile[PROFILE_MAX]; /* index 0 = profile 1 */
  uint8_t profile_last;           /* 1..PROFILE_MAX, preselected at power on */
  char language[8];               /* voice pack, from profile 1 */
  /* Perceived loudness in percent (100 % sounds twice as loud as 50 %), per level. */
  uint8_t volume_buzzer[LEVEL_COUNT];  /* piezo: vario tones, melodies */
  uint8_t volume_speaker[LEVEL_COUNT]; /* speaker: announcements */
  level_t flight_level;                /* last in-flight level, used at the next takeoff */
  bool rawlog;                         /* system_rawlog_bool: raw sensor log while switched on */
  bool uart_console;                   /* system_uartconsole_bool: the console on PC0/PC1 too (uart_console.h) */
  bool battery_say;                    /* system_batteryannounce_bool: the battery level after the profile selection */
  bool battery_say_low_only;           /* system_batteryannounceonlylow_bool: ... only below 60 % */
  uint8_t power_off_melody;            /* system_poweroffmelody_string: index into sound_power_off_names */
  uint16_t sync_window_s;              /* system_syncwindow_int: Bluetooth after switching off a flight (0 = no) */
  bool sync_while_moving;              /* system_syncwhilemoving_bool: that time counts once the device lies still */
  bool bt_announce;                    /* bluetooth_announce_bool: "Bluetooth connected" / "disconnected" at the link events */
  bool bt_pair_announce;               /* bluetooth_pairannounce_bool: a pairing request says "Bluetooth, press the button
                                          to confirm" after the prompt tones; off: the tones only */
  dst_rule_t dst_rule;                 /* system_dstrule_string: the time zone's summer time rule */
  tz_table_t tz_table;                 /* global_timechanges_longstring of the stock file (announce.h) */
  bool landing_detect;                 /* flight_landingdetect_bool */
  bool record_igc;                     /* flight_recordigc_bool: IGC file in tracks_IGC/<year> */
  bool record_kml;                     /* flight_recordkml_bool: KML file in tracks_KML/<year> */
  uint8_t record_hz;                   /* flight_recordrate_int: IGC data records per second (1..5) */
  uint16_t landing_time_s;             /* flight_landingtime_int: calm time before a landing counts */
  bool landing_impact;                 /* flight_landingimpact_bool: landing after a touchdown shock */
  uint8_t landing_impact_g;            /* flight_landingimpactg_int: the shock's peak in g */
  uint8_t landing_slow_kmh;            /* flight_landingspeed_int: then slower than this ... */
  uint8_t landing_impact_s;            /* flight_landingimpacttime_int: ... for this long, no constant sink */
  bool landing_handling;               /* flight_landinghandling_bool: landing from ground handling */
  uint8_t takeoff_speed_kmh;           /* flight_takeoffspeed_int: takeoff indicator A, ground speed ... */
  uint8_t takeoff_speed_s;             /* flight_takeoffspeedtime_int: ... held this long */
  uint16_t takeoff_distance_m;         /* flight_takeoffdistance_int: indicator B within 30 s */
  uint16_t takeoff_altitude_m;         /* flight_takeoffaltitude_int: indicator C within 30 s ... */
  uint16_t takeoff_climb_cms;          /* flight_takeoffclimb_int: ... or this climb or sink rate ... */
  uint8_t takeoff_climb_s;             /* flight_takeoffclimbtime_int: ... held this long */
  uint8_t baro_rate;                   /* system_barorate_int: barometer samples per second (32, 64, 100) */
  uint8_t gps_rate;                    /* system_gpsrate_int: 1 or 5 fixes per second (10 is read as 5) */
  uint8_t gps_systems;                 /* system_gpssystems_string: gps_systems_t (all, gps+glonass, gps) */
  float accel_offset[3];               /* imu_accoffset_x/y/z_int [mm/s²]: a = (raw - offset) * scale */
  float accel_scale[3];                /* imu_accscale_x/y/z_int [ppm of 1] */
  bool accel_cal_from_file;            /* an imu_acc* key was read; otherwise the factory OTP calibration is used */
  /* [global] of the stock file, reported in the BLE Device Information service (biplink.c) */
  char hw_version[20];                 /* system_hardwareversion_string, e.g. UltraBip_v1.0+0 */
  char stock_fw_version[24];           /* system_firmwareversion_string, e.g. 2026-06-23:48048a99 */
  char ble_name[40];                   /* system_blename_string: own text after "UltraBip🪂" (identity.h) */
  char app_secrets[80];                /* biplink_secrets_string (extension file): the 8-character secrets of the
                                          BipLink instances the pilot confirmed, comma-separated */
} config_t;

void config_defaults(config_t *c);
/* Reads both files (the SD card must be mounted); missing files keep defaults. */
bool config_load(config_t *c);
/* Takes a config.new.ultrabip from BipLink over as config.ultrabip; true when there was one. */
bool config_adopt_new(void);
/* Stores the selected profile in config_ext.ultrabip (other content kept). */
bool config_save_profile_last(uint8_t profile);
/* Stores the in-flight level for the next flight. */
bool config_save_flight_level(level_t level);
/* Stores the accelerometer calibration (from the configurator's calibration). */
bool config_save_accel_calibration(const float offset[3], const float scale[3]);
/* Stores the list of confirmed BipLink secrets (biplink.c). */
bool config_save_app_secrets(const char *list);
/* Copies ultrabeep/if_found.txt to IF_FOUND_PLEASE_READ.txt in the top folder when it has text, removes the
 * copy when it has none. System task only (the config file buffer); other tasks ask for it. */
void config_mirror_if_found(void);
void config_request_if_found_mirror(void);
bool config_take_if_found_request(void);
const char *config_level_name(level_t level);

/* The configuration loaded at power on. */
config_t *config_get(void);

#endif
