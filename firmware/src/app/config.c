#include "config.h"

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gps.h"
#include "ini.h"
#include "log.h"
#include "sound.h"
#include "storage.h"
#include "tap.h"

#define FILE_BUF_SIZE (16 * 1024)

static config_t active;
static char file_buf[FILE_BUF_SIZE]; /* used by one task at a time (system task) */

static const char ext_header[] =
  "# UltraBeep: settings the stock tools do not know (profiles 4-10, volumes, ...).\r\n"
  "# Format: docs/config-ext-format.md. BipLink and the stock configurator ignore this file.\r\n";

config_t *config_get(void)
{
  return &active;
}

void config_defaults(config_t *c)
{
  memset(c, 0, sizeof(*c));
  for (int i = 0; i < PROFILE_MAX; i++) {
    vario_tones_defaults(&c->profile[i].tones);
    c->profile[i].tap_sensitivity = TAP_SENSITIVITY_DEFAULT;
    c->profile[i].auto_off_min = 30;
    c->profile[i].takeoff_detect = true;
    announce_config_defaults(&c->profile[i].announce);
  }
  c->profile[0].present = true;
  c->profile[0].enabled = true;
  strcpy(c->profile[0].type, "paragliding");
  c->profile_last = 1;
  strcpy(c->language, "de");
  static const uint8_t buzzer[LEVEL_COUNT] = {40, 60, 100, 20}; /* piezo steps: quiet, medium, very loud, very quiet */
  static const uint8_t speaker[LEVEL_COUNT] = {40, 70, 100, 50};
  memcpy(c->volume_buzzer, buzzer, sizeof(buzzer));
  memcpy(c->volume_speaker, speaker, sizeof(speaker));
  c->flight_level = LEVEL_MED;
  c->rawlog = false; /* a configuration switches it on (the configurator's dev view) */
  c->battery_say = true;
  c->battery_say_low_only = true;
  c->sync_window_s = 60;
  c->sync_while_moving = true;
  c->dst_rule = DST_EU;
  c->landing_detect = true;
  c->record_igc = true;
  c->record_kml = true;
  c->record_hz = 4;
  c->baro_rate = 100;
  c->gps_rate = 5;
  c->gps_systems = GPS_SYSTEMS_ALL;
  for (int k = 0; k < 3; k++) {
    c->accel_offset[k] = 0.0f;
    c->accel_scale[k] = 1.0f;
  }
  c->landing_time_s = 60;
  c->landing_impact = true;
  c->bt_pair_announce = true;
  c->landing_impact_g = 4;
  c->landing_slow_kmh = 10;
  c->landing_impact_s = 10;
  c->landing_handling = true;
  c->takeoff_speed_kmh = 15;
  c->takeoff_speed_s = 5;
  c->takeoff_distance_m = 50;
  c->takeoff_altitude_m = 15;
  c->takeoff_climb_cms = 150;
  c->takeoff_climb_s = 5;
}

static const char *const level_names[] = {"low", "med", "high", "ground", "mute"};

const char *config_level_name(level_t level)
{
  return level <= LEVEL_MUTE ? level_names[level] : "?";
}

/* "volume_buzzerlow_int" -> LEVEL_LOW; -1 if the suffix is no level name */
static int level_from_key(const char *rest)
{
  for (int i = 0; i < LEVEL_COUNT; i++) {
    size_t n = strlen(level_names[i]);
    if (strncmp(rest, level_names[i], n) == 0 && strcmp(rest + n, "_int") == 0) {
      return i;
    }
  }
  return -1;
}

static int profile_index(const char *section)
{
  if (strncmp(section, "profile", 7) != 0) {
    return -1;
  }
  int n = atoi(section + 7);
  return (n >= 1 && n <= PROFILE_MAX) ? n - 1 : -1;
}

static uint8_t percent(const char *value)
{
  int v = atoi(value);
  return (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
}

typedef struct {
  config_t *c;
  bool ext;
} parse_ctx_t;

/* vario_tones_<field>_<n> and the vario thresholds; false if the key is none of them. */
static bool vario_key(vario_tones_t *t, const char *key, const char *value)
{
  int v = atoi(value);
  if (strcmp(key, "vario_liftstart_int") == 0) {
    t->lift_start_cms = (int16_t)v;
  } else if (strcmp(key, "vario_sinkstart_int") == 0) {
    t->sink_start_cms = (int16_t)v;
  } else if (strcmp(key, "vario_sinkalarm_bool") == 0) {
    t->sink_alarm = ini_parse_bool(value);
  } else if (strcmp(key, "vario_sweeptone_bool") == 0) {
    t->sweep = ini_parse_bool(value);
  } else if (strcmp(key, "vario_liftysound_bool") == 0) {
    t->lifty = ini_parse_bool(value);
  } else if (strcmp(key, "vario_integrationtime_int") == 0) {
    t->integration_pct = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
  } else if (strncmp(key, "vario_tones_", 12) == 0) {
    const char *field = key + 12;
    const char *us = strrchr(field, '_');
    int n = us ? atoi(us + 1) : -1;
    if (n < 0 || n >= VARIO_TONE_POINTS) {
      return false;
    }
    vario_tone_point_t *p = &t->point[n];
    size_t len = (size_t)(us - field);
    if (len == 2 && strncmp(field, "vz", 2) == 0) {
      p->vz_cms = (int16_t)v;
    } else if (len == 4 && strncmp(field, "freq", 4) == 0) {
      p->freq_hz = (uint16_t)(v < 0 ? 0 : v > 5000 ? 5000 : v);
    } else if (len == 8 && strncmp(field, "duration", 8) == 0) {
      p->period_ms = (uint16_t)(v < 20 ? 20 : v > 5000 ? 5000 : v);
    } else if (len == 4 && strncmp(field, "duty", 4) == 0) {
      p->duty = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
    } else {
      return false;
    }
  } else {
    return false;
  }
  return true;
}

/* Profile keys the stock tools do not know: in config_ext.ultrabip also for profiles 1-3. */
static bool ext_profile_key(const char *key)
{
  return strncmp(key, "announce_", 9) == 0 || strncmp(key, "identity_customname", 19) == 0 ||
         strncmp(key, "identity_voicetext", 18) == 0 || strncmp(key, "identity_voiceclip", 18) == 0;
}

/* The stock key "<prefix><item>_bool" -> the item's flag in `flags`. */
static bool matrix_key(const char *key, const char *prefix, bool flags[MATRIX_ITEMS], const char *value)
{
  size_t n = strlen(prefix);
  if (strncmp(key, prefix, n) != 0) {
    return false;
  }
  for (int i = 0; i < MATRIX_ITEMS; i++) {
    size_t m = strlen(announce_matrix_names[i]);
    if (strncmp(key + n, announce_matrix_names[i], m) == 0 && strcmp(key + n + m, "_bool") == 0) {
      flags[i] = ini_parse_bool(value);
      return true;
    }
  }
  return false;
}

static uint16_t clamp_u16(int v, int lo, int hi)
{
  return (uint16_t)(v < lo ? lo : v > hi ? hi : v);
}

static bool announce_key(announce_config_t *a, const char *key, const char *value)
{
  int v = atoi(value);
  if (strcmp(key, "voiceauto_enable_bool") == 0) {
    a->enable[TRIG_AUTO] = ini_parse_bool(value);
    a->enable_set[TRIG_AUTO] = true;
  } else if (strcmp(key, "voiceauto_altitudetrigger_int") == 0) {
    a->alt_trigger_m = clamp_u16(v, 0, 5000);
  } else if (strcmp(key, "voiceauto_distancetrigger_int") == 0) {
    a->dist_trigger_m = (uint32_t)(v < 0 ? 0 : v > 100000 ? 100000 : v);
  } else if (strcmp(key, "voiceauto_timetrigger_int") == 0) {
    a->time_trigger_min = clamp_u16(v, 0, 240);
  } else if (strcmp(key, "voicetap_enable_bool") == 0) {
    a->enable[TRIG_TAP] = ini_parse_bool(value);
    a->enable_set[TRIG_TAP] = true;
  } else if (strcmp(key, "voicetap_enablebeforetakeoff_bool") == 0) {
    a->tap_before_takeoff = ini_parse_bool(value);
  } else if (strcmp(key, "voicetap_enablewhensilent_bool") == 0) {
    a->tap_when_silent = ini_parse_bool(value);
  } else if (strncmp(key, "flightsummary_", 14) == 0) {
    static const char *const items[4] = {"duration_bool", "altitude_bool", "speed_bool", "climbrate_bool"};
    const char *item = key + 14;
    if (strcmp(item, "enable_bool") == 0) {
      a->summary_enable = ini_parse_bool(value);
      return true;
    }
    for (int i = 0; i < 4; i++) {
      if (strcmp(item, items[i]) == 0) {
        a->summary_item[i] = ini_parse_bool(value);
        return true;
      }
    }
    return false;
  } else if (strncmp(key, "announce_", 9) == 0) {
    const char *rest = key + 9;
    for (int t = 0; t < ANN_TRIGGERS; t++) {
      size_t n = strlen(announce_trigger_names[t]);
      if (strncmp(rest, announce_trigger_names[t], n) == 0 && rest[n] == '_') {
        if (strcmp(rest + n + 1, "items_string") == 0) {
          announce_list_parse(&a->list[t], value);
          return true;
        }
        if (strcmp(rest + n + 1, "enable_bool") == 0) {
          a->enable[t] = ini_parse_bool(value);
          a->enable_set[t] = true;
          return true;
        }
      }
    }
    if (strcmp(rest, "elevation_gain_int") == 0) {
      a->elevation_gain_m = clamp_u16(v, 0, 5000);
    } else if (strcmp(rest, "elevation_loss_int") == 0) {
      a->elevation_loss_m = clamp_u16(v, 0, 5000);
    } else if (strcmp(rest, "thermalexit_mintime_int") == 0) {
      a->thermalexit_min_s = clamp_u16(v, 10, 600);
    } else if (strcmp(rest, "thermalexit_mingain_int") == 0) {
      a->thermalexit_min_gain_m = clamp_u16(v, 0, 2000);
    } else if (strcmp(rest, "thermalclimb_int") == 0) {
      a->climb_report_s = clamp_u16(v, 5, 300);
    } else if (strcmp(rest, "glide_straighttime_int") == 0) {
      a->glide_straight_s = clamp_u16(v, 10, 600);
    } else if (strcmp(rest, "glidemingain_int") == 0) {
      a->glide_min_gain_m = clamp_u16(v, 0, 2000);
    } else if (strcmp(rest, "glide_repeat_int") == 0) {
      a->glide_repeat_min = clamp_u16(v, 0, 60);
    } else {
      return false;
    }
  } else {
    return matrix_key(key, "voiceauto_", a->auto_item, value) || matrix_key(key, "voicetap_", a->tap_item, value);
  }
  return true;
}

static bool on_key(void *ctx, const char *section, const char *key, const char *value)
{
  parse_ctx_t *pc = (parse_ctx_t *)ctx;
  config_t *c = pc->c;
  int pi = profile_index(section);

  if (pi >= 0) {
    /* Profiles 1-3 belong to the stock file; the extension file holds 4-10 and, for
     * profiles 1-3, only the keys the stock tools do not know. */
    if (pc->ext != (pi >= 3) && !(pc->ext && ext_profile_key(key))) {
      return true;
    }
    profile_t *p = &c->profile[pi];
    p->present = true;
    if (strcmp(key, "enabled_bool") == 0) {
      p->enabled = ini_parse_bool(value);
    } else if (strcmp(key, "identity_profilename_string") == 0) {
      snprintf(p->type, sizeof(p->type), "%s", value);
    } else if (strcmp(key, "identity_pilotname_string") == 0) {
      snprintf(p->pilot, sizeof(p->pilot), "%s", value);
    } else if (strcmp(key, "identity_wingname_string") == 0) {
      snprintf(p->glider, sizeof(p->glider), "%s", value);
    } else if (strcmp(key, "identity_compid_string") == 0) {
      snprintf(p->compid, sizeof(p->compid), "%s", value);
    } else if (strcmp(key, "system_tapsensitivity_int") == 0) {
      int v = atoi(value);
      p->tap_sensitivity = (uint8_t)(v < TAP_SENSITIVITY_MIN ? TAP_SENSITIVITY_MIN
                                     : v > TAP_SENSITIVITY_MAX ? TAP_SENSITIVITY_MAX : v);
    } else if (strcmp(key, "system_timeoffsetminutes_int") == 0) {
      int v = atoi(value);
      p->time_offset_min = (int16_t)(v < -720 ? -720 : v > 840 ? 840 : v);
    } else if (strcmp(key, "system_timezoneauto_bool") == 0) {
      p->tz_auto = ini_parse_bool(value);
    } else if (strcmp(key, "system_autopowerofftime_int") == 0) {
      int v = atoi(value);
      p->auto_off_min = (uint16_t)(v < 0 ? 0 : v > 600 ? 600 : v);
    } else if (strcmp(key, "led_userled_int") == 0) {
      int v = atoi(value);
      p->led_mode = (uint8_t)(v < 0 || v > LED_MODE_POWER_MUTED ? LED_MODE_OFF : v);
    } else if (strcmp(key, "gps_takeoffdetection_bool") == 0) {
      p->takeoff_detect = ini_parse_bool(value);
    } else if (strcmp(key, "bluetooth_remoteenable_bool") == 0) {
      p->bt_remote = ini_parse_bool(value);
    } else if (strcmp(key, "alarm_min2_int") == 0) {
      int v = atoi(value);
      p->alarm_min_m = (uint16_t)(v < 0 ? 0 : v > 9000 ? 9000 : v);
    } else if (strcmp(key, "alarm_max2_int") == 0) {
      int v = atoi(value);
      p->alarm_max_m = (uint16_t)(v < 0 ? 0 : v > 9000 ? 9000 : v);
    } else if (strcmp(key, "identity_customname_string") == 0) {
      snprintf(p->custom_name, sizeof(p->custom_name), "%s", value);
    } else if (strcmp(key, "identity_voiceclip_string") == 0) {
      /* a plain clip name as the file service takes it: [a-z0-9_], up to 24 characters */
      size_t len = strlen(value);
      bool plain = len > 0 && len < sizeof(p->voice_clip);
      for (size_t i = 0; plain && i < len; i++) {
        char ch = value[i];
        plain = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
      }
      snprintf(p->voice_clip, sizeof(p->voice_clip), "%s", plain ? value : "");
    } else if (announce_key(&p->announce, key, value)) {
      /* stored */
    } else if (vario_key(&p->tones, key, value)) {
      /* stored */
    } else if (pi == 0 && strcmp(key, "system_language_string") == 0 && value[0]) {
      snprintf(c->language, sizeof(c->language), "%s", value);
    }
    return true;
  }

  if (!pc->ext && strcmp(section, "global") == 0) {
    if (strcmp(key, "system_hardwareversion_string") == 0) {
      snprintf(c->hw_version, sizeof(c->hw_version), "%s", value);
    } else if (strcmp(key, "system_firmwareversion_string") == 0) {
      snprintf(c->stock_fw_version, sizeof(c->stock_fw_version), "%s", value);
    } else if (strcmp(key, "global_timechanges_longstring") == 0) {
      announce_tz_table_parse(value, &c->tz_table);
    }
    return true;
  }
  if (pc->ext && strcmp(section, "global") == 0) {
    if (strcmp(key, "system_profilelast_int") == 0) {
      int n = atoi(value);
      if (n >= 1 && n <= PROFILE_MAX) {
        c->profile_last = (uint8_t)n;
      }
    } else if (strncmp(key, "volume_buzzer", 13) == 0 && level_from_key(key + 13) >= 0) {
      c->volume_buzzer[level_from_key(key + 13)] = percent(value);
    } else if (strncmp(key, "volume_speaker", 14) == 0 && level_from_key(key + 14) >= 0) {
      c->volume_speaker[level_from_key(key + 14)] = percent(value);
    } else if (strcmp(key, "system_blename_string") == 0) {
      snprintf(c->ble_name, sizeof(c->ble_name), "%s", value);
    } else if (strcmp(key, "biplink_secrets_string") == 0) {
      snprintf(c->app_secrets, sizeof(c->app_secrets), "%s", value);
    } else if (strcmp(key, "system_rawlog_bool") == 0) {
      c->rawlog = ini_parse_bool(value);
    } else if (strcmp(key, "system_uartconsole_bool") == 0) {
      c->uart_console = ini_parse_bool(value);
    } else if (strcmp(key, "system_batteryannounce_bool") == 0) {
      c->battery_say = ini_parse_bool(value);
    } else if (strcmp(key, "system_batteryannounceonlylow_bool") == 0) {
      c->battery_say_low_only = ini_parse_bool(value);
    } else if (strcmp(key, "system_syncwindow_int") == 0) {
      int v = atoi(value);
      c->sync_window_s = (uint16_t)(v < 0 ? 0 : v > 600 ? 600 : v);
    } else if (strcmp(key, "system_syncwhilemoving_bool") == 0) {
      c->sync_while_moving = ini_parse_bool(value);
    } else if (strcmp(key, "bluetooth_announce_bool") == 0) {
      c->bt_announce = ini_parse_bool(value);
    } else if (strcmp(key, "bluetooth_pairannounce_bool") == 0) {
      c->bt_pair_announce = ini_parse_bool(value);
    } else if (strcmp(key, "system_poweroffmelody_string") == 0) {
      for (unsigned i = 0; i < SOUND_POWER_OFF_COUNT; i++) {
        if (strcmp(value, sound_power_off_names[i]) == 0) {
          c->power_off_melody = (uint8_t)i;
        }
      }
    } else if (strcmp(key, "system_dstrule_string") == 0) {
      c->dst_rule = announce_dst_rule_parse(value);
    } else if (strcmp(key, "flight_recordigc_bool") == 0) {
      c->record_igc = ini_parse_bool(value);
    } else if (strcmp(key, "flight_recordkml_bool") == 0) {
      c->record_kml = ini_parse_bool(value);
    } else if (strcmp(key, "system_barorate_int") == 0) {
      int v = atoi(value);
      c->baro_rate = (uint8_t)(v == 32 || v == 64 ? v : 100);
    } else if (strcmp(key, "system_gpsrate_int") == 0) {
      int v = atoi(value);
      c->gps_rate = (uint8_t)(v == 1 ? 1 : 5); /* 10 is gone: the module computed only 5 fixes per second there */
    } else if (strcmp(key, "system_gpssystems_string") == 0) {
      c->gps_systems = strcmp(value, "gps+glonass") == 0 ? GPS_SYSTEMS_GPS_GLONASS
                       : strcmp(value, "gps") == 0 ? GPS_SYSTEMS_GPS : GPS_SYSTEMS_ALL;
    } else if (strncmp(key, "imu_accoffset_", 14) == 0 && key[14] >= 'x' && key[14] <= 'z' &&
               strcmp(key + 15, "_int") == 0) {
      int v = atoi(value); /* mm/s² */
      c->accel_offset[key[14] - 'x'] = (float)(v < -3000 ? -3000 : v > 3000 ? 3000 : v) / 1000.0f;
      c->accel_cal_from_file = true;
    } else if (strncmp(key, "imu_accscale_", 13) == 0 && key[13] >= 'x' && key[13] <= 'z' &&
               strcmp(key + 14, "_int") == 0) {
      long v = atol(value); /* ppm of 1 */
      c->accel_scale[key[13] - 'x'] = (float)(v < 800000L ? 800000L : v > 1200000L ? 1200000L : v) / 1e6f;
      c->accel_cal_from_file = true;
    } else if (strcmp(key, "flight_recordrate_int") == 0) {
      int v = atoi(value);
      c->record_hz = (uint8_t)(v < 1 ? 1 : v > 5 ? 5 : v); /* at most one per GNSS fix */
    } else if (strcmp(key, "flight_landingdetect_bool") == 0) {
      c->landing_detect = ini_parse_bool(value);
    } else if (strcmp(key, "flight_landingtime_int") == 0) {
      int v = atoi(value);
      c->landing_time_s = (uint16_t)(v < 10 ? 10 : v > 600 ? 600 : v);
    } else if (strcmp(key, "flight_landingimpact_bool") == 0) {
      c->landing_impact = ini_parse_bool(value);
    } else if (strcmp(key, "flight_landingimpactg_int") == 0) {
      int v = atoi(value);
      c->landing_impact_g = (uint8_t)(v < 3 ? 3 : v > 15 ? 15 : v);
    } else if (strcmp(key, "flight_landingspeed_int") == 0) {
      int v = atoi(value);
      c->landing_slow_kmh = (uint8_t)(v < 3 ? 3 : v > 30 ? 30 : v);
    } else if (strcmp(key, "flight_landingimpacttime_int") == 0) {
      int v = atoi(value);
      c->landing_impact_s = (uint8_t)(v < 3 ? 3 : v > 60 ? 60 : v);
    } else if (strcmp(key, "flight_landinghandling_bool") == 0) {
      c->landing_handling = ini_parse_bool(value);
    } else if (strcmp(key, "flight_takeoffspeed_int") == 0) {
      int v = atoi(value);
      c->takeoff_speed_kmh = (uint8_t)(v < 5 ? 5 : v > 50 ? 50 : v);
    } else if (strcmp(key, "flight_takeoffspeedtime_int") == 0) {
      int v = atoi(value);
      c->takeoff_speed_s = (uint8_t)(v < 1 ? 1 : v > 30 ? 30 : v);
    } else if (strcmp(key, "flight_takeoffdistance_int") == 0) {
      int v = atoi(value);
      c->takeoff_distance_m = (uint16_t)(v < 10 ? 10 : v > 500 ? 500 : v);
    } else if (strcmp(key, "flight_takeoffaltitude_int") == 0) {
      int v = atoi(value);
      c->takeoff_altitude_m = (uint16_t)(v < 5 ? 5 : v > 100 ? 100 : v);
    } else if (strcmp(key, "flight_takeoffclimb_int") == 0) {
      int v = atoi(value);
      c->takeoff_climb_cms = (uint16_t)(v < 50 ? 50 : v > 500 ? 500 : v);
    } else if (strcmp(key, "flight_takeoffclimbtime_int") == 0) {
      int v = atoi(value);
      c->takeoff_climb_s = (uint8_t)(v < 1 ? 1 : v > 30 ? 30 : v);
    } else if (strcmp(key, "volume_flightlevel_string") == 0) {
      for (int i = 0; i <= LEVEL_MUTE; i++) {
        if (i != LEVEL_GROUND && strcmp(value, level_names[i]) == 0) {
          c->flight_level = (level_t)i;
        }
      }
    }
  }
  return true;
}

/*
 * The accelerometer calibration the factory stored in the OTP area (observed layout, the stock
 * firmware's "IMU Calibration" from 18 points): three offsets [m/s², to be added], three gains,
 * the temperature [°C], a marker word (bytes: accel config 0x47, gyro config 0x27, 18 points,
 * version 2), then the 18 raw points; the rest of the 1 KB area is empty. Used while no imu_acc*
 * key is set.
 */
#define OTP_CALIBRATION 0x1FFF7000UL

static bool factory_calibration(config_t *c)
{
  const float *otp = (const float *)OTP_CALIBRATION;
  for (int k = 0; k < 3; k++) {
    if (!(fabsf(otp[k]) < 2.0f) || !(otp[3 + k] > 0.9f && otp[3 + k] < 1.1f)) {
      return false;
    }
  }
  if (!(otp[6] > -40.0f && otp[6] < 85.0f)) {
    return false;
  }
  for (int k = 0; k < 3; k++) {
    c->accel_offset[k] = -otp[k]; /* ours: a = (raw - offset) * scale */
    c->accel_scale[k] = otp[3 + k];
  }
  log_printf("Accelerometer calibration from the factory OTP: offset %.3f %.3f %.3f m/s2, scale %.5f %.5f %.5f (taken at %.1f C)",
             (double)c->accel_offset[0], (double)c->accel_offset[1], (double)c->accel_offset[2], (double)c->accel_scale[0],
             (double)c->accel_scale[1], (double)c->accel_scale[2], (double)otp[6]);
  return true;
}

/* Writes our extension file; its folder is created when missing. */
static bool ext_write(const char *text, size_t len)
{
  storage_mkdir(CONFIG_EXT_DIR);
  return storage_write_text(CONFIG_EXT_PATH, text, len);
}

bool config_adopt_new(void)
{
  /* BipLink uploads a new stock file under this name; the firmware takes it over, as the stock does */
  if (storage_read_text(CONFIG_NEW_PATH, file_buf, sizeof(file_buf)) < 0) {
    return false;
  }
  storage_remove(CONFIG_PATH);
  bool ok = storage_rename(CONFIG_NEW_PATH, CONFIG_PATH);
  log_printf("Config: %s adopted as %s - %s", CONFIG_NEW_PATH, CONFIG_PATH, ok ? "ok" : "rename FAILED");
  return ok;
}

static volatile bool if_found_requested;

void config_request_if_found_mirror(void)
{
  if_found_requested = true;
}

bool config_take_if_found_request(void)
{
  bool v = if_found_requested;
  if_found_requested = false;
  return v;
}

void config_mirror_if_found(void)
{
  /* the note in the first half of the file buffer, the copy in the second (the configurator limits the note
   * to 2000 characters; the buffer is free between the config operations of the system task) */
  char *note = file_buf, *copy = file_buf + FILE_BUF_SIZE / 2;
  int n = storage_read_text(IF_FOUND_MASTER_PATH, note, FILE_BUF_SIZE / 2);
  bool text = false;
  for (int i = 0; i < n && !text; i++) {
    text = note[i] != ' ' && note[i] != '\t' && note[i] != '\r' && note[i] != '\n';
  }
  int m = storage_read_text(IF_FOUND_COPY_PATH, copy, FILE_BUF_SIZE / 2);
  if (!text) {
    if (m >= 0) {
      storage_remove(IF_FOUND_COPY_PATH);
      log_printf("If-found note: no text, %s removed", IF_FOUND_COPY_PATH);
    }
    return;
  }
  if (m == n && memcmp(note, copy, (size_t)n) == 0) {
    return; /* the copy is current */
  }
  bool ok = storage_write_text(IF_FOUND_COPY_PATH, note, (size_t)n);
  log_printf("If-found note: %s %s", IF_FOUND_COPY_PATH, ok ? "written" : "NOT written");
}

bool config_load(config_t *c)
{
  config_defaults(c);
  c->profile[0].present = false; /* only defaults if no file says otherwise */

  parse_ctx_t pc = {c, false};
  config_adopt_new();
  int n = storage_read_text(CONFIG_PATH, file_buf, sizeof(file_buf));
  if (n >= 0) {
    ini_parse(file_buf, on_key, &pc);
  }
  log_printf("Config %s: %s", CONFIG_PATH, n >= 0 ? "read" : "missing - defaults");

  pc.ext = true;
  int m = storage_read_text(CONFIG_EXT_PATH, file_buf, sizeof(file_buf));
  if (m >= 0) {
    ini_parse(file_buf, on_key, &pc);
  }
  log_printf("Config %s: %s", CONFIG_EXT_PATH, m >= 0 ? "read" : "missing - defaults");
  if (!c->accel_cal_from_file && !factory_calibration(c)) {
    log_printf("Accelerometer: no calibration (no imu_acc* keys, no factory OTP data)");
  }

  bool any = false;
  for (int i = 0; i < PROFILE_MAX; i++) {
    any = any || (c->profile[i].present && c->profile[i].enabled);
  }
  if (!any) {
    c->profile[0].present = true;
    c->profile[0].enabled = true;
    if (!c->profile[0].type[0]) {
      strcpy(c->profile[0].type, "paragliding");
    }
  }
  for (int i = 0; i < PROFILE_MAX; i++) {
    const profile_t *p = &c->profile[i];
    announce_config_resolve(&c->profile[i].announce); /* lists without a key: from the stock keys */
    if (p->present) {
      log_printf("  profile %d: %s, type '%s'%s%s", i + 1, p->enabled ? "enabled" : "disabled", p->type,
                 p->custom_name[0] ? " named " : "", p->custom_name);
    }
  }
  return n >= 0;
}

/* Sets one [global] value in config_ext.ultrabip, keeping everything else. */
static bool save_ext_global(const char *key, const char *value)
{
  int n = storage_read_text(CONFIG_EXT_PATH, file_buf, sizeof(file_buf));
  if (n < 0) {
    snprintf(file_buf, sizeof(file_buf), "%s", ext_header);
  }
  if (!ini_set(file_buf, sizeof(file_buf), "global", key, value)) {
    log_printf("config_ext.ultrabip too large");
    return false;
  }
  return ext_write(file_buf, strlen(file_buf));
}

bool config_save_accel_calibration(const float offset[3], const float scale[3])
{
  static const char axis[3] = {'x', 'y', 'z'};
  int n = storage_read_text(CONFIG_EXT_PATH, file_buf, sizeof(file_buf));
  if (n < 0) {
    snprintf(file_buf, sizeof(file_buf), "%s", ext_header);
  }
  for (int k = 0; k < 3; k++) {
    char key[24], value[16];
    snprintf(key, sizeof(key), "imu_accoffset_%c_int", axis[k]);
    snprintf(value, sizeof(value), "%ld", (long)(offset[k] * 1000.0f + (offset[k] >= 0.0f ? 0.5f : -0.5f)));
    if (!ini_set(file_buf, sizeof(file_buf), "global", key, value)) {
      return false;
    }
    snprintf(key, sizeof(key), "imu_accscale_%c_int", axis[k]);
    snprintf(value, sizeof(value), "%ld", (long)(scale[k] * 1e6f + 0.5f));
    if (!ini_set(file_buf, sizeof(file_buf), "global", key, value)) {
      return false;
    }
    active.accel_offset[k] = offset[k];
    active.accel_scale[k] = scale[k];
  }
  return ext_write(file_buf, strlen(file_buf));
}

bool config_save_profile_last(uint8_t profile)
{
  char value[8];
  snprintf(value, sizeof(value), "%u", profile);
  active.profile_last = profile;
  return save_ext_global("system_profilelast_int", value);
}

bool config_save_app_secrets(const char *list)
{
  snprintf(active.app_secrets, sizeof(active.app_secrets), "%s", list);
  return save_ext_global("biplink_secrets_string", list);
}

bool config_save_flight_level(level_t level)
{
  if (level == LEVEL_GROUND || level > LEVEL_MUTE) {
    return false;
  }
  active.flight_level = level;
  return save_ext_global("volume_flightlevel_string", level_names[level]);
}
