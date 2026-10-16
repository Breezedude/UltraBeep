/*
 * System task: the device state machine.
 *
 * Power on (device off = standby or USB mode): a short press wakes the device
 * (ack beep + LED; from standby done in main.c before the scheduler starts),
 * then a long press switches on (start melody). Every step has a 1 s limit:
 * wake-up press held longer, no second press within 1 s after the release or
 * the second press released before GESTURE_HOLD_MS - the device goes back to
 * where it started (standby or USB mode).
 * Power off: holding the button starts the power-off melody; held through 90 %
 * of it switches off at its end, released earlier cancels.
 * On the ground the device stays silent except for "GPS ok"; the LED blinks
 * yellow (good satellites) until the first 3D fix.
 */
#include "system.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "backup.h"
#include "biplink.h"
#include "analog.h"
#include "announce.h"
#include "battery.h"
#include "ble_app.h"
#include "board.h"
#include "button.h"
#include "config.h"
#include "flight.h"
#include "console.h"
#include "gesture.h"
#include "gps.h"
#include "handoff.h"
#include "health.h"
#include "indicator.h"
#include "log.h"
#include "maneuver.h"
#include "rawlog.h"
#include "recorder.h"
#include "rtc_time.h"
#include "sensors.h"
#include "sound.h"
#include "storage.h"
#include "summary.h"
#include "thermal.h"
#include "uart_console.h"
#include "vario_audio.h"
#include "version.h"

#define SHORT_PRESS_MAX_MS  600U  /* longer = power-off melody */
#define DOUBLE_PRESS_MS     700U  /* two short presses on the ground within this: manual takeoff */
#define POWER_OFF_RELEASE_PCT 85U /* the button may be released after this much of the melody */
#define CONFIRM_HOLD_MS     700U  /* profile selection: long press confirms */
#define PROFILE_IDLE_MS     3000U /* pause before the profile is repeated */
#define PROFILE_REPEATS     3     /* announcements before the automatic choice */
#define STABLE_RUN_MS       60000U
#define UPDATE_MIN_VBAT_MV  BATTERY_START_MV /* no flashing on a battery the device would not start on */
#define TICK_MS             50U
#define AUTO_OFF_RECHECK_MS 1000U

/* ST_ON: switched on after the profile selection, on the ground or in flight (in_flight). */
typedef enum { ST_OFF, ST_GESTURE, ST_PROFILE, ST_ON, ST_POWER_OFF, ST_SYNC, ST_USB } state_t;
typedef enum { EV_BUTTON, EV_OFF, EV_UPDATE, EV_TAP } event_type_t;

typedef struct {
  uint8_t type;
  button_event_t button;
} event_t;

static QueueHandle_t events;
static system_boot_t boot;
static volatile state_t state = ST_OFF;
static volatile bool usb_drive_wanted;
static uint8_t profile = 1;
static uint32_t running_since;
static bool resets_cleared;

static gesture_t gesture;
static bool gesture_from_usb;

static uint8_t enabled[PROFILE_MAX];
static uint8_t enabled_count;
static uint8_t sel;
static int announcements;
static bool idle_waiting;
static uint32_t idle_since;
static bool fresh_press_needed;
static bool confirmed;

static bool gps_ok_said;
static void set_headphones(bool on);

/* The serial console follows system_uartconsole_bool; switched on, it gets the log so far. */
static void apply_uart_console(void)
{
  bool want = config_get()->uart_console;
  if (want && !uart_console_enabled()) {
    uart_console_enable(true);
    console_printf("\r\n%s %s - serial console on\r\n", FW_NAME, FW_VERSION);
    log_replay_uart();
  } else if (!want && uart_console_enabled()) {
    uart_console_enable(false);
  }
}
static bool remote_said;  /* "Bluetooth remote control mode" said since power on */
static uint32_t last_short_up;   /* tick of the last short press on the ground (double press detection) */
static bool manual_takeoff;      /* a double press asked for the takeoff; taken at the next flight update */
static uint8_t faults_active;     /* check_faults: FAULT_* bits present now */
static uint8_t faults_said;       /* announced once since power on */
static uint8_t gps_silent_checks;
static uint32_t fault_tick;
static uint32_t still_since; /* auto power off: lying still on the ground since (0 = not) */
static uint32_t auto_off_tick;
static flight_detector_t flight;
static uint32_t flight_fix_seq;
static nmea_rate_t flight_second; /* the fixes that count: one per second */
static nmea_rate_t record_rate;   /* IGC data records (flight_recordrate_int) */
static bool in_flight; /* takeoff detected, no landing since */
static uint32_t flight_step_tick; /* last per-second step of the flight detection */
static flight_stats_t stats;
static maneuver_t maneuver;  /* wingover and spiral detection, peak load of the flight */
static uint32_t maneuver_tick;
/* Altitude alerts (alarm_min2_int, alarm_max2_int): once when the limit is crossed, then every 5 min. */
static bool alarm_above, alarm_below;
static uint32_t alarm_next_s;
#define ALARM_REPEAT_S     300U
#define ALARM_HYSTERESIS_M 10.0f
static battery_watch_t battery;
static uint32_t battery_tick;
static volatile uint32_t battery_mv; /* last reading, also with USB */
static bool battery_empty;           /* the power off below comes from an empty battery */
static battery_event_t read_battery(void);

/* Tone curve sent by the configurator for a test; double buffer, the sensor task reads it. */
static vario_tones_t test_tones[2];
static volatile int test_tones_idx = -1;

#define BATTERY_CHECK_MS 10000U
#define SYNC_MAX_MS      900000U /* Bluetooth after the power off: at most this long (connected, carried) */
static bool baro_referenced;
static volatile uint8_t vario_test; /* volume of the vario test on the ground, 0 = off */
static state_t before_off;
static uint32_t off_started;

/* ---------------------------------------------------------------- helpers */

const char *system_state_name(void)
{
  switch (state) {
  case ST_OFF: return "off";
  case ST_GESTURE: return "power-on gesture";
  case ST_PROFILE: return "profile selection";
  case ST_ON: return in_flight ? "flight" : "ground";
  case ST_POWER_OFF: return "power-off melody";
  case ST_SYNC: return "track sync";
  case ST_USB: return "USB mode";
  default: return "?";
  }
}

uint8_t system_profile(void)
{
  return profile;
}

bool system_wants_usb_drive(void)
{
  return usb_drive_wanted;
}

static uint8_t ground_voice(void)
{
  return config_get()->volume_speaker[LEVEL_GROUND];
}

static uint8_t ground_piezo(void)
{
  return config_get()->volume_buzzer[LEVEL_GROUND];
}

/* In flight: the flight level (the ground level when muted, so system sounds stay audible). */
static uint8_t system_piezo(void)
{
  const config_t *c = config_get();
  if (!in_flight || c->flight_level == LEVEL_MUTE) {
    return ground_piezo();
  }
  uint8_t v = c->volume_buzzer[c->flight_level];
  return v > ground_piezo() ? v : ground_piezo();
}

static uint8_t system_voice(void)
{
  const config_t *c = config_get();
  if (!in_flight || c->flight_level == LEVEL_MUTE) {
    return ground_voice();
  }
  uint8_t v = c->volume_speaker[c->flight_level];
  return v > ground_voice() ? v : ground_voice();
}

static volatile uint32_t last_button_ms; /* last button event, for the double tap */
static uint32_t double_taps;

static void on_button(const button_event_t *e)
{
  last_button_ms = HAL_GetTick();
  sensors_handling(); /* barometer held off for a moment (fusion.h) */
  event_t ev = {EV_BUTTON, *e};
  xQueueSend(events, &ev, 0);
}

static void on_double_tap(void)
{
  event_t ev = {EV_TAP, {BUTTON_UP, 0}};
  xQueueSend(events, &ev, 0);
}

#define TAP_BUTTON_GUARD_MS 600U /* pressing and releasing the button shakes the device too */

/* ---- announcements (announce.c): triggers with their block lists ---- */

#define CLIMB_AVG_S 10U /* climb rate in announcements: mean of the last 10 s */

static announce_auto_t ann_auto;
static announcement_t ann_pending; /* groups still to be spoken, one sound_say (or the tone) each */
static unsigned ann_next;
static uint8_t ann_voice;
static float climb_hist[CLIMB_AVG_S];
static unsigned climb_i, climb_n;
static thermal_t thermal;  /* circling, climb phases, wind and glide (thermal.c) */
/* extension clips in the voice pack or voices/custom: bit 0 glide_ratio, 1 g_load, 2 bank_angle, 3 the g/g_<tenths>
 * series, 4 the bank/bank_<deg> series; -1: not looked up since the pack or the custom clips changed */
static int ext_words = -1;

void system_voice_clips_changed(void)
{
  ext_words = -1;
}

static void open_voice_pack(const char *lang)
{
  storage_open_voice_pack(lang);
  ext_words = -1;
}

static const announce_config_t *ann_config(void)
{
  return &config_get()->profile[profile - 1].announce;
}

static void announce_input(announce_input_t *in)
{
  sensors_t s;
  gps_fix_t fx;
  sensors_get(&s);
  gps_get_fix(&fx);
  memset(in, 0, sizeof(*in));
  in->in_flight = in_flight;
  in->alt_m = s.alt_m;
  in->climb_ms = s.vario_ms;
  if (climb_n) {
    float sum = 0.0f;
    for (unsigned i = 0; i < climb_n; i++) {
      sum += climb_hist[i];
    }
    in->climb_ms = sum / (float)climb_n;
  }
  in->fix = fx.valid && fx.fix_type == 3;
  in->speed_kmh = fx.speed_ms * 3.6f;
  in->course_deg = fx.course_deg;
  in->airtime_s = stats.airtime_s;
  in->time_valid = rtc_time_valid();
  in->local_s = in->time_valid ? system_local_unix(rtc_time_unix()) % 86400U : 0U;
  uint32_t mv = system_battery_mv();
  in->battery_percent = mv ? (int)analog_battery_percent(mv) : -1;
  in->wind_valid = thermal_wind(&thermal, &in->wind_kmh, &in->wind_from_deg);
  in->glide_valid = thermal_glide_now(&thermal, &in->glide_ratio);
  in->thermal_valid = thermal.circling;
  in->thermal_climb_ms = thermal.thermal_ms;
  in->thermal_gain_m = s.alt_m - thermal.phase_alt;
  in->stats_valid = stats.flown;
  in->max_alt_m = stats.alt_max_m;
  in->max_speed_kmh = stats.speed_max_ms * 3.6f;
  in->max_climb_ms = stats.climb_max_ms;
  in->max_sink_ms = stats.sink_max_ms;
  if (ext_words < 0) {
    ext_words = (storage_voice_exists("glide_ratio") ? 1 : 0) | (storage_voice_exists("g_load") ? 2 : 0) |
                (storage_voice_exists("bank_angle") ? 4 : 0) |
                (storage_voice_exists("g/g_10") && storage_voice_exists("g/g_100") ? 8 : 0) |
                (storage_voice_exists("bank/bank_5") && storage_voice_exists("bank/bank_90") ? 16 : 0);
  }
  in->glide_word = (ext_words & 1) != 0;
  in->g_word = (ext_words & 2) != 0;
  in->bank_word = (ext_words & 4) != 0;
  in->g_series = (ext_words & 8) != 0;
  in->bank_series = (ext_words & 16) != 0;
}

/* Replaces what is still to be spoken. */
static void announce_queue(const announcement_t *a, uint8_t voice)
{
  ann_pending = *a;
  ann_next = 0;
  ann_voice = voice;
}

/* Adds to what is still to be spoken (events do not cut off an announcement). */
static void announce_append(const announcement_t *a, uint8_t voice)
{
  if (ann_next >= ann_pending.groups) {
    announce_queue(a, voice);
    return;
  }
  for (unsigned g = 0; g < a->groups && ann_pending.groups < ANN_GROUPS; g++) {
    unsigned d = ann_pending.groups++;
    ann_pending.count[d] = a->count[g];
    memcpy(ann_pending.clip[d], a->clip[g], sizeof(ann_pending.clip[d]));
  }
  ann_voice = voice;
}

/* Every system tick: the next group once the sound task is free; a group without clips is the signal tone. */
static void announce_drain(void)
{
  if (ann_next >= ann_pending.groups || sound_busy()) {
    return;
  }
  unsigned n = ann_pending.count[ann_next];
  if (n == 0) {
    sound_melody(MELODY_THERMAL_EXIT, system_piezo());
    ann_next++;
    return;
  }
  const char *clips[ANN_CLIPS];
  for (unsigned i = 0; i < n; i++) {
    clips[i] = ann_pending.clip[ann_next][i];
  }
  sound_say(clips, n, ann_voice);
  ann_next++;
}

/*
 * Speaks a trigger's list. Automatic triggers stay silent at the flight level mute and leave
 * out the blocks marked :ble while a phone is connected; a request (double tap) does neither
 * and comes first.
 */
static void announce_trigger(announce_trigger_t t, const announce_input_t *in, bool request)
{
  const announce_config_t *c = ann_config();
  bool muted = in_flight && config_get()->flight_level == LEVEL_MUTE;
  if (!c->enable[t] || (muted && !request)) {
    return;
  }
  ble_info_t ble;
  ble_get_info(&ble);
  announcement_t a;
  announce_build(&c->list[t], ble.connected && !request, in, &a);
  if (!a.groups) {
    return;
  }
  rawlog_event("announcement %s", announce_trigger_names[t]);
  if (request) {
    sound_stop();
    announce_queue(&a, muted ? ground_voice() : system_voice());
  } else {
    announce_append(&a, system_voice());
  }
}

/* The takeoff thresholds from config_ext.ultrabip (flight_takeoff* keys). */
/* Takeoff thresholds and the impact and handling landing rules from the configuration. */
static void apply_takeoff_config(void)
{
  const config_t *c = config_get();
  flight_takeoff_t t = {(float)c->takeoff_speed_kmh / 3.6f, c->takeoff_speed_s, (float)c->takeoff_distance_m,
                        (float)c->takeoff_altitude_m, (float)c->takeoff_climb_cms / 100.0f, c->takeoff_climb_s};
  flight_set_takeoff(&flight, &t);
  flight_landing_t l = {c->landing_impact, (float)c->landing_impact_g, (float)c->landing_slow_kmh / 3.6f,
                        c->landing_impact_s, c->landing_handling};
  flight_set_landing_rules(&flight, &l);
}

static void thermal_events(unsigned ev, uint32_t utc)
{
  announce_input_t in;
  announce_input(&in);
  if (ev & THERMAL_EXIT) {
    rawlog_event("thermal exit %.2f %.0f", (double)thermal.exit_climb_ms, (double)thermal.exit_gain_m);
    in.thermal_valid = true;
    in.thermal_climb_ms = thermal.exit_climb_ms;
    in.thermal_gain_m = thermal.exit_gain_m;
    announce_trigger(TRIG_THERMALEXIT, &in, false);
  }
  if (ev & THERMAL_CLIMB) {
    rawlog_event("thermal climb %.2f", (double)thermal.thermal_ms);
    announce_trigger(TRIG_THERMALING, &in, false);
  }
  if (ev & THERMAL_WIND) {
    rawlog_event("wind %.1f km/h from %.0f deg", (double)in.wind_kmh, (double)in.wind_from_deg);
    recorder_wind(utc, in.wind_kmh, in.wind_from_deg);
    announce_trigger(TRIG_WIND, &in, false);
  }
  if (ev & THERMAL_GLIDE) {
    rawlog_event("glide ratio %.1f", (double)thermal.glide);
    in.glide_valid = true;
    in.glide_ratio = thermal.glide;
    announce_trigger(TRIG_GLIDE, &in, false);
  }
}

/* Configurator play button: a trigger's list (unsaved) with demo values, at the ground volume. */
void system_test_announce(const char *trigger, const char *items)
{
  announce_list_t l;
  announce_list_parse(&l, items);
  announce_input_t in;
  announce_input(&in); /* the extension words of the voice pack */
  announce_demo_input(&in);
  in.in_flight = strcmp(trigger, "summary") != 0;
  announcement_t a;
  announce_build(&l, false, &in, &a);
  if (!a.groups) {
    sound_melody(MELODY_ACK, ground_piezo());
    return;
  }
  sound_stop();
  announce_queue(&a, ground_voice());
  rawlog_event("announcement demo %s", trigger);
}

/* Double tap while switched on: the tap trigger's list. */
static void double_tap(void)
{
  if (state != ST_ON) {
    return;
  }
  if (button_pressed() || HAL_GetTick() - last_button_ms < TAP_BUTTON_GUARD_MS) {
    rawlog_event("double tap ignored: button");
    return;
  }
  double_taps++;
  if (config_get()->profile[profile - 1].bt_remote && ble_remote_ready()) {
    /* the remote: a key press on the phone instead of the announcement (stock behaviour); the
     * first one after power on says "Bluetooth remote control mode" */
    ble_remote_press();
    if (!remote_said) {
      remote_said = true;
      sound_say1("bluetooth_remote_event", system_voice());
    }
    log_printf("Double tap: remote key");
    rawlog_event("double tap: remote key");
    return;
  }
  const announce_config_t *c = ann_config();
  bool muted = in_flight && config_get()->flight_level == LEVEL_MUTE;
  bool on = c->enable[TRIG_TAP] && (in_flight || c->tap_before_takeoff) && (!muted || c->tap_when_silent);
  log_printf("Double tap%s", on ? "" : " (announcement off)");
  rawlog_event("double tap");
  if (!on) {
    return;
  }
  announce_input_t in;
  announcement_t a;
  announce_input(&in);
  announce_build(&c->list[TRIG_TAP], false, &in, &a);
  if (!a.groups) {
    sound_melody(MELODY_ACK, system_piezo()); /* nothing to say yet (e.g. no fix) */
    return;
  }
  announce_trigger(TRIG_TAP, &in, true);
}

static void set_usb_drive(bool on)
{
  usb_drive_wanted = on;
  for (int i = 0; i < 50 && console_usb_drive_active() != on; i++) {
    if (!vbus_present()) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

static void enter_usb(void)
{
  state = ST_USB;
  storage_unmount();
  set_usb_drive(true);
  log_printf("USB mode: SD card as USB drive, debug console");
}

static void stop_gps_and_wait(void)
{
  gps_status_t g;
  gps_stop();
  for (int i = 0; i < 40; i++) {
    gps_get(&g);
    if (g.state == GPS_OFF) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

/* Switched off: USB mode with USB power, otherwise standby. */
static void go_off(void)
{
  if (!vbus_present()) {
    /* Should a step below hang, the watchdog reset then ends in standby, not in a loop. */
    backup_write(BACKUP_STANDBY, BACKUP_STANDBY_REQUEST);
  }
  indicator_off();
  sound_stop();
  set_headphones(false); /* as the stock: the speaker path at the power off */
  vario_audio_set(NULL, 0);
  sensors_stop();
  ble_disable();
  stop_gps_and_wait();
  recorder_close();
  rawlog_stop();
  in_flight = false;
  power_set_running(false);
  if (vbus_present()) {
    backup_write(BACKUP_STANDBY, 0);
    enter_usb();
    return;
  }
  storage_unmount();
  state = ST_OFF;
  log_printf("Standby");
  vTaskDelay(pdMS_TO_TICKS(50)); /* let the log reach the console */
  power_standby();
}

/* ------------------------------------------------------------- profiles */

static void profile_clip(uint8_t n, char *buf, size_t size)
{
  const profile_t *p = &config_get()->profile[n - 1];
  if (p->voice_clip[0]) {
    snprintf(buf, size, "%s", p->voice_clip); /* the configurator's voice line, named after the profile */
    if (storage_voice_exists(buf)) {
      return;
    }
  }
  if (p->custom_name[0]) {
    snprintf(buf, size, "profile_custom_%u", n); /* the voice line of configurators before 0.47 */
    if (storage_voice_exists(buf)) {
      return;
    }
  }
  snprintf(buf, size, "profile_%.15s", p->type);
  if (p->type[0] && storage_voice_exists(buf)) {
    return;
  }
  if (n <= 3) {
    snprintf(buf, size, "profile_%u", n);
  } else {
    snprintf(buf, size, "%u", n);
  }
}

static void announce_profile(bool intro)
{
  char name[32];
  profile_clip(enabled[sel], name, sizeof(name));
  const char *clips[2] = {"profile_select", name};
  if (intro) {
    sound_say(clips, 2, ground_voice());
  } else {
    sound_say(&clips[1], 1, ground_voice());
  }
  idle_waiting = false;
}

/* After the profile selection: the battery level (system_batteryannounce_bool), or only a low one. */
static void say_battery_level(void)
{
  const config_t *c = config_get();
  analog_values_t a;
  if (!c->battery_say || !analog_measure(&a)) {
    return;
  }
  battery_mv = a.vbat_mv;
  unsigned pct = analog_battery_percent(a.vbat_mv);
  if (c->battery_say_low_only && pct >= 60U) {
    return;
  }
  static char clip[16];
  unsigned p = (pct + 12U) / 25U * 25U; /* clips for 0, 25, 50, 75, 100 % */
  snprintf(clip, sizeof(clip), "%u_percent", p > 100U ? 100U : p);
  const char *list[] = {"battery", clip};
  sound_say(list, 2, ground_voice());
}

/*
 * The settings a raw log needs to be read correctly (docs/raw-log-format.md, record type 6), with the keys of
 * the configuration files: the device, its sensor, recording and flight detection settings after the header
 * and after a reload; the profile in use with its vario tones at the profile choice and after a reload.
 */
#define SETTINGS_TEXT 240
static char settings_text[SETTINGS_TEXT]; /* system task only */

static const char *bool_text(bool v)
{
  return v ? "true" : "false";
}

static void log_device_settings(void)
{
  const config_t *c = config_get();
  const uint32_t *uid64 = (const uint32_t *)UID64_BASE;
  static const char *const systems[] = {"all", "gps+glonass", "gps"};
  char *s = settings_text;
  snprintf(s, SETTINGS_TEXT, "device serial=%08lX%08lX id=UB%04lX hardware=%s bootloader=BL_v%lu stock=%s firmware=%s",
           (unsigned long)uid64[1], (unsigned long)uid64[0], (unsigned long)(uid64[0] & 0xFFFFU),
           c->hw_version[0] ? c->hw_version : "-", (unsigned long)((RTC->BKP14R >> 16) & 0xFFU),
           c->stock_fw_version[0] ? c->stock_fw_version : "-", FW_VERSION);
  rawlog_settings(s);
  snprintf(s, SETTINGS_TEXT, "sensors system_barorate_int=%u system_gpsrate_int=%u system_gpssystems_string=%s accel_calibration=%s",
           c->baro_rate, c->gps_rate, systems[c->gps_systems < 3U ? c->gps_systems : 0U],
           c->accel_cal_from_file ? "file" : "factory");
  rawlog_settings(s);
  snprintf(s, SETTINGS_TEXT, "recording flight_recordigc_bool=%s flight_recordkml_bool=%s flight_recordrate_int=%u",
           bool_text(c->record_igc), bool_text(c->record_kml), c->record_hz);
  rawlog_settings(s);
  snprintf(s, SETTINGS_TEXT, "takeoff flight_takeoffspeed_int=%u flight_takeoffspeedtime_int=%u flight_takeoffdistance_int=%u "
           "flight_takeoffaltitude_int=%u flight_takeoffclimb_int=%u flight_takeoffclimbtime_int=%u",
           c->takeoff_speed_kmh, c->takeoff_speed_s, c->takeoff_distance_m, c->takeoff_altitude_m, c->takeoff_climb_cms,
           c->takeoff_climb_s);
  rawlog_settings(s);
  snprintf(s, SETTINGS_TEXT, "landing flight_landingdetect_bool=%s flight_landingtime_int=%u flight_landingimpact_bool=%s "
           "flight_landingimpactg_int=%u flight_landingspeed_int=%u flight_landingimpacttime_int=%u flight_landinghandling_bool=%s",
           bool_text(c->landing_detect), c->landing_time_s, bool_text(c->landing_impact), c->landing_impact_g,
           c->landing_slow_kmh, c->landing_impact_s, bool_text(c->landing_handling));
  rawlog_settings(s);
}

static void log_profile_settings(void)
{
  const profile_t *p = &config_get()->profile[profile - 1];
  const vario_tones_t *t = &p->tones;
  char *s = settings_text;
  snprintf(s, SETTINGS_TEXT, "profile number=%u identity_profilename_string=%s gps_takeoffdetection_bool=%s "
           "system_tapsensitivity_int=%u system_timeoffsetminutes_int=%d system_timezoneauto_bool=%s",
           profile, p->type[0] ? p->type : "-", bool_text(p->takeoff_detect), p->tap_sensitivity, p->time_offset_min,
           bool_text(p->tz_auto));
  rawlog_settings(s);
  snprintf(s, SETTINGS_TEXT, "vario vario_liftstart_int=%d vario_sinkstart_int=%d vario_sinkalarm_bool=%s vario_sweeptone_bool=%s "
           "vario_liftysound_bool=%s vario_integrationtime_int=%u",
           t->lift_start_cms, t->sink_start_cms, bool_text(t->sink_alarm), bool_text(t->sweep), bool_text(t->lifty),
           t->integration_pct);
  rawlog_settings(s);
  int n = snprintf(s, SETTINGS_TEXT, "tones vz,freq,duration,duty=");
  for (int i = 0; i < VARIO_TONE_POINTS && n > 0 && (size_t)n < SETTINGS_TEXT; i++) {
    n += snprintf(s + n, SETTINGS_TEXT - (size_t)n, "%s%d,%u,%u,%u", i ? " " : "", t->point[i].vz_cms, t->point[i].freq_hz,
                  t->point[i].period_ms, t->point[i].duty);
  }
  rawlog_settings(s);
}

static void enter_ground(void)
{
  const config_t *c = config_get();
  const profile_t *p = &c->profile[profile - 1];
  sensors_set_tap_sensitivity(p->tap_sensitivity);
  /* takeoff detection off: flight mode from the first fix on, so no landing detection either */
  flight_set_landing(&flight, c->landing_detect && p->takeoff_detect, c->landing_time_s);
  apply_takeoff_config();
  alarm_above = alarm_below = false;
  state = ST_ON;
  fresh_press_needed = button_pressed();
  log_printf("Ground: profile %u%s", profile, p->takeoff_detect ? "" : " (no takeoff detection)");
  log_profile_settings();
  say_battery_level();
}

static void confirm_profile(void)
{
  confirmed = true;
  profile = enabled[sel];
  sound_stop();
  sound_melody(MELODY_CONFIRM, ground_piezo()); /* right away: the long press was accepted */
  sound_say1("profile_selected", ground_voice());
  log_printf("Profile %u selected", profile);
  rawlog_event("profile %u", profile);
  if (profile != config_get()->profile_last) {
    config_save_profile_last(profile);
  }
  enter_ground();
}

static void start_profile_selection(void)
{
  const config_t *c = config_get();
  enabled_count = 0;
  sel = 0;
  for (uint8_t i = 0; i < PROFILE_MAX; i++) {
    if (c->profile[i].present && c->profile[i].enabled) {
      if (i + 1U == c->profile_last) {
        sel = enabled_count;
      }
      enabled[enabled_count++] = (uint8_t)(i + 1U);
    }
  }
  if (enabled_count == 0) {
    enabled[enabled_count++] = 1;
  }
  state = ST_PROFILE;
  confirmed = false;
  fresh_press_needed = button_pressed();
  announcements = 1;
  if (enabled_count == 1) {
    announce_profile(false);
    confirm_profile();
    return;
  }
  announce_profile(true);
  log_printf("Profile selection: %u profiles, preselected %u", enabled_count, enabled[sel]);
}

static void say_pairing_confirmed(void); /* below, with the other Bluetooth announcements */

static void profile_event(const event_t *ev)
{
  if (ev && ev->type == EV_BUTTON) {
    const button_event_t *b = &ev->button;
    if (b->type == BUTTON_DOWN) {
      fresh_press_needed = false;
    } else if (fresh_press_needed) {
      return; /* still the press that switched the device on */
    } else if (b->type == BUTTON_HOLD && b->ms >= CONFIRM_HOLD_MS && !confirmed) {
      confirm_profile();
    } else if (b->type == BUTTON_UP && b->ms < CONFIRM_HOLD_MS && biplink_auth_pending()) {
      biplink_auth_confirm();
      say_pairing_confirmed();
    } else if (b->type == BUTTON_UP && b->ms < CONFIRM_HOLD_MS) {
      sel = (uint8_t)((sel + 1U) % enabled_count);
      sound_stop();
      sound_melody(MELODY_NEXT, ground_piezo());
      announce_profile(false);
      announcements = 1;
    }
    return;
  }
  if (state != ST_PROFILE || sound_busy()) {
    return;
  }
  if (!idle_waiting) {
    idle_waiting = true;
    idle_since = HAL_GetTick();
  } else if (HAL_GetTick() - idle_since >= PROFILE_IDLE_MS) {
    if (announcements >= PROFILE_REPEATS) {
      confirm_profile();
    } else {
      announce_profile(false);
      announcements++;
    }
  }
}

/* ------------------------------------------------------------ power on/off */

/*
 * At power on, before the GNSS, the sensors or the card start: an empty battery keeps the device off
 * (battery.h), with the error melody and the red LED instead of the start melody. The mean of three
 * readings under the light load of the start.
 */
static bool battery_allows_start(void)
{
  analog_values_t a;
  uint32_t sum = 0, n = 0;
  for (int i = 0; i < 3; i++) {
    if (analog_measure(&a)) {
      sum += a.vbat_mv;
      n++;
    }
  }
  uint32_t mv = n ? sum / n : 0U;
  battery_mv = mv;
  if (battery_can_start(mv, vbus_present())) {
    return true;
  }
  log_printf("Battery empty: %lu mV - not switching on", (unsigned long)mv);
  indicator_blinks(LED_RED, 3, 1500);
  sound_melody(MELODY_ERROR, ground_piezo());
  for (int i = 0; i < 15; i++) {
    health_alive(HEALTH_SYSTEM);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return false;
}

static void power_on(bool melody, bool select_profile)
{
  battery_empty = false;
  if (!battery_allows_start()) {
    go_off();
    return;
  }
  if (melody) {
    sound_melody(MELODY_POWER_ON, ground_piezo());
    indicator_power_on();
  }
  log_printf("Power on");
  power_set_running(true);
  running_since = HAL_GetTick();
  resets_cleared = false;
  gps_ok_said = false;
  remote_said = false;
  baro_referenced = false;
  last_short_up = 0;
  manual_takeoff = false;
  faults_active = 0;
  faults_said = 0;
  gps_silent_checks = 0;
  fault_tick = HAL_GetTick();
  flight_init(&flight);
  flight_fix_seq = 0;
  flight_second = (nmea_rate_t){0};
  record_rate = (nmea_rate_t){0};
  in_flight = false;
  recorder_reset();
  summary_reset(&stats);
  battery_watch_init(&battery);
  battery_tick = HAL_GetTick() - BATTERY_CHECK_MS; /* first reading at once (power indicator) */
  set_usb_drive(false);
  storage_mount();
  config_load(config_get());
  config_mirror_if_found();
  apply_uart_console();
  sound_set_power_off_melody(config_get()->power_off_melody);
  /* sensor settings before the sensors start: no restart for a barometer rate */
  sensors_set_baro_rate(config_get()->baro_rate);
  sensors_set_accel_calibration(config_get()->accel_offset, config_get()->accel_scale);
  gps_set_rate(config_get()->gps_rate);
  gps_set_systems(config_get()->gps_systems);
  gps_start(GPS_START_AUTO); /* search satellites while the profile is chosen */
  sensors_start();           /* the vertical filter settles meanwhile */
  ble_enable();
  still_since = 0;
  open_voice_pack(config_get()->language);
  if (config_get()->rawlog && rawlog_start()) {
    log_device_settings();
  }
  flight_set_landing(&flight, config_get()->landing_detect, config_get()->landing_time_s);
  apply_takeoff_config();
  if (boot.reason == BOOT_UPDATE) {
    sound_say1(power_update_succeeded() ? "update_ok" : "update_fail", ground_voice());
    boot.reason = BOOT_OTHER;
  }
  if (select_profile) {
    start_profile_selection();
  } else {
    profile = config_get()->profile_last;
    enter_ground();
  }
}

static void start_power_off_melody(void)
{
  before_off = state;
  state = ST_POWER_OFF;
  sound_stop();
  sound_melody(MELODY_POWER_OFF, system_piezo());
  indicator_solid(LED_RED);
  off_started = HAL_GetTick();
}

/* Flight summary: the summary trigger's list, then wait until it is spoken. */
static void announce_summary(uint8_t voice)
{
  if (!stats.flown) {
    return;
  }
  log_printf("Summary: airtime %lu min, max altitude %.0f m, max speed %.0f km/h, climb %.1f m/s, sink %.1f m/s, "
             "%.1f g",
             (unsigned long)((stats.airtime_s + 30U) / 60U), (double)stats.alt_max_m,
             (double)(stats.speed_max_ms * 3.6f), (double)stats.climb_max_ms, (double)stats.sink_max_ms,
             (double)stats.g_max);
  const announce_config_t *c = ann_config();
  if (!c->enable[TRIG_SUMMARY]) {
    return;
  }
  announce_input_t in;
  announce_input(&in);
  in.in_flight = false;
  in.stats_valid = true;
  in.g_load = stats.g_max;
  ble_info_t ble;
  ble_get_info(&ble);
  announcement_t a;
  announce_build(&c->list[TRIG_SUMMARY], ble.connected, &in, &a);
  if (!a.groups) {
    return;
  }
  announce_queue(&a, voice);
  for (int i = 0; i < 600 && (i < 3 || ann_next < ann_pending.groups || sound_busy()); i++) { /* at most 60 s */
    announce_drain();
    health_alive(HEALTH_SYSTEM);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

/*
 * After a flight: Bluetooth stays on for system_syncwindow_int seconds so a phone can fetch the
 * tracks (longer while connected; a button press ends it). With system_syncwhilemoving_bool the
 * IMU keeps running and the time counts from the moment the device lies still: switched off after
 * the landing, carried to the packing place, put down - Bluetooth is still on when the phone comes
 * out. Never longer than SYNC_MAX_MS. The GPS is switched off meanwhile.
 */
static void track_sync_window(void)
{
  const config_t *cfg = config_get();
  uint32_t window_ms = (uint32_t)cfg->sync_window_s * 1000U;
  bool wait_still = cfg->sync_while_moving;
  ble_info_t b;
  ble_get_info(&b);
  if (!b.running || vbus_present() || (window_ms == 0U && !wait_still)) {
    return;
  }
  state = ST_SYNC;
  if (!wait_still) {
    sensors_stop();
  }
  stop_gps_and_wait();
  indicator_heartbeat(LED_BLUE);
  log_printf("Track sync: Bluetooth stays on for %lu s%s", (unsigned long)cfg->sync_window_s,
             wait_still ? " once the device lies still" : "");
  uint32_t start = HAL_GetTick();
  uint32_t still_since = start; /* moving restarts the time */
  for (;;) {
    health_alive(HEALTH_SYSTEM);
    ble_get_info(&b);
    uint32_t now = HAL_GetTick();
    if (wait_still) {
      sensors_t sn;
      sensors_get(&sn);
      if (sn.imu_ok && !sn.at_rest) {
        still_since = now;
      }
    }
    if (now - start >= SYNC_MAX_MS || (now - still_since >= window_ms && !b.connected) || vbus_present()) {
      break;
    }
    if (read_battery() == BATTERY_EVENT_CRITICAL) {
      log_printf("Track sync ended: battery empty (%lu mV)", (unsigned long)battery_mv);
      break;
    }
    event_t ev;
    if (xQueueReceive(events, &ev, pdMS_TO_TICKS(100)) == pdTRUE && ev.type == EV_BUTTON &&
        ev.button.type == BUTTON_DOWN) {
      log_printf("Track sync ended by the button");
      break;
    }
  }
  if (wait_still) {
    sensors_stop();
  }
  indicator_off();
}

static void shutdown(void)
{
  log_printf("Power off");
  ann_pending.groups = 0;
  bool flown = stats.flown;
  uint8_t voice = system_voice();
  vario_audio_set(NULL, 0);
  recorder_close(); /* complete and signed before the summary and the download window */
  rawlog_stop();    /* the raw log ends with the track (the sync window runs the IMU for its stillness rule only) */
  if (battery_empty) {
    /* no summary, no Bluetooth window: the warning still plays out (at most 4 s), then off */
    for (int i = 0; i < 40 && sound_busy(); i++) {
      health_alive(HEALTH_SYSTEM);
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  } else {
    announce_summary(voice);
    if (flown) {
      track_sync_window();
    }
  }
  go_off();
}

/*
 * Auto power off (system_autopowerofftime_int, 0 = never): after that many minutes lying still on
 * the ground - never in flight, while Bluetooth is connected or on USB power. A device carried
 * up the hill moves and stays on; after a landing it switches off in the car or on the table.
 */
static void check_auto_off(const button_event_t *b)
{
  uint32_t now = HAL_GetTick();
  if (b) {
    still_since = 0;
  }
  if (now - auto_off_tick < AUTO_OFF_RECHECK_MS) {
    return;
  }
  auto_off_tick = now;
  uint16_t minutes = config_get()->profile[profile - 1].auto_off_min;
  sensors_t s;
  ble_info_t bi;
  sensors_get(&s);
  ble_get_info(&bi);
  if (minutes == 0U || in_flight || !s.at_rest || bi.connected || vbus_present()) {
    still_since = 0;
    return;
  }
  if (!still_since) {
    still_since = now;
  } else if (now - still_since >= (uint32_t)minutes * 60000U) {
    log_printf("Auto power off: still for %u min on the ground", minutes);
    rawlog_event("auto power off");
    sound_stop();
    sound_melody(MELODY_POWER_OFF, system_piezo());
    shutdown();
  }
}


/*
 * Hardware faults the pilot must hear about without a laptop: a sensor that does not answer, a
 * GNSS module that stays silent, no SD card. Checked every 10 s from 30 s after power on; each
 * new fault is said once ("warning, error, <part>") and the LED blinks red while it persists.
 */
#define FAULT_BARO  (1U << 0)
#define FAULT_IMU   (1U << 1)
#define FAULT_GPS   (1U << 2)
#define FAULT_SD    (1U << 3)
#define FAULT_FIRST_MS 30000U
#define FAULT_CHECK_MS 10000U

static void check_faults(void)
{
  uint32_t now = HAL_GetTick();
  if (now - running_since < FAULT_FIRST_MS || now - fault_tick < FAULT_CHECK_MS) {
    return;
  }
  fault_tick = now;
  sensors_t sn;
  sensors_get(&sn);
  gps_status_t g;
  gps_get(&g);
  uint8_t f = 0;
  if (sn.on && !sn.baro_ok) {
    f |= FAULT_BARO;
  }
  if (sn.on && !sn.imu_ok) {
    f |= FAULT_IMU;
  }
  /* silent in two checks in a row: not just the pause of a restart or of the rate search */
  gps_silent_checks = g.state != GPS_OFF && !g.alive ? (uint8_t)(gps_silent_checks + 1U) : 0U;
  if (gps_silent_checks >= 2U) {
    f |= FAULT_GPS;
  }
  if (!storage_is_mounted()) {
    f |= FAULT_SD;
  }
  uint8_t fresh = (uint8_t)(f & ~faults_said);
  if (fresh) {
    log_printf("FAULT:%s%s%s%s", fresh & FAULT_BARO ? " barometer" : "", fresh & FAULT_IMU ? " IMU" : "",
               fresh & FAULT_GPS ? " GNSS module silent" : "", fresh & FAULT_SD ? " no SD card" : "");
    faults_said |= fresh;
    if (!(fresh & FAULT_SD)) { /* the clips live on the card */
      const char *clips[4] = {"warning", "error", NULL, NULL};
      unsigned n = 2;
      if (fresh & FAULT_BARO) {
        clips[n++] = "sensors_pressure";
      }
      if (fresh & FAULT_IMU) {
        clips[n++] = "sensors_imu";
      }
      if (fresh & FAULT_GPS && n < 4U) {
        clips[n++] = "gps";
      }
      sound_say(clips, n, system_voice());
    }
  }
  if (!f && faults_active) {
    log_printf("Faults cleared");
  }
  faults_active = f;
}

/*
 * BipLink uploaded a new configuration: taken over and applied without a restart (the stock
 * switches off instead). Announced with the stock's clip "loading configuration"; the GNSS module
 * and the voice pack are restarted only when their settings changed.
 */
static volatile bool reload_requested; /* the configurator's RELOAD (ble.c) */

void system_request_config_reload(void)
{
  reload_requested = true;
}

/*
 * A new configuration taken over at once: a file uploaded by the BipLink app (config.new.ultrabip), or the
 * configurator's "Apply now" after it saved the files. Sensors, GNSS and the voice pack restart only when
 * their settings changed. The Bluetooth name, the remote control and the raw log follow at the next power on.
 */
static void check_config_update(void)
{
  if (config_take_if_found_request()) {
    config_mirror_if_found(); /* the configurator sent a new note */
  }
  bool from_app = biplink_take_config_update();
  bool from_configurator = reload_requested;
  reload_requested = false;
  if (!from_app && !from_configurator) {
    return;
  }
  const config_t *c = config_get();
  uint8_t gps_rate = c->gps_rate, gps_systems = c->gps_systems;
  char language[sizeof(c->language)];
  snprintf(language, sizeof(language), "%s", c->language);
  log_printf("Config: %s - reloading", from_app ? "new file from BipLink" : "the configurator asked to apply it");
  sound_say1("loading_config", system_voice());
  config_adopt_new();
  config_load(config_get());
  apply_uart_console();
  c = config_get();
  sound_set_power_off_melody(c->power_off_melody);
  sensors_set_baro_rate(c->baro_rate); /* restarts the sensors itself when the rate changed */
  sensors_set_accel_calibration(c->accel_offset, c->accel_scale);
  if (c->gps_rate != gps_rate || c->gps_systems != gps_systems) {
    gps_set_rate(c->gps_rate);
    gps_set_systems(c->gps_systems);
    gps_start(GPS_START_AUTO);
  }
  if (strcmp(c->language, language) != 0) {
    open_voice_pack(c->language);
  }
  flight_set_landing(&flight, c->landing_detect, c->landing_time_s);
  apply_takeoff_config();
  sensors_set_tap_sensitivity(c->profile[profile - 1].tap_sensitivity); /* the profile in use */
  rawlog_event("config reloaded (%s)", from_app ? "BipLink" : "configurator");
  log_device_settings();
  log_profile_settings();
}

/* Every 10 s on battery: a warning, and a clean power off before the battery is empty. */
/*
 * USB-C audio adapter (as the stock firmware): with the detect line below ANALOG_AUDIO_ADAPTER_MV
 * twice in a row and no USB power, the output switches to the headphones (PH3 high) with a short
 * melody there; unplugged, or with USB power, back to the speaker and the USB data lines.
 */
#define AUDIO_CHECK_MS 1000U
static uint32_t audio_check_tick;
static uint8_t audio_votes;

static void set_headphones(bool on)
{
  if (on == sound_headphones()) {
    return;
  }
  sound_stop();
  board_audio_headphones(on);
  sound_set_headphones(on);
  log_printf("Audio: %s", on ? "headphones (USB-C audio adapter)" : "speaker");
  if (on) {
    sound_melody(MELODY_CONFIRM, system_voice());
  }
}

static void check_audio_adapter(void)
{
  if (HAL_GetTick() - audio_check_tick < AUDIO_CHECK_MS) {
    return;
  }
  audio_check_tick = HAL_GetTick();
  bool plugged = !vbus_present() && analog_audio_detect_mv() < ANALOG_AUDIO_ADAPTER_MV;
  if (plugged == sound_headphones()) {
    audio_votes = 0;
    return;
  }
  if (++audio_votes >= 2U || vbus_present()) {
    audio_votes = 0;
    set_headphones(plugged);
  }
}

/* Every BATTERY_CHECK_MS: the battery voltage into the watch (battery.h); no events on USB power. */
static battery_event_t read_battery(void)
{
  if (HAL_GetTick() - battery_tick < BATTERY_CHECK_MS) {
    return BATTERY_EVENT_NONE;
  }
  battery_tick = HAL_GetTick();
  analog_values_t a;
  if (!analog_measure(&a)) {
    return BATTERY_EVENT_NONE;
  }
  battery_mv = a.vbat_mv;
  if (vbus_present()) {
    return BATTERY_EVENT_NONE; /* charging: no warnings */
  }
  return battery_watch_update(&battery, a.vbat_mv);
}

/* Switched on (profile selection, ground, flight): a warning in the log, the power off when empty. */
static void check_battery(void)
{
  battery_event_t ev = read_battery();
  uint32_t mv = battery_mv;
  if (ev == BATTERY_EVENT_LOW) {
    log_printf("Battery low: %lu mV", (unsigned long)mv); /* no voice: the level is said after power on */
    rawlog_event("battery low %lu", (unsigned long)mv);
  } else if (ev == BATTERY_EVENT_CRITICAL) {
    log_printf("Battery empty: %lu mV - switching off", (unsigned long)mv);
    rawlog_event("battery empty %lu", (unsigned long)mv);
    sound_stop();
    sound_say1("battery", system_voice());
    sound_melody(MELODY_POWER_OFF, system_piezo());
    battery_empty = true;
    shutdown();
  }
}

/*
 * USB was unplugged in USB mode: an ultrabip*.update copied onto the drive is
 * installed right away (the bootloader checks and flashes it after a window
 * watchdog reset). Afterwards the device announces the result and stays off.
 */
static void install_dropped_update(void)
{
  set_usb_drive(false);
  if (!storage_mount()) {
    return;
  }
  storage_lock();
  bool found = handoff_prepare_update_from_root();
  storage_unlock();
  if (!found) {
    return;
  }
  analog_values_t a;
  if (analog_measure(&a) && a.vbat_mv < UPDATE_MIN_VBAT_MV) {
    log_printf("Update: battery %lu mV is too low, not installed", (unsigned long)a.vbat_mv);
    sound_melody(MELODY_ERROR, ground_piezo());
    vTaskDelay(pdMS_TO_TICKS(1000));
    return;
  }
  log_printf("Update found after unplugging USB: installing");
  backup_write(BACKUP_AFTER_UPDATE, BACKUP_UPDATE_THEN_OFF);
  sound_melody(MELODY_CONFIRM, ground_piezo());
  vTaskDelay(pdMS_TO_TICKS(400));
  storage_unmount();
  power_set_running(false);
  handoff_wwdg_reset();
}

/* After an update started from USB mode: result as melody and voice, then off. */
static void announce_update_and_go_off(void)
{
  bool ok = power_update_succeeded();
  log_printf("Update %s", ok ? "installed" : "FAILED");
  storage_mount();
  config_load(config_get());
  config_mirror_if_found();
  apply_uart_console();
  open_voice_pack(config_get()->language);
  sound_melody(ok ? MELODY_CONFIRM : MELODY_ERROR, ground_piezo());
  sound_say1(ok ? "update_ok" : "update_fail", ground_voice());
  for (int i = 0; i < 60 && (i < 3 || sound_busy()); i++) {
    health_alive(HEALTH_SYSTEM);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  go_off();
}

static void do_update(void)
{
  log_printf("Update requested");
  sound_stop();
  stop_gps_and_wait();
  recorder_close();
  rawlog_stop();
  set_usb_drive(false);
  storage_mount();
  storage_lock();
  bool ok = handoff_prepare_update_from_root();
  storage_unlock();
  if (!ok) {
    log_printf("Update: nothing installed");
    if (state == ST_USB) {
      enter_usb();
    } else if (state != ST_OFF) {
      gps_start(GPS_START_AUTO);
    }
    return;
  }
  storage_unmount();
  power_set_running(false);
  handoff_wwdg_reset();
}

/* --------------------------------------------------------------- gesture */

static void gesture_abort(const char *why)
{
  log_printf("Power-on gesture: %s - back to %s", why, gesture_from_usb ? "USB mode" : "standby");
  indicator_off();
  if (gesture_from_usb && vbus_present()) {
    state = ST_USB;
    return;
  }
  go_off();
}

/* Button events and the 50 ms tick (ev == NULL); gesture.c checks the time limits on both. */
static void gesture_event(const event_t *ev)
{
  const button_event_t *b = (ev && ev->type == EV_BUTTON) ? &ev->button : NULL;
  gesture_phase_t before = gesture.phase;
  switch (gesture_step(&gesture, HAL_GetTick(), b, button_pressed())) {
  case GESTURE_CONTINUE:
    if (before != gesture.phase) {
      /* white while the button is held, as for the wake-up press from standby (main.c) */
      if (gesture.phase == GESTURE_WAIT_HOLD) {
        indicator_solid(LED_WHITE);
      } else {
        indicator_off();
      }
    }
    break;
  case GESTURE_POWER_ON:
    indicator_off();
    power_on(true, true);
    break;
  case GESTURE_ABORT:
    gesture_abort(gesture.why);
    break;
  }
}

static void start_gesture_from_usb(void)
{
  sound_melody(MELODY_ACK, ground_piezo());
  indicator_solid(LED_WHITE);
  gesture_from_usb = true;
  gesture_start_pressed(&gesture, HAL_GetTick());
  state = ST_GESTURE;
}

/* ------------------------------------------------------------------ task */

/*
 * Altitude alerts of the profile (alarm_max2_int, alarm_min2_int; 0 = off): the error melody
 * and the altitude once when the limit is crossed, then every 5 min while beyond it; 10 m of
 * hysteresis on the way back. Altitudes above sea level, so only with the GNSS reference.
 */
static void say_altitude_alarm(uint32_t now_s, float alt_m)
{
  static char clip[16];
  snprintf(clip, sizeof(clip), "al_%d", summary_altitude_clip(alt_m));
  const char *list[] = {"al", clip};
  sound_stop();
  sound_melody(MELODY_ERROR, system_piezo());
  sound_say(list, 2, system_voice());
  alarm_next_s = now_s + ALARM_REPEAT_S;
  log_printf("Altitude alert: %.0f m", (double)alt_m);
  rawlog_event("altitude alert %.0f", (double)alt_m);
}

static void check_altitude_alarms(uint32_t now_s, float alt_m)
{
  const profile_t *p = &config_get()->profile[profile - 1];
  bool above = p->alarm_max_m && alt_m > (float)p->alarm_max_m - (alarm_above ? ALARM_HYSTERESIS_M : 0.0f);
  bool below = p->alarm_min_m && alt_m < (float)p->alarm_min_m + (alarm_below ? ALARM_HYSTERESIS_M : 0.0f);
  bool crossed = (above && !alarm_above) || (below && !alarm_below);
  alarm_above = above;
  alarm_below = below;
  if (crossed || ((above || below) && (int32_t)(now_s - alarm_next_s) >= 0)) {
    say_altitude_alarm(now_s, alt_m);
  }
}

/*
 * Wingover and spiral detection (maneuver.c) every 50 ms in flight from the fast
 * acceleration and rotation rate; the result is logged (announcement to follow with
 * the announcement rework).
 */
static void update_maneuver(void)
{
  if (!in_flight) {
    return;
  }
  uint32_t now = HAL_GetTick();
  uint32_t dt_ms = now - maneuver_tick;
  if (dt_ms < TICK_MS) {
    return;
  }
  maneuver_tick = now;
  sensors_t s;
  sensors_get(&s);
  if (!s.imu_ok) {
    return;
  }
  maneuver_kind_t k = maneuver_update(&maneuver, s.accel_fast_ms2 / 9.80665f, s.rate_dps, (float)dt_ms / 1000.0f);
  if (k != MANEUVER_NONE) {
    log_printf("Manoeuvre: %s, %.1f g, bank %.0f deg", maneuver_kind_name(k), (double)maneuver.done_n_max,
               (double)maneuver.done_bank_deg);
    rawlog_event("maneuver %s %.2f %.0f", maneuver_kind_name(k), (double)maneuver.done_n_max,
                 (double)maneuver.done_bank_deg);
    announce_input_t in;
    announce_input(&in);
    in.g_load = maneuver.done_n_max;
    in.bank_deg = maneuver.done_bank_deg;
    announce_trigger(TRIG_WINGOVER, &in, false);
  }
}

/*
 * Per GNSS fix (5 Hz): the IGC data records at their rate (4 Hz by
 * default). Once per second (the first fix of each second): the sample for
 * the pre-record buffer on the ground and the B record in flight, and the
 * takeoff and landing detection (flight.c). Takeoff: flight mode with vario
 * tones at the flight level and the IGC file; landing: silent again, the
 * file goes on.
 */
static void on_landing(uint32_t utc)
{
  log_printf("Landing detected (%02lu:%02lu:%02lu UTC)", (unsigned long)(utc / 3600U % 24U),
             (unsigned long)(utc / 60U % 60U), (unsigned long)(utc % 60U));
  rawlog_event("landing %lu", (unsigned long)utc);
  in_flight = false;
  recorder_landing(utc);
  sound_say1("landing", ground_voice());
}

/* No epoch from the GNSS module for a while (silent or off): in flight the landing detection still runs
 * once per second on the sensors alone (impact and handling rules), so the vario does not beep on after
 * the landing. */
static void flight_without_gnss(void)
{
  if (!in_flight || HAL_GetTick() - flight_step_tick < 1500U) {
    return;
  }
  sensors_t s;
  sensors_get(&s);
  if (!s.valid) {
    return;
  }
  flight_step_tick = HAL_GetTick();
  uint32_t now_s = HAL_GetTick() / 1000U;
  flight_input_t in = {now_s, false, 0, 0, 0.0f, s.alt_std_m, s.vario_ms, false, 0.0f, 0.0f, 0.0f, 0.0f};
  in.motion = sensors_take_motion(&in.acc_max_g, &in.acc_min_g, &in.rate_max_dps);
  if (flight_update(&flight, &in) == FLIGHT_EVENT_LANDING) {
    on_landing(rtc_time_unix() - (now_s - flight.landing_s));
  }
}

static void update_flight(void)
{
  gps_fix_t fx;
  gps_get_fix(&fx);
  if (fx.seq == flight_fix_seq) {
    flight_without_gnss();
    return;
  }
  flight_fix_seq = fx.seq;
  bool second = nmea_rate_due(&flight_second, &fx, 1U);
  bool record = nmea_rate_due(&record_rate, &fx, config_get()->record_hz);
  if (!second && !record) {
    return;
  }
  sensors_t s;
  sensors_get(&s);
  if (!s.valid) {
    return; /* the altitude would jump from 0 once the barometer has settled */
  }
  recorder_sample_t rs = {fx, s.alt_std_m, s.alt_raw_m, s.alt_m, s.vario_ms, s.accel_ms2, second};
  recorder_sample(&rs);
  if (!second) {
    return;
  }
  if (baro_referenced && fx.valid && fx.fix_type == 3 && fx.hdop > 0.0f && fx.hdop <= 2.0f) {
    sensors_track_reference(fx.alt_msl_m);
  }

  flight_step_tick = HAL_GetTick();
  uint32_t now_s = HAL_GetTick() / 1000U;
  flight_input_t in = {now_s, fx.valid && fx.fix_type == 3, fx.lat_e7, fx.lon_e7, fx.speed_ms, s.alt_std_m,
                       s.vario_ms, false, 0.0f, 0.0f, 0.0f, fx.hdop};
  in.motion = sensors_take_motion(&in.acc_max_g, &in.acc_min_g, &in.rate_max_dps);
  flight_event_t ev = flight_update(&flight, &in);
  if (ev == FLIGHT_EVENT_NONE && !config_get()->profile[profile - 1].takeoff_detect && flight.state == FLIGHT_GROUND &&
      in.fix) {
    flight_force_airborne(&flight, now_s); /* gps_takeoffdetection_bool off: airborne with the first fix */
    ev = FLIGHT_EVENT_TAKEOFF;
  }
  if (ev == FLIGHT_EVENT_NONE && manual_takeoff && flight.state == FLIGHT_GROUND && in.fix) {
    flight_force_airborne(&flight, now_s);
    ev = FLIGHT_EVENT_TAKEOFF;
    log_printf("Takeoff by double press");
    rawlog_event("manual takeoff");
  }
  manual_takeoff = false;
  if (ev == FLIGHT_EVENT_TAKEOFF) {
    uint32_t utc = fx.unix_time - (now_s - flight.takeoff_s);
    log_printf("Takeoff detected (began about %lu s ago, %02lu:%02lu:%02lu UTC)", (unsigned long)(now_s - flight.takeoff_s),
               (unsigned long)(utc / 3600U % 24U), (unsigned long)(utc / 60U % 60U), (unsigned long)(utc % 60U));
    rawlog_event("takeoff %lu", (unsigned long)utc);
    in_flight = true;
    announce_auto_start(&ann_auto, now_s, s.alt_m);
    thermal_init(&thermal);
    maneuver_init(&maneuver);
    maneuver_tick = HAL_GetTick();
    system_test_stop(); /* a test from the configurator never continues into the flight */
    recorder_takeoff(utc, profile);
    sound_say1("takeoff", system_voice());
  } else if (ev == FLIGHT_EVENT_LANDING) {
    on_landing(fx.unix_time - (now_s - flight.landing_s));
  }
  if (in_flight) {
    summary_update(&stats, s.alt_m, fx.valid ? fx.speed_ms : -1.0f, s.vario_ms);
    if (maneuver.g_max_flight > stats.g_max) {
      stats.g_max = maneuver.g_max_flight;
    }
    if (baro_referenced) {
      check_altitude_alarms(now_s, s.alt_m);
    }
  }

  climb_hist[climb_i] = s.vario_ms;
  climb_i = (climb_i + 1U) % CLIMB_AVG_S;
  if (climb_n < CLIMB_AVG_S) {
    climb_n++;
  }
  if (in_flight) {
    const announce_config_t *ac = ann_config();
    thermal_config_t tc = {ac->climb_report_s, ac->glide_min_gain_m, ac->thermalexit_min_s,
                           ac->thermalexit_min_gain_m, ac->glide_straight_s, ac->glide_repeat_min};
    unsigned ev = thermal_update(&thermal, &tc, now_s, fx.valid && fx.fix_type == 3, fx.vel_e_ms, fx.vel_n_ms, s.alt_m,
                                 s.yaw_ok, s.yaw_dps);
    if (ev) {
      thermal_events(ev, fx.unix_time);
    }
  }
  if (in_flight) {
    unsigned due = announce_auto_due(&ann_auto, ann_config(), now_s, s.alt_m, fx.valid && fx.fix_type == 3,
                                     fx.lat_e7, fx.lon_e7);
    if (due) {
      announce_input_t in;
      announce_input(&in);
      if (due & ANN_DUE_AUTO) {
        announce_trigger(TRIG_AUTO, &in, false);
      }
      if (due & ANN_DUE_ELEVATION) {
        announce_trigger(TRIG_ELEVATION, &in, false);
      }
    }
  }
}

/* In flight a short press cycles low -> med -> high -> mute -> low (stored for the next flight). */
static void next_flight_level(void)
{
  static const char *const clips[] = {"vol_1", "vol_2", "vol_3", NULL, "vol_0"}; /* index = level_t */
  level_t l = config_get()->flight_level;
  l = l == LEVEL_LOW ? LEVEL_MED : l == LEVEL_MED ? LEVEL_HIGH : l == LEVEL_HIGH ? LEVEL_MUTE : LEVEL_LOW;
  config_get()->flight_level = l;
  sound_stop();
  if (storage_voice_exists(clips[l])) {
    sound_say1(clips[l], system_voice());
  } else {
    sound_melody(l == LEVEL_MUTE ? MELODY_ERROR : MELODY_NEXT, system_piezo());
  }
  log_printf("Flight level %s", config_level_name(l));
  rawlog_event("level %s", config_level_name(l));
  config_save_flight_level(l);
}

const char *system_flight_state_name(void)
{
  return flight_state_name(flight.state);
}

/*
 * Power indicator (led_userled_int 3/4): on battery n quick green blinks every 3 s (3 full,
 * 2 medium, 1 ok), red 1 nearly empty (below the warning level), red 2 about to switch off.
 * On USB power: green breathing while charging, dim green when full.
 */
static void power_indicator(void)
{
  if (vbus_present()) {
    if (charger_line_low()) {
      indicator_breathe(LED_GREEN, 3000);
    } else {
      indicator_dim(LED_GREEN, 1);
    }
    return;
  }
  uint32_t mv = battery_mv;
  if (mv == 0U) {
    indicator_off(); /* no reading yet */
  } else if (mv <= BATTERY_CRITICAL_MV + 30U) {
    indicator_blinks(LED_RED, 2, 3000); /* shortly before the power off */
  } else if (mv <= BATTERY_LOW_MV) {
    indicator_blinks(LED_RED, 1, 3000);
  } else {
    unsigned pct = analog_battery_percent(mv);
    indicator_blinks(LED_GREEN, pct >= 70U ? 3 : pct >= 40U ? 2 : 1, 3000);
  }
}

/* The LED once the GPS has a fix: led_userled_int of the profile (config.h led_mode_t); a green
 * heartbeat when the profile shows nothing (no LED meaning, or the vario LED outside the flight). */
static void update_user_led(void)
{
  const config_t *c = config_get();
  uint8_t m = c->profile[profile - 1].led_mode;
  bool muted = in_flight && c->flight_level == LEVEL_MUTE;
  bool vario = (m == LED_MODE_VARIO || (m == LED_MODE_VARIO_MUTED && muted)) && (in_flight || vario_test);
  vario_audio_led_enable(vario);
  if (vario) {
    indicator_vario();
  } else if (m == LED_MODE_POWER || (m == LED_MODE_POWER_MUTED && muted)) {
    power_indicator();
  } else {
    indicator_heartbeat(LED_GREEN);
  }
}

static bool auth_announced;
static bool ble_was_connected;

/* "Bluetooth connected" and "Bluetooth disconnected" at the link events (bluetooth_announce_bool), on the
 * ground and in flight. The pairing confirmation by button then plays the confirm melody instead of the clip. */
static void check_ble_announce(void)
{
  ble_info_t bi;
  ble_get_info(&bi);
  if (bi.connected == ble_was_connected) {
    return;
  }
  ble_was_connected = bi.connected;
  if (config_get()->bt_announce) {
    sound_say1(bi.connected ? "bluetooth_connected" : "bluetooth_disconnected", system_voice());
  }
}

/* The button confirmed a BipLink pairing: "Bluetooth connected", unless the connection itself was announced. */
static void say_pairing_confirmed(void)
{
  if (config_get()->bt_announce) {
    sound_melody(MELODY_CONFIRM, system_piezo());
  } else {
    sound_say1("bluetooth_connected", system_voice());
  }
}

/* A short press on the ground, during the launch preparation: "ready" and "waiting for takeoff" with a 3D fix,
 * "searching GPS" before one, "GPS disabled" when the profile has no GPS. The clips ready and
 * wait_for_takeoff are extension clips the configurator's Voice view generates. */
static void say_launch_readiness(void)
{
  gps_status_t g;
  gps_get(&g);
  if (g.state == GPS_FIX) {
    static const char *const clips[] = {"ready", "wait_for_takeoff"};
    sound_say(clips, 2, system_voice());
  } else if (g.state == GPS_OFF) {
    sound_say1("gps_disabled", system_voice());
  } else {
    sound_say1("no_gps", system_voice());
  }
}

/* A new app or browser asks for access: two rising tones, then "Bluetooth, press the button to confirm"
 * (bluetooth_pairannounce_bool; the clip press_button_to_confirm is an extension clip), once; blue pulses until
 * the button or the timeout. */
static void check_auth_request(void)
{
  bool pend = biplink_auth_pending();
  if (pend && !auth_announced) {
    auth_announced = true;
    sound_melody(MELODY_PROMPT, system_piezo());
    if (config_get()->bt_pair_announce) {
      static const char *const clips[] = {"bluetooth", "press_button_to_confirm"};
      sound_say(clips, 2, system_voice());
    }
  } else if (!pend) {
    auth_announced = false;
  }
}

static void update_gps_indicator(void)
{
  check_auth_request();
  check_ble_announce();
  if (biplink_auth_pending()) {
    indicator_pulse(LED_BLUE, 100, 400);
    return;
  }
  if (faults_active) {
    indicator_blinks(LED_RED, 3, 1500); /* a hardware fault beats every other LED meaning */
    return;
  }
  gps_status_t g;
  gps_get(&g);
  if (g.state == GPS_FIX || g.state == GPS_OFF) {
    if (state == ST_ON) {
      update_user_led();
    } else {
      vario_audio_led_enable(false);
      indicator_heartbeat(LED_GREEN); /* the profile selection */
    }
  }
  if (g.state == GPS_FIX) {
    if (!baro_referenced && g.fix_type == 3 && g.hdop > 0.0f && g.hdop <= 2.5f) {
      sensors_set_reference(g.alt_m); /* barometric altitudes follow the GNSS altitude (MSL) */
      baro_referenced = true;
    }
    if (state == ST_ON && !in_flight && !gps_ok_said) {
      gps_ok_said = true;
      sound_say1("gps_ok", ground_voice());
    }
  } else if (g.state != GPS_OFF) {
    vario_audio_led_enable(false);
    indicator_count(LED_YELLOW, g.sats_good > 4 ? 4 : g.sats_good);
  }
}

/* Vario tones only in flight at the flight level; on the ground only in the console test mode. */
static void update_vario_audio(void)
{
  const config_t *c = config_get();
  uint8_t lvl = 0;
  if (state == ST_ON && in_flight) {
    lvl = c->flight_level == LEVEL_MUTE ? 0 : c->volume_buzzer[c->flight_level];
  } else if (state == ST_ON) {
    lvl = vario_test;
  }
  int idx = test_tones_idx;
  bool test = idx >= 0 && !in_flight;
  vario_audio_set(test ? &test_tones[idx] : &c->profile[profile - 1].tones, lvl);
  vario_audio_weak_lift(in_flight || (state == ST_ON && vario_test)); /* never on the ground */
}

static void handle(const event_t *ev)
{
  if (ev && ev->type == EV_OFF && state != ST_OFF && state != ST_USB) {
    shutdown();
    return;
  }
  if (ev && ev->type == EV_UPDATE) {
    do_update();
    return;
  }
  if (ev && ev->type == EV_TAP) {
    double_tap();
    return;
  }
  const button_event_t *b = (ev && ev->type == EV_BUTTON) ? &ev->button : NULL;
  if (b && b->type != BUTTON_HOLD) {
    rawlog_event("button %s %lu", b->type == BUTTON_DOWN ? "down" : "up", (unsigned long)b->ms);
  }

  /* The board resets itself after a long press: the last line before a reset shows when. */
  static uint32_t held_logged;
  if (b && b->type == BUTTON_HOLD && b->ms >= 3000U && b->ms / 1000U != held_logged) {
    held_logged = b->ms / 1000U;
    log_printf("Button held %lu s", (unsigned long)held_logged);
  } else if (b && b->type == BUTTON_UP) {
    held_logged = 0;
  }

  switch (state) {
  case ST_GESTURE:
    gesture_event(ev);
    break;
  case ST_PROFILE:
    profile_event(ev);
    update_gps_indicator();
    check_battery();
    check_faults();
    check_audio_adapter();
    check_config_update();
    break;
  case ST_ON:
    if (b && b->type == BUTTON_DOWN) {
      fresh_press_needed = false;
    } else if (b && b->type == BUTTON_HOLD && b->ms >= SHORT_PRESS_MAX_MS && !fresh_press_needed) {
      start_power_off_melody();
      break;
    } else if (b && b->type == BUTTON_UP && b->ms < SHORT_PRESS_MAX_MS && !fresh_press_needed && biplink_auth_pending()) {
      biplink_auth_confirm();
      say_pairing_confirmed();
    } else if (b && b->type == BUTTON_UP && b->ms < SHORT_PRESS_MAX_MS && !fresh_press_needed && !in_flight) {
      /* Two short presses on the ground: the takeoff by hand, for launches the detector misses (a walk
       * off a slope). Only with a 3D fix, because the recording needs positions. */
      uint32_t now = HAL_GetTick();
      if (last_short_up && now - last_short_up < DOUBLE_PRESS_MS) {
        last_short_up = 0;
        gps_status_t g;
        gps_get(&g);
        if (g.state == GPS_FIX) {
          manual_takeoff = true;
          log_printf("Takeoff requested by double press");
        } else {
          sound_say1("no_gps", system_voice()); /* no fix yet: nothing to record */
        }
      } else {
        last_short_up = now;
        say_launch_readiness();
      }
    } else if (b && b->type == BUTTON_UP && b->ms < SHORT_PRESS_MAX_MS && !fresh_press_needed && in_flight) {
      next_flight_level(); /* on release: a press that becomes a hold never changes the level */
    }
    update_gps_indicator();
    update_flight();
    update_maneuver();
    check_battery();
    check_faults();
    check_audio_adapter();
    check_config_update();
    if (state == ST_ON) {
      check_auto_off(b);
    }
    break;
  case ST_POWER_OFF:
    if (b && b->type == BUTTON_UP && HAL_GetTick() - off_started < sound_power_off_ms() * POWER_OFF_RELEASE_PCT / 100U) {
      sound_stop();
      state = before_off;
      indicator_off();
      log_printf("Power off cancelled");
    } else if (HAL_GetTick() - off_started > 300U && !sound_busy()) {
      shutdown();
      break;
    }
    update_flight(); /* the track goes on while the melody plays */
    break;
  case ST_USB:
    if (b && b->type == BUTTON_DOWN) {
      start_gesture_from_usb();
    } else if (!vbus_present()) {
      install_dropped_update(); /* returns only without a valid update */
      go_off();
    } else if (charger_line_low()) {
      indicator_breathe(LED_GREEN, 3000);
    } else {
      indicator_dim(LED_GREEN, 1); /* charged: lit, but not dazzling */
    }
    break;
  case ST_SYNC: /* handled in track_sync_window() */
  case ST_OFF:
    break;
  }

  update_vario_audio();

  if (running_since && !resets_cleared && HAL_GetTick() - running_since > STABLE_RUN_MS) {
    power_reset_count_clear();
    resets_cleared = true;
  }
}

static void boot_decision(void)
{
  bool off_after_update = backup_read(BACKUP_AFTER_UPDATE) == BACKUP_UPDATE_THEN_OFF;
  backup_write(BACKUP_AFTER_UPDATE, 0); /* only for the boot right after the hand-off */
  switch (boot.reason) {
  case BOOT_BUTTON:
    gesture_from_usb = false;
    if (boot.first_press_valid) {
      gesture_start_released(&gesture, boot.first_release_tick);
      state = ST_GESTURE;
    } else {
      log_printf("Power-on gesture: first press too long - back to standby");
      go_off();
    }
    break;
  case BOOT_RESUME: {
    uint32_t n = power_reset_count_increment();
    log_printf("Unexpected reset while switched on (%lu in a row)", (unsigned long)n);
    if (n >= 3) {
      sound_melody(MELODY_ERROR, ground_piezo());
      vTaskDelay(pdMS_TO_TICKS(1200));
      power_reset_count_clear();
      go_off();
    } else {
      power_on(false, false);
    }
    break;
  }
  case BOOT_UPDATE:
    if (off_after_update) {
      announce_update_and_go_off();
    } else {
      power_on(true, true);
    }
    break;
  default:
    /* Includes the board's long-press reset and leaving the bootloader's recovery: stay off. */
    power_set_running(false);
    if (vbus_present()) {
      enter_usb();
    } else {
      go_off();
    }
    break;
  }
}

static void system_task(void *arg)
{
  (void)arg;
  event_t ev;
  boot_decision();
  for (;;) {
    health_alive(HEALTH_SYSTEM);
    bool got = xQueueReceive(events, &ev, pdMS_TO_TICKS(TICK_MS)) == pdTRUE;
    handle(got ? &ev : NULL);
    if (state == ST_ON) {
      announce_drain();
    }
  }
}

void system_start(const system_boot_t *b)
{
  boot = *b;
  config_defaults(config_get());
  events = xQueueCreate(16, sizeof(event_t));
  button_init(on_button, b->reason == BOOT_BUTTON && button_pressed() ? HAL_GetTick() : 0U);
  sensors_set_tap_handler(on_double_tap);
  xTaskCreate(system_task, "system", 1024, NULL, 3, NULL); /* FatFs: IGC files, config */
}

void system_request_power_off(void)
{
  event_t ev = {EV_OFF, {BUTTON_UP, 0}};
  xQueueSend(events, &ev, 0);
}

void system_request_update(void)
{
  event_t ev = {EV_UPDATE, {BUTTON_UP, 0}};
  xQueueSend(events, &ev, 0);
}

void system_vario_test(uint8_t level)
{
  vario_test = level;
  rawlog_event("vario test %u", level);
}

/* ---- tests from the configurator (Bluetooth), only on the ground ---- */

bool system_test_allowed(void)
{
  return (state == ST_ON || state == ST_PROFILE) && !in_flight;
}

void system_test_tones(const vario_tones_t *t)
{
  int next = test_tones_idx == 0 ? 1 : 0;
  test_tones[next] = *t;
  test_tones_idx = next;
}

void system_test_stop(void)
{
  test_tones_idx = -1;
  vario_test = 0;
  vario_audio_simulate(false, 0.0f);
}

uint32_t system_battery_mv(void)
{
  return battery_mv;
}

uint32_t system_local_unix(uint32_t utc)
{
  const config_t *c = config_get();
  const profile_t *p = &c->profile[profile - 1];
  return utc + (uint32_t)(announce_utc_offset_min(utc, p->time_offset_min, p->tz_auto, c->dst_rule, &c->tz_table) * 60);
}

uint32_t system_double_taps(void)
{
  return double_taps;
}

/* A second Bluetooth link (the configurator next to the phone) only on the ground: switched on and not in
 * flight, in the profile selection, or in the track sync window after switching off. */
bool system_second_link_allowed(void)
{
  return state == ST_SYNC || state == ST_PROFILE || (state == ST_ON && !in_flight);
}

bool system_in_flight(void)
{
  return in_flight;
}
