#include "console.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "analog.h"
#include "audio.h"
#include "backup.h"
#include "ble_app.h"
#include "ble_console.h"
#include "board.h"
#include "config.h"
#include "dump.h"
#include "gps.h"
#include "health.h"
#include "log.h"
#include "power.h"
#include "rawlog.h"
#include "recorder.h"
#include "rtc_time.h"
#include "sensors.h"
#include "sound.h"
#include "storage.h"
#include "system.h"
#include "uart_console.h"
#include "usb_device.h"
#include "version.h"

#define POLL_MS      20U
#define IDLE_POLL_MS 100U /* nothing connected: the cable and the Bluetooth console are still noticed in time */

typedef enum { SOURCE_USB, SOURCE_UART, SOURCE_BLE } source_t;

static volatile bool drive_active;
static char line[96];
static size_t line_len;
static source_t line_source; /* where the line came from: 'log' replays there */

bool console_usb_drive_active(void)
{
  return drive_active;
}

/* ---- commands ---- */

static void cmd_help(void)
{
  static const char text[] = "Commands:\r\n"
                 "  status                      device, battery, GPS summary\r\n"
                 "  gps                         GPS details and start milestones\r\n"
                 "  gps start [auto|cold|time]  power-cycle the GPS with this start mode\r\n"
                 "  gps rate 1|5                fixes per second, restarts the GPS (default 5)\r\n"
                 "  gps systems all|gg|gps      satellite systems: all, GPS+GLONASS, GPS only; restarts the GPS\r\n"
                 "  gps stop                    GPS off\r\n"
                 "  gps sats                    satellites heard, signal strength (C/N0) of each\r\n"
                 "  gps monitor on|off          show every line from the GPS module\r\n"
                 "  gps send <@CMD ...>         raw command to the GPS module\r\n"
                 "  gps baud <n>                listen at another baud rate (diagnostics, until the next start)\r\n"
                 "  gps probe                   read the RX line with a pull-up and a pull-down (floating = no module)\r\n"
                 "  gps blast <s>               send 0x55 for s seconds: find the TX line to the module with a meter\r\n"
                 "  gps boot [baud] [s]         power-cycle the module and look for its boot loader prompt (hex dump)\r\n"
                 "  gps boot 0                  power-cycle the module and scan the rates at which it talks\r\n"
                 "  rtc                         RTC time and last GPS synchronisation\r\n"
                 "  profiles                    profiles from the configuration\r\n"
                 "  sensors                     barometer, altitude, climb rate, load\r\n"
                 "  sensors monitor on|off      log them once per second\r\n"
                 "  baro rate 32|64|100         barometer samples per second (new raw log file)\r\n"
                 "  imu cal                     accelerometer calibration and the last 1 s mean\r\n"
                 "  vario test on [vol]|off     vario tones on the ground, volume 1-100 % (20)\r\n"
                 "  piezo [classic|pwm]         piezo drive (pwm: experimental, until power off)\r\n"
                 "  piezo pulse <ns>            pulse of the step \"very quiet\" (until power off)\r\n"
                 "  rawlog                      raw data log: file, size, losses\r\n"
                 "  tasks                       stack reserve of each task, the heap's low point (RAM headroom)\r\n"
                 "  say <clip>                  play 1:/voices/<clip>.qoa\r\n"
                 "  melody <0-5>                play a piezo melody\r\n"
                 "  log                         replay the log\r\n"
                 "  dump                        stock bootloader, OTP, option bytes, chip data to 0:/ultrabeep/dump/\r\n"
                 "  update                      install ultrabip*.update from the SD card root\r\n"
                 "  off                         switch off (like the button)\r\n"
                 "  reboot                      restart (resumes in the current state)\r\n";
  console_write(text, sizeof(text) - 1U); /* longer than console_printf's buffer */
}

/* The least free stack of each task since its start and the heap's low point: the RAM that could be given back. */
static void cmd_tasks(void)
{
  TaskStatus_t st[16];
  UBaseType_t n = uxTaskGetSystemState(st, sizeof(st) / sizeof(st[0]), NULL);
  for (UBaseType_t i = 0; i < n; i++) {
    console_printf("  %-10s stack reserve %4lu bytes\r\n", st[i].pcTaskName,
                   (unsigned long)(st[i].usStackHighWaterMark * sizeof(StackType_t)));
  }
  console_printf("  %-10s stack reserve %4lu bytes (main stack: interrupts)\r\n", "ISR",
                 (unsigned long)board_isr_stack_reserve());
  console_printf("heap: %u bytes free, %u at the lowest, of %u\r\n", (unsigned)xPortGetFreeHeapSize(),
                 (unsigned)xPortGetMinimumEverFreeHeapSize(), (unsigned)configTOTAL_HEAP_SIZE);
}

static void cmd_rawlog(void)
{
  rawlog_status_t st;
  rawlog_get_status(&st);
  if (!st.active) {
    console_printf("raw log off (it records while the device is switched on, system_rawlog_bool)\r\n");
    return;
  }
  console_printf("raw log %s: %lu bytes written, %lu records dropped\r\n", st.file, (unsigned long)st.bytes,
                 (unsigned long)st.dropped);
}

static void cmd_sensors(const char *args)
{
  if (strncmp(args, "monitor ", 8) == 0) {
    sensors_monitor(strcmp(args + 8, "on") == 0);
    return;
  }
  sensors_t s;
  sensors_get(&s);
  if (!s.on) {
    console_printf("sensors off (they run while the device is switched on)\r\n");
    return;
  }
  console_printf("  barometer %s (id %02X, expected 10), IMU %s (id %02X, expected 42)\r\n",
                 s.baro_ok ? "ok" : "NO ANSWER", s.baro_id, s.imu_ok ? "ok" : "NO ANSWER", s.imu_id);
  if (!s.samples) {
    console_printf("  no barometer samples yet\r\n");
    return;
  }
  console_printf("  pressure %.1f Pa, temperature %.1f C, %lu samples%s\r\n", (double)s.pressure_pa,
                 (double)s.temp_c, (unsigned long)s.samples, s.valid ? "" : " (settling)");
  console_printf("  pressure altitude %.1f m (1013.25 hPa)\r\n", (double)s.alt_std_m);
  console_printf("  altitude %.1f m (raw %.1f m), %s\r\n", (double)s.alt_m, (double)s.alt_raw_m,
                 s.referenced ? "referenced to GNSS" : "not referenced yet (standard atmosphere)");
  console_printf("  climb rate %+.2f m/s, acceleration %.2f m/s2\r\n", (double)s.vario_ms, (double)s.accel_ms2);
}

static void print_milestone(const char *name, uint32_t ms)
{
  if (ms) {
    console_printf("  %-22s %6lu.%01lu s\r\n", name, (unsigned long)(ms / 1000U), (unsigned long)(ms % 1000U / 100U));
  } else {
    console_printf("  %-22s      -\r\n", name);
  }
}

static void cmd_gps_details(void)
{
  gps_status_t g;
  gps_get(&g);
  console_printf("GPS %s, start %s, time %s, %lu lines, %lu bad checksums, systems %s\r\n", gps_state_name(g.state),
                 gps_start_name(g.start_mode), g.time_injected ? "given" : "not given", (unsigned long)g.lines,
                 (unsigned long)g.bad_checksums, g.systems ? g.systems : "-");
  console_printf("  %u fixes per second, %lu baud, %lu bytes received (%lu framing errors), %lu bytes lost in the UART, "
                 "%lu placeholder epochs skipped, %lu restarts\r\n", g.cycle_ms ? 1000U / g.cycle_ms : 0U,
                 (unsigned long)g.baud, (unsigned long)g.rx_bytes, (unsigned long)g.rx_errors, (unsigned long)g.overruns,
                 (unsigned long)g.placeholders, (unsigned long)g.restarts);
  console_printf("  fix type %u, quality %u, used %u, in view %u, good (>=30 dB-Hz) %u, HDOP %.1f\r\n", g.fix_type,
                 g.quality, g.sats_used, g.sats_view, g.sats_good, (double)g.hdop);
  if (g.quality) {
    console_printf("  %.6f %.6f, %.1f m, %.1f km/h, %.0f deg\r\n", g.lat_e7 / 1e7, g.lon_e7 / 1e7, (double)g.alt_m,
                   (double)g.speed_kmh, (double)g.course_deg);
  }
  gps_fix_t fx;
  gps_get_fix(&fx);
  if (fx.seq) {
    console_printf("  last fix #%lu: %s, %lu.%03u UTC, %.1f m MSL + geoid %.1f m, velocity N %+.2f E %+.2f D %+.2f m/s%s\r\n",
                   (unsigned long)fx.seq, fx.valid ? "valid" : "invalid", (unsigned long)fx.unix_time, fx.ms,
                   (double)fx.alt_msl_m, (double)fx.geoid_sep_m, (double)fx.vel_n_ms, (double)fx.vel_e_ms,
                   (double)fx.vel_d_ms, fx.vel_d_valid ? "" : " (D unknown)");
  }
  console_printf("Milestones after power on:\r\n");
  print_milestone("first NMEA", g.t_first_nmea);
  print_milestone("first good satellite", g.t_first_signal);
  print_milestone("4 good satellites", g.t_four_good);
  print_milestone("UTC time", g.t_time);
  print_milestone("2D fix", g.t_fix2d);
  print_milestone("3D fix (TTFF)", g.t_fix3d);
  if (g.state != GPS_OFF) {
    print_milestone("since power on", HAL_GetTick() - g.power_on_tick);
  }
}

static void cmd_gps(char *args)
{
  char *sub = strtok(args, " ");
  if (!sub) {
    cmd_gps_details();
  } else if (strcmp(sub, "start") == 0) {
    char *mode = strtok(NULL, " ");
    gps_start_t m = GPS_START_AUTO;
    if (mode && strcmp(mode, "cold") == 0) {
      m = GPS_START_COLD;
    } else if (mode && strcmp(mode, "time") == 0) {
      m = GPS_START_TIME;
    }
    gps_start(m);
  } else if (strcmp(sub, "rate") == 0) {
    char *hz = strtok(NULL, " ");
    gps_set_rate(hz ? (unsigned)atoi(hz) : 5U);
    gps_start(GPS_START_AUTO);
  } else if (strcmp(sub, "systems") == 0) {
    char *which = strtok(NULL, " ");
    gps_set_systems(which && strcmp(which, "gg") == 0 ? GPS_SYSTEMS_GPS_GLONASS
                    : which && strcmp(which, "gps") == 0 ? GPS_SYSTEMS_GPS : GPS_SYSTEMS_ALL);
    gps_start(GPS_START_AUTO);
  } else if (strcmp(sub, "stop") == 0) {
    gps_stop();
  } else if (strcmp(sub, "sats") == 0) {
    gps_print_sats();
  } else if (strcmp(sub, "monitor") == 0) {
    char *on = strtok(NULL, " ");
    gps_set_monitor(on && strcmp(on, "on") == 0);
  } else if (strcmp(sub, "send") == 0) {
    char *cmd = strtok(NULL, "");
    if (cmd) {
      gps_send(cmd);
    }
  } else if (strcmp(sub, "baud") == 0) {
    char *n = strtok(NULL, " ");
    gps_set_baud(n ? (uint32_t)atol(n) : 115200U);
  } else if (strcmp(sub, "blast") == 0) {
    char *n = strtok(NULL, " ");
    unsigned s = n ? (unsigned)atoi(n) : 10U;
    console_printf("sending 0x55 for %u s\r\n", s > 120U ? 120U : s);
    gps_blast(s > 120U ? 120U : s);
    console_printf("done\r\n");
  } else if (strcmp(sub, "boot") == 0) {
    char *b = strtok(NULL, " ");
    char *s = strtok(NULL, " ");
    gps_boot_probe(b ? (uint32_t)atol(b) : 115200U, s ? (unsigned)atoi(s) : 8U);
  } else if (strcmp(sub, "probe") == 0) {
    bool up, down;
    gps_probe_rx(&up, &down);
    console_printf("GPS RX line: %d with pull-up, %d with pull-down - %s\r\n", up, down,
                   up && !down ? "floating: nothing drives it (module unpowered or not connected)"
                   : up && down ? "driven high: the module is powered and idle"
                   : !up && !down ? "driven low: the module holds the line down (reset, no supply?)" : "?");
  } else {
    console_printf("unknown gps command\r\n");
  }
}

static void print_unix(const char *label, uint32_t t)
{
  uint16_t y;
  uint8_t mo, d, h, mi, s;
  civil_from_unix(t, &y, &mo, &d, &h, &mi, &s);
  console_printf("%s %04u-%02u-%02u %02u:%02u:%02u UTC\r\n", label, y, mo, d, h, mi, s);
}

static void cmd_rtc(void)
{
  if (!rtc_time_valid()) {
    console_printf("RTC: never set from GPS\r\n");
    return;
  }
  uint32_t now = rtc_time_unix();
  uint32_t last = rtc_time_last_sync();
  print_unix("RTC now      ", now);
  print_unix("last GPS sync", last);
  console_printf("age %lu s (the GPS gets the time if < 3600 s and the rate error is known and < %d ppm)\r\n",
                 (unsigned long)(now - last), RTC_TRUST_PPM);
  uint32_t pa, ps;
  rtc_time_prescalers(&pa, &ps);
  int32_t ppm = 0;
  if (rtc_time_drift(&ppm)) {
    console_printf("clock %s, prescalers %lu/%lu, rate error at the last sync %ld ppm\r\n", rtc_time_source_name(),
                   (unsigned long)pa, (unsigned long)ps, (long)ppm);
  } else {
    console_printf("clock %s, prescalers %lu/%lu, rate error not measured yet\r\n", rtc_time_source_name(),
                   (unsigned long)pa, (unsigned long)ps);
  }
}

static void cmd_status(void)
{
  gps_status_t g;
  gps_get(&g);
  analog_values_t a;
  bool adc = analog_measure(&a);
  console_printf("%s %s, uptime %lu s, state %s (%s), profile %u\r\n", FW_NAME, FW_VERSION,
                 (unsigned long)(HAL_GetTick() / 1000U), system_state_name(), system_flight_state_name(),
                 system_profile());
  if (adc) {
    console_printf("Battery %lu mV (%u %%), solar %lu mV, USB %d, charger line %s\r\n", (unsigned long)a.vbat_mv,
                   a.battery_percent, (unsigned long)a.solar_mv, vbus_present(), charger_line_low() ? "low" : "high");
  }
  console_printf("GPS %s, good satellites %u, used %u, TTFF %lu ms\r\n", gps_state_name(g.state), g.sats_good,
                 g.sats_used, (unsigned long)g.t_fix3d);
  sensors_t sn;
  sensors_get(&sn);
  console_printf("Sensors: barometer %s, IMU %s, GNSS module %s (%lu sentences, %lu restarts), SD card %s, voice pack %s\r\n",
                 !sn.on ? "off" : sn.baro_ok ? "ok" : "NO ANSWER", !sn.on ? "off" : sn.imu_ok ? "ok" : "NO ANSWER",
                 g.state == GPS_OFF ? "off" : g.alive ? "talking" : "SILENT", (unsigned long)g.lines,
                 (unsigned long)g.restarts, storage_is_mounted() ? "ok" : "NOT MOUNTED",
                 storage_voice_pack_ready() ? "ok" : "MISSING");
  recorder_status_t rec;
  recorder_get_status(&rec);
  if (rec.active) {
    console_printf("IGC %s: %lu records, %lu bytes%s\r\n", rec.file, (unsigned long)rec.samples,
                   (unsigned long)rec.bytes, rec.signed_file ? ", signed" : ", unsigned (no key)");
  } else {
    console_printf("IGC: not recording (starts with the takeoff)%s\r\n", rec.file[0] ? ", last file closed" : "");
  }
  ble_info_t b;
  ble_get_info(&b);
  console_printf("Bluetooth %s%s, stack %u.%u.%u, FUS %u.%u.%u, MTU %u\r\n", b.running ? "on as " : "off",
                 b.running ? ble_name() : "", b.fw_major, b.fw_minor, b.fw_sub, b.fus_major, b.fus_minor, b.fus_sub,
                 b.mtu);
  console_printf("SD card %s, voice pack %s, free heap %u bytes\r\n", storage_is_mounted() ? "mounted" : "not mounted",
                 storage_voice_pack_ready() ? "ok" : "missing", (unsigned)xPortGetFreeHeapSize());
}

static void cmd_profiles(void)
{
  const config_t *c = config_get();
  for (int i = 0; i < PROFILE_MAX; i++) {
    const profile_t *p = &c->profile[i];
    if (p->present) {
      console_printf("  %2d %-8s %-14s%s\r\n", i + 1, p->enabled ? "enabled" : "disabled", p->type,
                     i + 1 == c->profile_last ? " (last used)" : "");
    }
  }
}

static void execute(char *cmd)
{
  while (*cmd == ' ') {
    cmd++;
  }
  char *args = strchr(cmd, ' ');
  if (args) {
    *args++ = 0;
  } else {
    args = cmd + strlen(cmd);
  }
  if (!cmd[0]) {
    return;
  }
  if (strcmp(cmd, "help") == 0) {
    cmd_help();
  } else if (strcmp(cmd, "status") == 0) {
    cmd_status();
  } else if (strcmp(cmd, "gps") == 0) {
    cmd_gps(args);
  } else if (strcmp(cmd, "rtc") == 0) {
    cmd_rtc();
  } else if (strcmp(cmd, "profiles") == 0) {
    cmd_profiles();
  } else if (strcmp(cmd, "sensors") == 0) {
    cmd_sensors(args);
  } else if (strcmp(cmd, "vario") == 0 && strncmp(args, "test ", 5) == 0) {
    bool on = strncmp(args + 5, "on", 2) == 0;
    int level = on && args[7] == ' ' ? atoi(args + 8) : 20;
    level = level < 1 ? 1 : level > 100 ? 100 : level;
    system_vario_test(on ? (uint8_t)level : 0);
    if (on) {
      console_printf("vario test on, volume %d %%\r\n", level);
    } else {
      console_printf("vario test off\r\n");
    }
  } else if (strcmp(cmd, "piezo") == 0) {
    if (strcmp(args, "pwm") == 0 || strcmp(args, "classic") == 0) {
      piezo_set_drive(args[0] == 'p' ? PIEZO_DRIVE_PWM : PIEZO_DRIVE_CLASSIC);
    } else if (strncmp(args, "pulse ", 6) == 0) {
      sound_set_quiet_pulse((uint32_t)atoi(args + 6));
    }
    console_printf("piezo drive %s, very quiet pulse %lu ns\r\n", piezo_get_drive() == PIEZO_DRIVE_PWM ? "pwm" : "classic",
                   (unsigned long)sound_quiet_pulse());
  } else if (strcmp(cmd, "baro") == 0 && strncmp(args, "rate ", 5) == 0) {
    unsigned hz = (unsigned)atoi(args + 5);
    rawlog_status_t st;
    rawlog_get_status(&st);
    if (st.active) {
      rawlog_stop(); /* the header names the rate: a new file */
    }
    sensors_set_baro_rate(hz);
    if (st.active) {
      rawlog_start();
    }
    console_printf("barometer: %u Hz (until power off; system_barorate_int in config_ext.ultrabip keeps it)\r\n",
                   sensors_baro_rate());
  } else if (strcmp(cmd, "imu") == 0 && strcmp(args, "cal") == 0) {
    float o[3], s[3];
    sensors_cal_sample_t c;
    sensors_accel_calibration(o, s);
    sensors_cal_sample(&c);
    console_printf("offset %.3f %.3f %.3f m/s2, scale %.5f %.5f %.5f\r\n", (double)o[0], (double)o[1], (double)o[2],
                   (double)s[0], (double)s[1], (double)s[2]);
    console_printf("last second (raw): %.3f %.3f %.3f m/s2, |a| %.3f, %s\r\n", (double)c.mean[0], (double)c.mean[1],
                   (double)c.mean[2], (double)sqrtf(c.mean[0] * c.mean[0] + c.mean[1] * c.mean[1] + c.mean[2] * c.mean[2]),
                   c.still ? "still" : "moving");
  } else if (strcmp(cmd, "rawlog") == 0) {
    cmd_rawlog();
  } else if (strcmp(cmd, "tasks") == 0) {
    cmd_tasks();
  } else if (strcmp(cmd, "say") == 0 && args[0]) {
    sound_say1(args, 70);
  } else if (strcmp(cmd, "melody") == 0) {
    sound_melody((melody_t)atoi(args), 60);
  } else if (strcmp(cmd, "log") == 0) {
    if (line_source == SOURCE_BLE) {
      log_replay_ble();
    } else if (line_source == SOURCE_UART) {
      log_replay_uart();
    } else {
      log_replay();
    }
  } else if (strcmp(cmd, "dump") == 0) {
    (void)dump_device();
  } else if (strcmp(cmd, "update") == 0) {
    system_request_update();
  } else if (strcmp(cmd, "off") == 0) {
    system_request_power_off();
  } else if (strcmp(cmd, "reboot") == 0) {
    console_printf("restarting\r\n");
    vTaskDelay(pdMS_TO_TICKS(100));
    NVIC_SystemReset();
  } else {
    console_printf("unknown command '%s' - try help\r\n", cmd);
  }
}

/* ---- USB and input ---- */

static void follow_usb(void)
{
  bool vbus = vbus_present();
  if (vbus && !usb_is_started()) {
    usb_start(false);
  } else if (!vbus && usb_is_started()) {
    usb_stop();
    drive_active = false;
  }
  if (!usb_is_started()) {
    drive_active = false;
    return;
  }
  bool want = system_wants_usb_drive();
  if (want != drive_active) {
    usb_msc_set_ready(want);
    drive_active = want;
  }
}

/* The next typed character from USB, the serial pins or Bluetooth, or -1. */
static int next_char(void)
{
  int c;
  if ((c = usb_cdc_getc()) >= 0) {
    line_source = SOURCE_USB;
  } else if ((c = uart_console_getc()) >= 0) {
    line_source = SOURCE_UART;
  } else if ((c = ble_console_getc()) >= 0) {
    line_source = SOURCE_BLE;
  }
  return c;
}

/* After CONSOLE on: the greeting of the USB console, on Bluetooth only. */
static void greet_ble(void)
{
  char s[64];
  int n = snprintf(s, sizeof(s), "%s %s - log so far:\r\n", FW_NAME, FW_VERSION);
  ble_console_write(s, n > 0 ? (size_t)n : 0U);
  log_replay_ble();
  static const char hint[] = "Type 'help' for commands.\r\n> ";
  ble_console_write(hint, sizeof(hint) - 1U);
}

static void console_task(void *arg)
{
  (void)arg;
  bool was_connected = false;
  ble_console_patient_task(HEALTH_CONSOLE);
  for (;;) {
    health_alive(HEALTH_CONSOLE);
    follow_usb();
    bool connected = usb_cdc_connected();
    if (connected && !was_connected) {
      vTaskDelay(pdMS_TO_TICKS(100));
      console_printf("\r\n%s %s - log so far:\r\n", FW_NAME, FW_VERSION);
      log_replay();
      console_printf("Type 'help' for commands.\r\n> ");
      line_len = 0;
    }
    was_connected = connected;
    if (ble_console_greeting()) {
      greet_ble();
    }

    int c;
    while ((c = next_char()) >= 0) {
      if (c == '\r' || c == '\n') {
        if (line_len) {
          console_printf("\r\n");
          line[line_len] = 0;
          execute(line);
          line_len = 0;
          console_printf("> ");
        }
      } else if (c == 0x08 || c == 0x7F) {
        if (line_len) {
          line_len--;
          console_printf("\b \b");
        }
      } else if (c >= 0x20 && line_len < sizeof(line) - 1U) {
        line[line_len++] = (char)c;
        char echo = (char)c;
        console_write(&echo, 1);
      }
    }
    bool idle = !connected && !uart_console_enabled() && !ble_console_active();
    vTaskDelay(pdMS_TO_TICKS(idle ? IDLE_POLL_MS : POLL_MS));
  }
}

void console_init(void)
{
  xTaskCreate(console_task, "console", 768, NULL, 2, NULL);
}
