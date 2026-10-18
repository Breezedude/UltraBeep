/*
 * Tests for hardware-independent code, compiled for Cortex-M4 and executed in
 * the Unicorn emulator by run_logic_tests.py (no host compiler needed).
 */
#include <stdio.h>
#include <string.h>

#include <math.h>

#include "announce.h"
#include "battery.h"
#include "config.h"
#include "console_pack.h"
#include "crc32.h"
#include "flight.h"
#include "fusion.h"
#include "gesture.h"
#include "identity.h"
#include "igc.h"
#include "ini.h"
#include "kml.h"
#include "nmea.h"
#include "recorder.h"
#include "rtc_time.h"
#include "sha256.h"
#include "summary.h"
#include "tap.h"
#include "thermal.h"
#include "maneuver.h"
#include "tilt.h"
#include "qoa.h"
#include "qoa_ref.h"
#include "vario.h"

static char report[4096];
static size_t report_len;
static int passed;
static int failed;

#define CHECK(cond, ...)                                                               \
  do {                                                                                 \
    if (cond) {                                                                        \
      passed++;                                                                        \
    } else {                                                                           \
      failed++;                                                                        \
      report_len += (size_t)snprintf(report + report_len, sizeof(report) - report_len, \
                                     "FAIL line %d: ", __LINE__);                      \
      report_len += (size_t)snprintf(report + report_len, sizeof(report) - report_len, \
                                     __VA_ARGS__);                                     \
      report_len += (size_t)snprintf(report + report_len, sizeof(report) - report_len, "\n"); \
    }                                                                                  \
  } while (0)

/* ---- ini ---- */

typedef struct {
  char found[64];
  int count;
} lookup_t;

static const char *want_section;
static const char *want_key;

static bool collect(void *ctx, const char *section, const char *key, const char *value)
{
  lookup_t *l = (lookup_t *)ctx;
  if (strcmp(section, want_section) == 0 && strcmp(key, want_key) == 0) {
    snprintf(l->found, sizeof(l->found), "%s", value);
    l->count++;
  }
  return true;
}

static const char *get(const char *text, const char *section, const char *key, int *count)
{
  static lookup_t l;
  memset(&l, 0, sizeof(l));
  want_section = section;
  want_key = key;
  ini_parse(text, collect, &l);
  *count = l.count;
  return l.found;
}

static void test_ini(void)
{
  static char buf[512];
  int n;

  /* new file */
  buf[0] = 0;
  CHECK(ini_set(buf, sizeof(buf), "global", "system_profilelast_int", "3"), "set in empty buffer");
  CHECK(strcmp(buf, "[global]\nsystem_profilelast_int=3\n") == 0, "empty buffer result: '%s'", buf);

  /* replace a value, keep everything else (CRLF file) */
  strcpy(buf, "# comment\r\n[profile1]\r\nenabled_bool=true\r\n\r\n[global]\r\nsystem_profilelast_int=1\r\n"
              "other_int=5\r\n");
  CHECK(ini_set(buf, sizeof(buf), "global", "system_profilelast_int", "10"), "replace");
  CHECK(strcmp(buf, "# comment\r\n[profile1]\r\nenabled_bool=true\r\n\r\n[global]\r\nsystem_profilelast_int=10\r\n"
                    "other_int=5\r\n") == 0,
        "replace result: '%s'", buf);

  /* add a key to a section that is followed by another section */
  strcpy(buf, "[global]\r\na_int=1\r\n\r\n[profile4]\r\nenabled_bool=true\r\n");
  CHECK(ini_set(buf, sizeof(buf), "global", "b_int", "2"), "insert");
  CHECK(strcmp(buf, "[global]\r\na_int=1\r\nb_int=2\r\n\r\n[profile4]\r\nenabled_bool=true\r\n") == 0,
        "insert result: '%s'", buf);
  CHECK(strcmp(get(buf, "global", "b_int", &n), "2") == 0 && n == 1, "parse inserted key");
  CHECK(strcmp(get(buf, "profile4", "enabled_bool", &n), "true") == 0 && n == 1, "parse other section");

  /* key with the same prefix must not match */
  strcpy(buf, "[global]\nvolume_int=1\nvolume_intx=7\n");
  CHECK(ini_set(buf, sizeof(buf), "global", "volume_intx", "8"), "prefix key");
  CHECK(strcmp(buf, "[global]\nvolume_int=1\nvolume_intx=8\n") == 0, "prefix result: '%s'", buf);

  /* missing section is appended; file without final line end */
  strcpy(buf, "[profile1]\nenabled_bool=true");
  CHECK(ini_set(buf, sizeof(buf), "global", "x_int", "4"), "append section");
  CHECK(strcmp(buf, "[profile1]\nenabled_bool=true\n\n[global]\nx_int=4\n") == 0, "append result: '%s'", buf);

  /* same key in another section stays untouched */
  strcpy(buf, "[profile1]\nenabled_bool=false\n[profile2]\nenabled_bool=false\n");
  CHECK(ini_set(buf, sizeof(buf), "profile2", "enabled_bool", "true"), "section scope");
  CHECK(strcmp(buf, "[profile1]\nenabled_bool=false\n[profile2]\nenabled_bool=true\n") == 0, "scope result: '%s'",
        buf);

  /* spaces around '=' and comments are tolerated */
  strcpy(buf, "; c\n[ global ]\n  key_int = 42  \n# x=1\n");
  CHECK(strcmp(get(buf, " global ", "key_int", &n), "42") == 0 || strcmp(get(buf, "global", "key_int", &n), "42") == 0,
        "spaces");

  /* buffer too small: refused, unchanged */
  char small[24] = "[g]\na=1\n";
  CHECK(!ini_set(small, sizeof(small), "g", "a", "0123456789012345678"), "overflow refused");
  CHECK(strcmp(small, "[g]\na=1\n") == 0, "overflow unchanged: '%s'", small);

  CHECK(ini_parse_bool("true") && ini_parse_bool("1") && !ini_parse_bool("false") && !ini_parse_bool("0"), "bool");
}

/* ---- time ---- */

static void test_time(void)
{
  /* RTC correction: 1 h counted by an RTC 1 % fast is 3564 s; a stopped RTC (-29 %) is not trusted */
  uint32_t est = 0, age = 0;
  CHECK(rtc_drift_estimate(1000000U + 3600U, 1000000U, 10000, &est, &age) && age == 3564U && est == 1000000U + 3564U,
        "rtc 1 %% fast: age %lu", (unsigned long)age);
  CHECK(rtc_drift_estimate(1000000U + 3600U, 1000000U, -5000, &est, &age) && age == 3618U, "rtc slow: age %lu",
        (unsigned long)age);
  CHECK(!rtc_drift_estimate(1000000U + 3600U, 1000000U, -294636, &est, &age), "rtc rate beyond 2 %% untrusted");
  CHECK(!rtc_drift_estimate(1000000U, 1000000U + 5U, 0, &est, &age), "rtc behind the last sync untrusted");
  CHECK(unix_from_civil(1970, 1, 1, 0, 0, 0) == 0U, "epoch");
  CHECK(unix_from_civil(2026, 10, 7, 9, 30, 0) == 1791365400U, "2026-10-07 09:30:00 = %lu",
        (unsigned long)unix_from_civil(2026, 10, 7, 9, 30, 0));
  CHECK(unix_from_civil(2024, 2, 29, 23, 59, 59) == 1709251199U, "leap day");
  uint16_t y;
  uint8_t mo, d, h, mi, s;
  civil_from_unix(1791365400U, &y, &mo, &d, &h, &mi, &s);
  CHECK(y == 2026 && mo == 10 && d == 7 && h == 9 && mi == 30 && s == 0, "round trip %u-%u-%u %u:%u:%u", y, mo, d,
        h, mi, s);
  civil_from_unix(1709251199U, &y, &mo, &d, &h, &mi, &s);
  CHECK(y == 2024 && mo == 2 && d == 29 && h == 23 && mi == 59 && s == 59, "leap round trip");
  civil_from_unix(4102444800U, &y, &mo, &d, &h, &mi, &s); /* 2100-01-01 */
  CHECK(y == 2100 && mo == 1 && d == 1, "2100: %u-%u-%u", y, mo, d);
}

static void test_crc(void)
{
  CHECK(crc32("123456789", 9) == 0xCBF43926U, "crc32 check value 0x%08lX", (unsigned long)crc32("123456789", 9));
  CHECK(crc32("", 0) == 0U, "crc32 of nothing");
}

/* ---- power-on gesture ---- */

typedef struct {
  uint32_t down, up; /* ms relative to the start; held in [down, up) */
} press_t;

/*
 * Feeds the gesture like the system task does: DOWN/UP at the edges, HOLD every
 * 100 ms while held, a tick every 50 ms. Times are relative to `base` (tests the
 * tick wrap-around). Returns the result; *at = relative time of the result.
 */
static gesture_result_t simulate(gesture_t *g, uint32_t base, const press_t *presses, int n, uint32_t *at)
{
  for (uint32_t rel = 0; rel < 10000U; rel += 10U) {
    bool pressed = false;
    button_event_t e;
    const button_event_t *ev = NULL;
    for (int i = 0; i < n; i++) {
      if (rel == presses[i].down) {
        e = (button_event_t){BUTTON_DOWN, 0};
        ev = &e;
      } else if (rel == presses[i].up) {
        e = (button_event_t){BUTTON_UP, presses[i].up - presses[i].down};
        ev = &e;
      } else if (rel > presses[i].down && rel < presses[i].up && (rel - presses[i].down) % 100U == 0) {
        e = (button_event_t){BUTTON_HOLD, rel - presses[i].down};
        ev = &e;
      }
      pressed |= rel >= presses[i].down && rel < presses[i].up;
    }
    if (ev || rel % 50U == 0) {
      gesture_result_t r = gesture_step(g, base + rel, ev, pressed);
      if (r != GESTURE_CONTINUE) {
        *at = rel;
        return r;
      }
    }
  }
  *at = 0;
  return GESTURE_CONTINUE;
}

static void test_gesture(void)
{
  gesture_t g;
  uint32_t at;
  gesture_result_t r;
  static const uint32_t bases[] = {0U, 0xFFFFFC00U}; /* normal and across the tick wrap-around */

  for (int i = 0; i < 2; i++) {
    uint32_t base = bases[i];

    /* Woken from standby: the wake-up press was released at 0. */
    gesture_start_released(&g, base);
    r = simulate(&g, base, (const press_t[]){{300, 1500}}, 1, &at);
    CHECK(r == GESTURE_POWER_ON && at >= 800 && at <= 850, "long second press: result %d at %lu", r, (unsigned long)at);

    gesture_start_released(&g, base);
    r = simulate(&g, base, NULL, 0, &at);
    CHECK(r == GESTURE_ABORT && at > 1000 && at <= 1050, "no second press: result %d at %lu", r, (unsigned long)at);

    gesture_start_released(&g, base);
    r = simulate(&g, base, (const press_t[]){{300, 500}}, 1, &at);
    CHECK(r == GESTURE_ABORT && at == 500, "short second press: result %d at %lu", r, (unsigned long)at);

    gesture_start_released(&g, base);
    r = simulate(&g, base, (const press_t[]){{1200, 3000}}, 1, &at);
    CHECK(r == GESTURE_ABORT && at <= 1050, "late second press: result %d at %lu", r, (unsigned long)at);

    /* From USB mode: the first press is still held at 0. */
    gesture_start_pressed(&g, base);
    r = simulate(&g, base, (const press_t[]){{0, 5000}}, 1, &at);
    CHECK(r == GESTURE_ABORT && at > 1000 && at <= 1050, "first press held: result %d at %lu", r, (unsigned long)at);

    gesture_start_pressed(&g, base);
    r = simulate(&g, base, (const press_t[]){{0, 200}, {700, 2000}}, 2, &at);
    CHECK(r == GESTURE_POWER_ON && at >= 1200 && at <= 1250, "short then long press: result %d at %lu", r,
          (unsigned long)at);

    gesture_start_pressed(&g, base);
    r = simulate(&g, base, (const press_t[]){{0, 200}, {700, 900}, {1000, 2500}}, 3, &at);
    CHECK(r == GESTURE_ABORT && at == 900, "two short presses: result %d at %lu", r, (unsigned long)at);
  }

  /* Second press released without an UP event (lost): ends after the step limit. */
  gesture_start_released(&g, 0);
  gesture_step(&g, 100, &(button_event_t){BUTTON_DOWN, 0}, true);
  r = gesture_step(&g, 400, NULL, false);
  CHECK(r == GESTURE_CONTINUE, "released, before the hold time");
  r = gesture_step(&g, 700, NULL, false);
  CHECK(r == GESTURE_CONTINUE, "released, after the hold time");
  r = gesture_step(&g, 1150, NULL, false);
  CHECK(r == GESTURE_ABORT, "released, after the step limit: result %d", r);
}

/* ---- vario: vertical filter and tone map ---- */

static uint32_t rng_state = 12345U;

/* Approximately normal noise with standard deviation 1 (sum of 12 uniforms). */
static float noise(void)
{
  float s = 0.0f;
  for (int i = 0; i < 12; i++) {
    rng_state = rng_state * 1664525U + 1013904223U;
    s += (float)(rng_state >> 8) / 16777216.0f;
  }
  return s - 6.0f;
}

static void test_vario_filter(void)
{
  const float dt = 1.0f / 32.0f;
  const float sigma = 0.05f; /* altitude noise [m] */
  vario_filter_t f;
  vario_filter_init(&f, VARIO_FILTER_Q, VARIO_FILTER_R);
  float h = 1000.0f;
  float sum2 = 0.0f;
  int n2 = 0;
  float t_truth = -1.0f, t_filter = -1.0f, peak = 0.0f, v_sum = 0.0f;
  int v_n = 0;
  for (int i = 0; i < 32 * 20; i++) {
    float t = i * dt;
    float climb = t < 10.0f ? 0.0f : (t < 11.0f ? 2.0f * (t - 10.0f) : 2.0f); /* 0 -> 2 m/s in 1 s */
    h += climb * dt;
    vario_filter_update(&f, h + sigma * noise(), dt);
    if (t > 5.0f && t < 10.0f) {
      sum2 += f.v * f.v;
      n2++;
    }
    if (t_truth < 0.0f && climb >= 1.0f) {
      t_truth = t;
    }
    if (t_filter < 0.0f && t >= 10.0f && f.v >= 1.0f) {
      t_filter = t;
    }
    if (f.v > peak) {
      peak = f.v;
    }
    if (t > 15.0f) {
      v_sum += f.v;
      v_n++;
    }
  }
  float rms = sum2 / (float)n2;
  CHECK(rms < 0.06f * 0.06f, "vario noise at rest too high (mean square %d mm2/s2)", (int)(rms * 1e6f));
  CHECK(t_filter > 0.0f && t_filter - t_truth < 0.6f, "vario lag %d ms", (int)((t_filter - t_truth) * 1000.0f));
  CHECK(peak < 2.3f, "vario overshoot: peak %d mm/s", (int)(peak * 1000.0f));
  float v_mean = v_sum / (float)v_n;
  CHECK(v_mean > 1.95f && v_mean < 2.05f, "vario steady state %d mm/s", (int)(v_mean * 1000.0f));
}

static void check_tone(const vario_tones_t *t, float climb, bool sound, int freq, int period, int on, int line)
{
  vario_tone_t r = vario_tone(t, climb);
  CHECK(r.sound == sound && (!sound || (r.freq_hz == freq && r.period_ms == period && r.on_ms == on)),
        "tone at %d cm/s (line %d): sound %d %u Hz %u/%u ms", (int)(climb * 100.0f), line, r.sound, r.freq_hz, r.on_ms,
        r.period_ms);
}

/* ---- Bluetooth identity as the stock firmware forms it ---- */

static void test_identity(void)
{
  uint8_t a[6];
  identity_ble_address(a, 0x0228EFF7U, 0x26);
  CHECK(a[5] == 0x57 && a[4] == 0x0D && a[3] == 0x35 && a[2] == 0x26 && a[1] == 0xEF && a[0] == 0xF7,
        "address %02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
  char n[IDENTITY_NAME_MAX + 1];
  identity_device_name(n, sizeof(n), "Florian Schmid", 0x0228EFF7U);
  CHECK(strcmp(n, "UltraBip\xF0\x9F\xAA\x82" "Florian") == 0, "name: first word of the pilot: %s", n);
  identity_device_name(n, sizeof(n), "", 0x0228EFF7U);
  CHECK(strcmp(n, "UltraBip\xF0\x9F\xAA\x82" "EFF7") == 0, "name without a pilot: %s", n);
  identity_device_name(n, sizeof(n), "Jo:hn", 0x00A74360U);
  CHECK(strcmp(n, "UltraBip\xF0\x9F\xAA\x82" "Jo") == 0, "name stops at ':': %s", n);
  identity_device_name(n, sizeof(n), "Abcdefghijklmn", 0);
  CHECK(strlen(n) == 26 && strstr(n, "\xF0\x9F\xAA\x82" "Abcdefghijklmn") != NULL, "14 letters fit: %s", n);
  identity_device_name(n, sizeof(n), "Abcdefghijklmnopqrst", 0);
  CHECK(strcmp(n, "UltraBip_Abcdefghijklmnopq") == 0, "long name: '_' and cut: %s", n);
  identity_device_name_with(n, sizeof(n), " Flo S.", 0x0228EFF7U);
  CHECK(strcmp(n, "UltraBip\xF0\x9F\xAA\x82" "Flo S.") == 0, "own text with a space: %s", n);
  identity_device_name_with(n, sizeof(n), "", 0x0228EFF7U);
  CHECK(strcmp(n, "UltraBip\xF0\x9F\xAA\x82" "EFF7") == 0, "empty own text: the default: %s", n);
  identity_device_name_with(n, sizeof(n), "Abcdefghijklmnop\xC3\xA4", 0); /* 16 letters and an a-umlaut */
  CHECK(strcmp(n, "UltraBip_Abcdefghijklmnop") == 0, "cut before a two-byte character: %s", n);
}

/* console_pack on a plain buffer: the packet as a string, the used bytes in *used */
static const char *pack(const char *s, uint32_t from, bool flush, size_t max, uint32_t *used)
{
  static char pkt[128];
  size_t n = console_pack(s, UINT32_MAX, from, (uint32_t)strlen(s), flush, pkt, max, used);
  pkt[n] = 0;
  return pkt;
}

static void test_console_pack(void)
{
  uint32_t used;
  const char *s = "> help\r\n  status   device\r\n> ";
  const char *p = pack(s, 0, false, 64, &used);
  CHECK(strcmp(p, "CON > help\nCON   status   device\n") == 0 && used == 27, "two lines, the prompt waits: '%s' %lu", p,
        (unsigned long)used);
  p = pack(s, 27, false, 64, &used);
  CHECK(p[0] == 0 && used == 0, "a line without its end waits: '%s' %lu", p, (unsigned long)used);
  p = pack(s, 27, true, 64, &used);
  CHECK(strcmp(p, "CONP > \n") == 0 && used == 2, "it goes out after a pause: '%s' %lu", p, (unsigned long)used);
  p = pack("\r", 0, true, 64, &used);
  CHECK(p[0] == 0 && used == 1, "a lone CR is used up: '%s' %lu", p, (unsigned long)used);

  const char *l = "abcdefghijklmnopqrstuvwxyz\r\n";
  p = pack(l, 0, false, 16, &used);
  CHECK(strcmp(p, "CONP abcdefghij\n") == 0 && used == 10, "a long line in parts: '%s' %lu", p, (unsigned long)used);
  p = pack(l, 10, false, 16, &used);
  CHECK(strcmp(p, "CONP klmnopqrst\n") == 0 && used == 10, "second part: '%s' %lu", p, (unsigned long)used);
  p = pack(l, 20, false, 16, &used);
  CHECK(strcmp(p, "CON uvwxyz\n") == 0 && used == 8, "last part ends the line: '%s' %lu", p, (unsigned long)used);

  const char *u = "abcdefghi\xC3\xA4z\r\n";
  p = pack(u, 0, false, 16, &used);
  CHECK(strcmp(p, "CONP abcdefghi\n") == 0 && used == 9, "no cut inside a UTF-8 character: '%s' %lu", p,
        (unsigned long)used);
  p = pack(u, 9, false, 16, &used);
  CHECK(strcmp(p, "CON \xC3\xA4z\n") == 0 && used == 5, "the character in the next part: '%s' %lu", p,
        (unsigned long)used);

  /* across the end of a 16-byte ring */
  char ring[16];
  memset(ring, '#', sizeof(ring));
  memcpy(ring + 12, "abc\r", 4);
  memcpy(ring, "\nxy\n", 4);
  char pkt[64];
  size_t n = console_pack(ring, 15U, 12U, 20U, false, pkt, sizeof(pkt) - 1U, &used);
  pkt[n] = 0;
  CHECK(strcmp(pkt, "CON abc\nCON xy\n") == 0 && used == 8, "across the ring end: '%s' %lu", pkt, (unsigned long)used);
}

static void test_vario_tones(void)
{
  vario_tones_t t;
  vario_tones_defaults(&t);
  check_tone(&t, 0.8f, true, 513, 770, 200, __LINE__);   /* a point of the map */
  check_tone(&t, 1.0f, true, 594, 635, 168, __LINE__);   /* half way between 0.8 and 1.2 */
  check_tone(&t, 0.10f, true, 445, 868, 420, __LINE__);  /* lift threshold */
  check_tone(&t, 0.05f, false, 0, 0, 0, __LINE__);       /* below the threshold */
  check_tone(&t, -1.0f, false, 0, 0, 0, __LINE__);       /* normal sink: silent */
  check_tone(&t, -3.0f, true, 79, 450, 450, __LINE__);   /* sink alarm, continuous */
  check_tone(&t, -6.0f, true, 62, 450, 225, __LINE__);   /* below the map */
  check_tone(&t, 10.0f, true, 1000, 230, 104, __LINE__); /* above the map */
  t.sink_alarm = false;
  check_tone(&t, -3.0f, false, 0, 0, 0, __LINE__);

  /* weak-lift tone: a 40 ms tick every second within 0.4 m/s below the lift threshold (10 cm/s) */
  vario_tone_t w = vario_tone_weak(&t, 0.0f);
  CHECK(w.sound && w.period_ms == 1000 && w.on_ms == 40 && w.freq_hz > 100 && w.freq_hz < 440,
        "weak tone at 0 m/s: %u Hz, %u/%u ms", w.freq_hz, w.on_ms, w.period_ms);
  CHECK(!vario_tone_weak(&t, 0.10f).sound && !vario_tone_weak(&t, -0.31f).sound, "weak tone outside the band");
  t.lifty = false;
  CHECK(!vario_tone_weak(&t, 0.0f).sound, "weak tone when disabled");
  t.lifty = true;

  /* integration time: the stock app's seconds less the fusion's own 0.2 s */
  CHECK(t.integration_pct == 25, "default integration 25 %%");
  CHECK(fabsf(vario_integration_tau_s(0) - 0.1f) < 1e-4f && fabsf(vario_integration_tau_s(25) - 0.5f) < 1e-4f &&
            fabsf(vario_integration_tau_s(50) - 1.2f) < 1e-4f && fabsf(vario_integration_tau_s(100) - 5.5f) < 1e-4f &&
            fabsf(vario_integration_tau_s(200) - 5.5f) < 1e-4f,
        "integration time constants");
  CHECK(fabsf(vario_integration_tau_s(60) - 1.76f) < 1e-3f, "integration 60 %%: %.3f s", (double)vario_integration_tau_s(60));

  /* a running period is cut short for lift: lift during a weak tick or the sink tone at once */
  CHECK(vario_tone_cut_short(&t, 0.5f, false, 40, 300), "lift during the pause of a weak tick");
  CHECK(vario_tone_cut_short(&t, 0.5f, false, 450, 100), "lift during the sink tone");
  CHECK(!vario_tone_cut_short(&t, 0.05f, false, 40, 300), "below the lift threshold: the tick goes on");
  /* stronger lift during a slow lift period (0.1 m/s: 868 ms, beep 420 ms; 2 m/s: about 442 ms) */
  CHECK(!vario_tone_cut_short(&t, 2.0f, true, 420, 300), "a sounding beep is never cut");
  CHECK(vario_tone_cut_short(&t, 2.0f, true, 420, 600), "stronger lift: the shorter period is over");
  CHECK(!vario_tone_cut_short(&t, 0.15f, true, 420, 600), "about the same lift: the pause goes on");
  CHECK(!vario_tone_cut_short(&t, -1.0f, true, 420, 600), "lift ended: the period runs out");

  /* the TONES command of the configurator */
  vario_tones_t p;
  bool ok = vario_tones_parse("20 -300 1 -500,62,450,50 -350,75,450,100 -50,100,450,100 5,440,875,50 "
                              "80,513,770,26 120,675,500,27 300,845,370,30 800,1200,9000,45", &p);
  CHECK(ok && p.lift_start_cms == 20 && p.sink_start_cms == -300 && p.sink_alarm, "TONES header");
  CHECK(p.point[0].vz_cms == -500 && p.point[3].freq_hz == 440 && p.point[7].freq_hz == 1200 &&
            p.point[7].period_ms == 5000 && p.point[7].duty == 45,
        "TONES points (period limited to 5000 ms)");
  CHECK(!vario_tones_parse("20 -300 1 -500,62,450,50", &p), "TONES with too few points accepted");
  /* the flags after the points: given, they are taken; left out, the caller's stay */
  vario_tones_defaults(&p);
  p.lifty = false;
  p.integration_pct = 60;
  ok = vario_tones_parse("20 -300 1 -500,62,450,50 -350,75,450,100 -50,100,450,100 5,440,875,50 "
                         "80,513,770,26 120,675,500,27 300,845,370,30 800,1200,230,45", &p);
  CHECK(ok && !p.lifty && p.sweep && p.integration_pct == 60, "TONES without flags keeps the profile's");
  ok = vario_tones_parse("20 -300 1 -500,62,450,50 -350,75,450,100 -50,100,450,100 5,440,875,50 "
                         "80,513,770,26 120,675,500,27 300,845,370,30 800,1200,230,45 1 0 40", &p);
  CHECK(ok && p.lifty && !p.sweep && p.integration_pct == 40, "TONES with the weak-lift, sweep and integration flags");
}

/* ---- barometer + IMU fusion ---- */

/*
 * Device lying tilted (up = (3, 4, 8) in sensor axes), accelerometer 2.6 %
 * too high (as measured: 10.07 m/s^2 at rest), gyro bias 0.6 deg/s.
 * climb_from: start of a 2 m/s^2 vertical acceleration lasting 1 s (< 0 = none);
 * puff_at: centre of a 0.5 m pressure puff (< 0 = none).
 */
static float settle_peak; /* largest climb rate error between 2 s and 12 s (start-up) */
static int baro_every = 3;        /* IMU samples per barometer sample (3: 33 Hz) */
static float baro_noise = 0.05f;  /* barometric altitude noise per sample [m] */

static void run_fusion(fusion_t *f, float seconds, float climb_from, float puff_at, float *peak_v, float *t_1ms,
                       float *v_mean_end)
{
  settle_peak = 0.0f;
  const float ux = 0.31800f, uy = 0.42400f, uz = 0.84800f;
  const float dt = 0.01f;
  float h = 500.0f, v = 0.0f;
  float sum = 0.0f;
  int n = 0;
  *peak_v = 0.0f;
  *t_1ms = -1.0f;
  for (int i = 0; i < (int)(seconds * 100.0f); i++) {
    float t = i * dt;
    float a = (climb_from >= 0.0f && t >= climb_from && t < climb_from + 1.0f) ? 2.0f : 0.0f;
    v += a * dt;
    h += v * dt;
    float sf = 1.026f * (9.80665f + a);
    float acc[3] = {ux * sf + 0.02f * noise(), uy * sf + 0.02f * noise(), uz * sf + 0.02f * noise()};
    float gyr[3] = {0.0105f + 0.002f * noise(), 0.002f * noise(), 0.002f * noise()};
    fusion_imu(f, gyr, acc, dt);
    if (i % baro_every == 0) {
      float bump = 0.0f;
      if (puff_at >= 0.0f) {
        float x = (t - puff_at) / 0.4f;
        bump = 0.5f * expf(-x * x);
      }
      fusion_baro(f, h + bump + baro_noise * noise());
    }
    float err = fabsf(f->v - v);
    if (t > 2.0f && t < 12.0f && err > settle_peak) {
      settle_peak = err;
    }
    if (t > 12.0f && err > *peak_v) {
      *peak_v = err;
    }
    if (*t_1ms < 0.0f && climb_from >= 0.0f && t >= climb_from && f->v >= 1.0f) {
      *t_1ms = t;
    }
    if (t > seconds - 5.0f) {
      sum += f->v;
      n++;
    }
  }
  *v_mean_end = sum / (float)n;
}

/*
 * Lying still at 500 m; from t = 20 s the barometer reads `squeeze` m more for `len` s (a squeezed case:
 * negative = higher pressure), or for good if len < 0 (a pressure offset). hold: the button/tap hold
 * starts with it. Returns the largest climb rate error after 20 s.
 */
static float squeeze_peak(float squeeze, float len, bool hold)
{
  static fusion_t f;
  fusion_init(&f);
  const float ux = 0.31800f, uy = 0.42400f, uz = 0.84800f, dt = 0.01f;
  float peak = 0.0f;
  for (int i = 0; i < 4000; i++) {
    float t = i * dt;
    float sf = 1.026f * 9.80665f;
    float acc[3] = {ux * sf + 0.02f * noise(), uy * sf + 0.02f * noise(), uz * sf + 0.02f * noise()};
    float gyr[3] = {0.0105f + 0.002f * noise(), 0.002f * noise(), 0.002f * noise()};
    fusion_imu(&f, gyr, acc, dt);
    if (hold && i == 2000) {
      fusion_hold_baro(&f, FUSION_HOLD_S);
    }
    if (i % 3 == 0) {
      bool on = t >= 20.0f && (len < 0.0f || t < 20.0f + len);
      fusion_baro(&f, 500.0f + (on ? squeeze : 0.0f) + 0.05f * noise());
    }
    if (t >= 20.0f && fabsf(f.v) > peak) {
      peak = fabsf(f.v);
    }
  }
  return peak;
}

/* ---- manoeuvre modes: a spiral, handling, the g reference after a bad first sample ---- */

/* IMU sample of a steady rotation: load in g, rate in rad/s about the axis (nx, ny, nz). */
static void maneuver_imu(fusion_t *f, float load_g, float rate, float nx, float ny, float nz, float dt)
{
  float acc[3] = {0.0f, sqrtf(load_g * load_g - 1.0f) * 9.80665f, 9.80665f};
  float n = sqrtf(nx * nx + ny * ny + nz * nz);
  float gyr[3] = {rate * nx / n, rate * ny / n, rate * nz / n};
  fusion_imu(f, gyr, acc, dt);
}

static void test_fusion_maneuvers(void)
{
  static fusion_t f;
  const float dt = 0.01f;

  /* A spiral: 20 s at rest, 40 s at 3 g about a steady axis at 100 deg/s with a sink of 10 m/s and a
   * +-8 m pressure modulation at the rotation period (3.6 s), then 20 s of level flight at 1 g. */
  fusion_init(&f);
  fusion_set_baro_rate(&f, 100.0f);
  float err2 = 0.0f, quiet2 = 0.0f, window_50 = 0.0f, bias_19 = 0.0f, bias_79 = 0.0f;
  int n_err = 0, n_quiet = 0, mode_25 = -1, mode_59 = -1, mode_75 = -1;
  for (int i = 0; i < 8000; i++) {
    float t = i * dt;
    bool in = t >= 20.0f && t < 60.0f;
    maneuver_imu(&f, in ? 3.0f : 1.0f, in ? 1.745f : 0.0f, 0.0f, 0.0f, 1.0f, dt);
    float h = t < 20.0f ? 1500.0f
              : in      ? 1500.0f - 10.0f * (t - 20.0f) + 8.0f * sinf(2.0f * (float)M_PI * (t - 20.0f) / 3.6f)
                        : 1100.0f;
    fusion_baro(&f, h);
    if (t >= 30.0f && t < 58.0f) {
      float e = f.vz + 10.0f;
      err2 += e * e;
      n_err++;
    }
    if (t >= 70.0f) {
      quiet2 += f.vz * f.vz;
      n_quiet++;
    }
    if (i == 1900) {
      bias_19 = f.b;
    } else if (i == 2500) {
      mode_25 = f.mode;
    } else if (i == 5000) {
      window_50 = f.window;
    } else if (i == 5900) {
      mode_59 = f.mode;
    } else if (i == 7500) {
      mode_75 = f.mode;
    } else if (i == 7900) {
      bias_79 = f.b;
    }
  }
  CHECK(mode_25 == FUSION_MODE_SPIRAL && mode_59 == FUSION_MODE_SPIRAL, "spiral mode at 25 s: %d, at 59 s: %d", mode_25,
        mode_59);
  CHECK(mode_75 == FUSION_MODE_NORMAL, "mode 15 s after the spiral: %d", mode_75);
  CHECK(fabsf(window_50 - 3.6f) < 0.2f, "window one rotation: %d ms", (int)(window_50 * 1000.0f));
  float rms = sqrtf(err2 / (float)n_err);
  CHECK(rms < 0.7f, "spiral: climb rate error rms %d mm/s (the modulation alone would give 14 m/s)", (int)(rms * 1000.0f));
  float quiet = sqrtf(quiet2 / (float)n_quiet);
  CHECK(quiet < 0.3f, "after the spiral: climb rate rms %d mm/s", (int)(quiet * 1000.0f));
  CHECK(fabsf(bias_79 - bias_19) < 0.1f, "bias state changed by the spiral: %d -> %d mm/s2", (int)(bias_19 * 1000.0f),
        (int)(bias_79 * 1000.0f));

  /* Handling: 10 s at rest, 6 s of fast rotations about changing axes with the load swinging, 14 s at
   * rest. No spiral mode, the climb rate stays near zero (the IMU would show +-1 m/s). */
  fusion_init(&f);
  fusion_set_baro_rate(&f, 100.0f);
  static const float axes[6][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {0, 1, 1}, {1, 0, 1}};
  float during2 = 0.0f, after2 = 0.0f;
  int n_during = 0, n_after = 0, mode_13 = -1;
  bool spiral_seen = false;
  for (int i = 0; i < 3000; i++) {
    float t = i * dt;
    if (t >= 10.0f && t < 16.0f) {
      int k = (int)(t * 5.0f);
      const float *ax = axes[k % 6];
      float sgn = k % 2 ? 1.0f : -1.0f;
      float g = 9.80665f * (1.0f + 0.8f * sinf(2.0f * (float)M_PI * t * 1.7f));
      float acc[3] = {0.3f * g, 0.2f * g, g};
      float n = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
      float gyr[3] = {sgn * 5.236f * ax[0] / n, sgn * 5.236f * ax[1] / n, sgn * 5.236f * ax[2] / n};
      fusion_imu(&f, gyr, acc, dt);
    } else {
      maneuver_imu(&f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, dt);
    }
    fusion_baro(&f, 1500.0f);
    if (t >= 11.0f && t < 16.0f) {
      during2 += f.vz * f.vz;
      n_during++;
    } else if (t >= 22.0f) {
      after2 += f.vz * f.vz;
      n_after++;
    }
    if (f.mode == FUSION_MODE_SPIRAL) {
      spiral_seen = true;
    }
    if (i == 1300) {
      mode_13 = f.mode;
    }
  }
  CHECK(mode_13 == FUSION_MODE_HANDLING, "handling mode at 13 s: %d", mode_13);
  CHECK(!spiral_seen, "spiral mode during handling");
  float during = sqrtf(during2 / (float)n_during), after = sqrtf(after2 / (float)n_after);
  CHECK(during < 0.5f, "handled: climb rate rms %d mm/s", (int)(during * 1000.0f));
  CHECK(after < 0.2f, "after handling: climb rate rms %d mm/s", (int)(after * 1000.0f));

  /* The first sample 8 % below g (device in the hand at power-on), then at rest: g_ref recovers and
   * rest is detected (the old rest test against g_ref never passed again). */
  fusion_init(&f);
  fusion_set_baro_rate(&f, 100.0f);
  for (int i = 0; i < 2000; i++) {
    float acc[3] = {0.0f, 0.0f, i < 2 ? 0.92f * 9.80665f : 9.80665f};
    float gyr[3] = {0.0f, 0.0f, 0.0f};
    fusion_imu(&f, gyr, acc, dt);
    fusion_baro(&f, 1500.0f);
  }
  CHECK(f.at_rest && fabsf(f.g_ref - 9.80665f) < 0.1f, "bad first sample: rest %d, g_ref %d mm/s2", f.at_rest,
        (int)(f.g_ref * 1000.0f));
}

static void test_fusion(void)
{
  /* Handling: a squeezed case (-10 m for 0.6 s) or a pressure offset (+5 m for good). */
  float free_pulse = squeeze_peak(-10.0f, 0.6f, false);
  float held_pulse = squeeze_peak(-10.0f, 0.6f, true);
  CHECK(free_pulse > 1.0f && held_pulse < 0.3f, "squeezed case: %d mm/s without hold, %d mm/s with hold",
        (int)(free_pulse * 1000.0f), (int)(held_pulse * 1000.0f));
  float held_offset = squeeze_peak(5.0f, -1.0f, true);
  CHECK(held_offset < 0.3f, "pressure offset during a hold: %d mm/s", (int)(held_offset * 1000.0f));

  fusion_t f;
  float peak, t1, vend;

  /* Lying still, one pressure puff: the climb rate stays quiet. */
  fusion_init(&f);
  run_fusion(&f, 30.0f, -1.0f, 20.0f, &peak, &t1, &vend);
  CHECK(f.at_rest, "rest not detected");
  CHECK(peak < 0.09f, "puff on the table: climb rate error %d mm/s", (int)(peak * 1000.0f));
  CHECK(settle_peak < 0.12f, "start-up on the table: climb rate error %d mm/s", (int)(settle_peak * 1000.0f));
  CHECK(fabsf(f.g_ref - 1.026f * 9.80665f) < 0.05f, "g_ref %d mm/s2", (int)(f.g_ref * 1000.0f));

  /* A gyro bias of 3 deg/s (the test device has 2.9) still allows rest and is learned. */
  fusion_init(&f);
  for (int i = 0; i < 1000; i++) {
    float acc[3] = {0.02f * noise(), 0.02f * noise(), 10.07f + 0.02f * noise()};
    float gyr[3] = {0.0524f + 0.002f * noise(), 0.002f * noise(), 0.002f * noise()};
    fusion_imu(&f, gyr, acc, 0.01f);
  }
  CHECK(f.at_rest && fabsf(f.gyro_bias[0] - 0.0524f) < 0.005f, "gyro bias: rest %d, learned %d mdeg/s", f.at_rest,
        (int)(f.gyro_bias[0] * 57296.0f));

  /* A steady turn (18 deg/s, 20 deg bank) is no rest, although nothing fluctuates. */
  fusion_init(&f);
  for (int i = 0; i < 1000; i++) {
    float load = 9.80665f / 0.9397f;
    float acc[3] = {0.02f * noise(), 0.02f * noise(), load + 0.02f * noise()};
    float gyr[3] = {0.0f, 0.0f, 0.314f};
    fusion_imu(&f, gyr, acc, 0.01f);
  }
  CHECK(!f.at_rest, "steady turn taken as rest");

  /* Climb: 0 -> 2 m/s within 1 s, shows almost at once and holds. */
  fusion_init(&f);
  run_fusion(&f, 25.0f, 10.0f, -1.0f, &peak, &t1, &vend);
  CHECK(t1 > 0.0f && t1 - 10.5f < 0.1f, "climb 1 m/s reached %d ms after the truth", (int)((t1 - 10.5f) * 1000.0f));
  CHECK(peak < 0.3f, "climb: largest climb rate error %d mm/s", (int)(peak * 1000.0f));
  CHECK(fabsf(vend - 2.0f) < 0.1f, "climb: steady %d mm/s", (int)(vend * 1000.0f));
  float t1_33 = t1;

  /* Barometer at 100 Hz with the same noise per second: the same behaviour as at 33 Hz. */
  baro_every = 1;
  baro_noise = 0.05f * 1.732f;
  fusion_init(&f);
  fusion_set_baro_rate(&f, 100.0f);
  run_fusion(&f, 25.0f, 10.0f, -1.0f, &peak, &t1, &vend);
  CHECK(t1 > 0.0f && fabsf(t1 - t1_33) < 0.05f && peak < 0.3f && fabsf(vend - 2.0f) < 0.1f,
        "100 Hz climb: 1 m/s after %d ms (33 Hz: %d ms), error %d mm/s, steady %d mm/s", (int)((t1 - 10.5f) * 1000.0f),
        (int)((t1_33 - 10.5f) * 1000.0f), (int)(peak * 1000.0f), (int)(vend * 1000.0f));
  fusion_init(&f);
  fusion_set_baro_rate(&f, 100.0f);
  run_fusion(&f, 30.0f, -1.0f, 20.0f, &peak, &t1, &vend);
  CHECK(f.at_rest && peak < 0.09f, "100 Hz: puff on the table %d mm/s, rest %d", (int)(peak * 1000.0f), f.at_rest);
  baro_every = 3;
  baro_noise = 0.05f;
}

/* ---- NMEA epochs ---- */

/* Splits a sentence (without $ and checksum) at the commas, like gps.c. */
static int fields(char *buf, size_t size, const char *sentence, char **f, int max)
{
  snprintf(buf, size, "%s", sentence);
  int n = 0;
  f[n++] = buf;
  for (char *p = buf; *p && n < max; p++) {
    if (*p == ',') {
      *p = 0;
      f[n++] = p + 1;
    }
  }
  return n;
}

static void test_nmea(void)
{
  uint32_t ms = 0;
  CHECK(nmea_time_of_day("120512.25", &ms) && ms == 43512250U, "time of day %lu", (unsigned long)ms);
  CHECK(nmea_time_of_day("235959", &ms) && ms == 86399000U, "time without fraction %lu", (unsigned long)ms);
  CHECK(!nmea_time_of_day("12x512", &ms), "malformed time accepted");
  CHECK(nmea_coord_e7("4612.3456", "N") == 462057600, "lat %ld", (long)nmea_coord_e7("4612.3456", "N"));
  CHECK(nmea_coord_e7("01120.7890", "E") == 113464833, "lon %ld", (long)nmea_coord_e7("01120.7890", "E"));
  CHECK(nmea_coord_e7("01120.7890", "W") == -113464833, "west");
  CHECK(nmea_coord_e7("3352.12345", "S") == -338687242, "south, 5 decimals %ld", (long)nmea_coord_e7("3352.12345", "S"));
  CHECK(nmea_coord_e7("", "N") == 0, "empty coordinate");

  nmea_epoch_t e;
  gps_fix_t fix;
  char buf[128];
  char *f[24];
  int n;
  nmea_epoch_init(&e);

  n = fields(buf, sizeof(buf), "GPGGA,120512.25,4612.3456,N,01120.7890,E,1,08,0.9,1305.3,M,46.4,M,,", f, 24);
  nmea_epoch_gga(&e, f, n);
  n = fields(buf, sizeof(buf), "GPGSA,A,3,01,02,03,04,05,06,07,08,,,,,1.6,0.9,1.3", f, 24);
  nmea_epoch_gsa(&e, f, n);
  n = fields(buf, sizeof(buf), "GPRMC,120512.25,A,4612.3456,N,01120.7890,E,10.0,90.0,071026,,,A", f, 24);
  CHECK(nmea_epoch_rmc(&e, f, n, &fix), "epoch not completed");
  CHECK(fix.valid && fix.fix_type == 3 && fix.sats == 8, "fix valid %d type %u sats %u", fix.valid, fix.fix_type,
        fix.sats);
  CHECK(fix.unix_time == unix_from_civil(2026, 10, 7, 12, 5, 12) && fix.ms == 250, "fix time %lu.%03u",
        (unsigned long)fix.unix_time, fix.ms);
  CHECK(fix.lat_e7 == 462057600 && fix.lon_e7 == 113464833, "fix position");
  CHECK(fabsf(fix.alt_msl_m - 1305.3f) < 0.01f && fabsf(fix.geoid_sep_m - 46.4f) < 0.01f, "heights");
  CHECK(fabsf(fix.vel_e_ms - 5.144f) < 0.01f && fabsf(fix.vel_n_ms) < 0.01f, "velocity east %d mm/s",
        (int)(fix.vel_e_ms * 1000.0f));
  CHECK(!fix.vel_d_valid, "vertical speed without a previous fix");

  /* One second later, 2 m higher: sinking speed -2 m/s. */
  n = fields(buf, sizeof(buf), "GPGGA,120513.25,4612.3456,N,01120.7890,E,1,08,0.9,1307.3,M,46.4,M,,", f, 24);
  nmea_epoch_gga(&e, f, n);
  n = fields(buf, sizeof(buf), "GPRMC,120513.25,A,4612.3456,N,01120.7890,E,0.0,0.0,071026,,,A", f, 24);
  CHECK(nmea_epoch_rmc(&e, f, n, &fix) && fix.vel_d_valid && fabsf(fix.vel_d_ms + 2.0f) < 0.01f,
        "vertical speed %d mm/s", (int)(fix.vel_d_ms * 1000.0f));

  /* RMC of another second than the GGA: no epoch. */
  n = fields(buf, sizeof(buf), "GPGGA,120514.00,4612.3456,N,01120.7890,E,1,08,0.9,1307.3,M,46.4,M,,", f, 24);
  nmea_epoch_gga(&e, f, n);
  n = fields(buf, sizeof(buf), "GPRMC,120515.00,A,4612.3456,N,01120.7890,E,0.0,0.0,071026,,,A", f, 24);
  CHECK(!nmea_epoch_rmc(&e, f, n, &fix), "epoch from mismatched times");

  /* No fix yet: the module's default date and status V. */
  n = fields(buf, sizeof(buf), "GPGGA,000007.00,4612.3456,N,01120.7890,E,0,00,2.3,1305.3,M,46.4,M,,", f, 24);
  nmea_epoch_gga(&e, f, n);
  n = fields(buf, sizeof(buf), "GPRMC,000007.00,V,4612.3456,N,01120.7890,E,,,060180,,,N,V", f, 24);
  CHECK(nmea_epoch_rmc(&e, f, n, &fix) && !fix.valid, "fix without satellites taken as valid");
}

/* ---- takeoff and landing ---- */

typedef struct {
  flight_detector_t d;
  uint32_t t;
  float x, y; /* metres east / north of the start */
  float alt;
} flight_sim_t;

/* Runs `seconds` at constant velocity; returns the first event (time in *at). */
static flight_event_t flight_run(flight_sim_t *s, int seconds, float ve, float vn, float vz, bool fix, uint32_t *at)
{
  const float m_per_e7 = 0.0111319f;
  const float coslat = 0.69466f; /* 46 degrees */
  for (int i = 0; i < seconds; i++) {
    s->t++;
    s->x += ve;
    s->y += vn;
    s->alt += vz;
    flight_input_t in = {s->t, fix, 460000000 + (int32_t)(s->y / m_per_e7), 110000000 + (int32_t)(s->x / (m_per_e7 * coslat)),
                         sqrtf(ve * ve + vn * vn), s->alt, vz, false, 0.0f, 0.0f, 0.0f, 1.0f};
    flight_event_t ev = flight_update(&s->d, &in);
    if (ev != FLIGHT_EVENT_NONE) {
      *at = s->t;
      return ev;
    }
  }
  return FLIGHT_EVENT_NONE;
}

/* GPS errors: the position moves `ve` m/s east while the module reports `speed` and `hdop`; level, with a fix. */
static flight_event_t flight_run_gps(flight_sim_t *s, int seconds, float ve, float speed, float hdop, uint32_t *at)
{
  const float m_per_e7 = 0.0111319f;
  const float coslat = 0.69466f;
  for (int i = 0; i < seconds; i++) {
    s->t++;
    s->x += ve;
    flight_input_t in = {s->t, true, 460000000 + (int32_t)(s->y / m_per_e7), 110000000 + (int32_t)(s->x / (m_per_e7 * coslat)),
                         speed, s->alt, 0.0f, false, 0.0f, 0.0f, 0.0f, hdop};
    flight_event_t ev = flight_update(&s->d, &in);
    if (ev != FLIGHT_EVENT_NONE) {
      *at = s->t;
      return ev;
    }
  }
  return FLIGHT_EVENT_NONE;
}

static void flight_sim_init(flight_sim_t *s)
{
  memset(s, 0, sizeof(*s));
  flight_init(&s->d);
  s->t = 1000;
  s->alt = 1500.0f;
}

static float man_tilt = -1.0f; /* the measured tilt man_turn feeds (negative: not known) */

static maneuver_kind_t man_turn(maneuver_t *m, float load_g, float turn_dps, float seconds)
{
  maneuver_kind_t k = MANEUVER_NONE;
  for (int i = 0; i < (int)(seconds / 0.05f + 0.5f); i++) {
    maneuver_kind_t r = maneuver_update(m, load_g, fabsf(turn_dps), turn_dps, man_tilt, 0.05f);
    if (r != MANEUVER_NONE) {
      k = r;
    }
  }
  return k;
}

static void test_maneuver(void)
{
  maneuver_t m;
  maneuver_init(&m);
  man_turn(&m, 1.0f, 0.0f, 40.0f); /* past the settling time after the takeoff */
  /* a steep thermal turn: 1.4 g at 25 deg/s for a minute is no manoeuvre */
  CHECK(man_turn(&m, 1.4f, 25.0f, 60.0f) == MANEUVER_NONE && !m.active, "thermal turn: manoeuvre");
  /* thermal circles that change direction at 1.5 g: no wingover (the load stays below 1.8 g) */
  man_turn(&m, 1.5f, -45.0f, 4.0f);
  man_turn(&m, 1.1f, 0.0f, 1.0f);
  CHECK(man_turn(&m, 1.5f, 45.0f, 4.0f) == MANEUVER_NONE && !m.active, "change of direction at 1.5 g");
  /* wingover as flown on 2026-10-16: left 1.5 s at 1.6 g, reversal 1.5 s at 1.0 g, right 2 s at 2.5 g */
  man_turn(&m, 1.0f, 0.0f, 10.0f);
  man_turn(&m, 1.6f, -80.0f, 1.5f);
  man_turn(&m, 1.0f, 10.0f, 1.5f);
  CHECK(man_turn(&m, 2.5f, 110.0f, 2.0f) == MANEUVER_NONE && m.active && m.kind == MANEUVER_WINGOVER, "wingover: start");
  /* and back left once more: the same wingover, two swings */
  man_turn(&m, 1.1f, -10.0f, 1.0f);
  man_turn(&m, 2.2f, -100.0f, 2.0f);
  maneuver_kind_t k = man_turn(&m, 1.0f, 0.0f, 3.0f);
  CHECK(k == MANEUVER_WINGOVER && m.done_reversals == 2 && m.done_n_max > 2.4f && m.done_n_max <= 2.5f &&
            m.done_bank_deg > 65.0f && m.done_bank_deg < 67.0f,
        "wingover: %s, %u swings, %d/100 g, bank %d", maneuver_kind_name(k), m.done_reversals,
        (int)(m.done_n_max * 100.0f), (int)m.done_bank_deg);
  /* spiral: 2.0 g at 90 deg/s for 10 s, then level */
  man_turn(&m, 1.0f, 0.0f, 10.0f);
  man_turn(&m, 2.0f, 90.0f, 10.0f);
  k = man_turn(&m, 1.0f, 0.0f, 3.0f);
  CHECK(k == MANEUVER_SPIRAL && m.done_n_max > 1.95f && m.done_bank_deg > 59.0f && m.done_bank_deg < 61.0f,
        "spiral: %s, bank %d", maneuver_kind_name(k), (int)m.done_bank_deg);
  /* swings into a spiral: the wingover is reported when the turn goes on, then the spiral */
  man_turn(&m, 1.0f, 0.0f, 10.0f);
  man_turn(&m, 1.7f, -70.0f, 1.5f);
  man_turn(&m, 1.0f, 5.0f, 1.0f);
  k = man_turn(&m, 2.6f, 110.0f, 6.0f);
  CHECK(k == MANEUVER_WINGOVER && m.done_into_turn && m.done_n_max > 2.5f && m.done_n_max < 2.65f && m.active &&
            m.kind == MANEUVER_SPIRAL,
        "swings into a spiral: wingover (%d/100 g, not the spiral's load), spiral running", (int)(m.done_n_max * 100.0f));
  CHECK(man_turn(&m, 1.0f, 0.0f, 3.0f) == MANEUVER_SPIRAL, "swings into a spiral: spiral");
  /* a 2.3 g pull without turning (collapse recovery) is no wingover */
  man_turn(&m, 1.0f, 0.0f, 10.0f);
  man_turn(&m, 0.3f, 0.0f, 1.0f);
  man_turn(&m, 2.3f, 20.0f, 1.0f);
  CHECK(man_turn(&m, 1.0f, 0.0f, 3.0f) == MANEUVER_NONE && !m.active, "pull-up without turns");
  /* knocks and handling: fast short swings beyond 200 deg/s */
  for (int i = 0; i < 6; i++) {
    man_turn(&m, 2.0f, i % 2 ? 300.0f : -300.0f, 0.6f);
  }
  CHECK(man_turn(&m, 1.0f, 0.0f, 3.0f) == MANEUVER_NONE && !m.active, "knocks: no wingover");
  CHECK(m.g_max_flight > 2.5f && m.g_max_flight <= 2.6f, "flight peak load %d/100 g", (int)(m.g_max_flight * 100.0f));
  CHECK(maneuver_bank_deg(1.0f) == 0.0f && maneuver_bank_deg(0.5f) == 0.0f, "bank below 1 g");
  /* right after the takeoff (the launch): no wingover */
  maneuver_init(&m);
  man_turn(&m, 1.6f, -80.0f, 1.5f);
  man_turn(&m, 1.0f, 10.0f, 1.0f);
  man_turn(&m, 2.5f, 110.0f, 2.0f);
  CHECK(man_turn(&m, 1.0f, 0.0f, 3.0f) == MANEUVER_NONE, "no wingover in the first 30 s");

  /* with the measured tilt: the wingover's bank is its peak, the unloaded top between the swings included */
  maneuver_init(&m);
  man_tilt = 10.0f;
  man_turn(&m, 1.0f, 0.0f, 40.0f);
  man_tilt = 60.0f;
  man_turn(&m, 1.6f, -80.0f, 1.5f);
  man_tilt = 118.0f; /* the top of the swing, unloaded */
  man_turn(&m, 1.0f, 10.0f, 1.5f);
  man_tilt = 70.0f;
  man_turn(&m, 2.5f, 110.0f, 2.0f);
  man_tilt = 10.0f;
  k = man_turn(&m, 1.0f, 0.0f, 3.0f);
  CHECK(k == MANEUVER_WINGOVER && m.done_tilt_deg == 118.0f && m.done_bank_deg == 118.0f,
        "wingover with the tilt: bank %d (tilt %d)", (int)m.done_bank_deg, (int)m.done_tilt_deg);
  /* the tilt not known through the first swing: acos(1/n) as before, the tilt not reported */
  man_turn(&m, 1.0f, 0.0f, 10.0f);
  man_tilt = -1.0f;
  man_turn(&m, 1.6f, -80.0f, 1.5f);
  man_tilt = 118.0f;
  man_turn(&m, 1.0f, 10.0f, 1.5f);
  man_turn(&m, 2.5f, 110.0f, 2.0f);
  man_tilt = 10.0f;
  k = man_turn(&m, 1.0f, 0.0f, 3.0f);
  CHECK(k == MANEUVER_WINGOVER && m.done_tilt_deg < 0.0f && m.done_bank_deg > 66.0f && m.done_bank_deg < 67.0f,
        "wingover, tilt not known throughout: bank %d from the load", (int)m.done_bank_deg);
  /* a spiral keeps acos(1/n) and reports the tilt it measured */
  man_turn(&m, 1.0f, 0.0f, 10.0f);
  man_tilt = 100.0f;
  man_turn(&m, 2.0f, 90.0f, 10.0f);
  man_tilt = 10.0f;
  k = man_turn(&m, 1.0f, 0.0f, 3.0f);
  CHECK(k == MANEUVER_SPIRAL && m.done_tilt_deg == 100.0f && m.done_bank_deg > 59.0f && m.done_bank_deg < 61.0f,
        "spiral with the tilt: bank %d from the load, tilt %d", (int)m.done_bank_deg, (int)m.done_tilt_deg);
  /* swings into a spiral: the tilt up to the spiral's first second, not the spiral's */
  man_turn(&m, 1.0f, 0.0f, 10.0f);
  man_tilt = 80.0f;
  man_turn(&m, 1.7f, -70.0f, 1.5f);
  man_turn(&m, 1.0f, 5.0f, 1.0f);
  man_tilt = 85.0f;
  man_turn(&m, 2.6f, 110.0f, 1.0f);
  man_tilt = 130.0f;
  k = man_turn(&m, 2.6f, 110.0f, 5.0f);
  CHECK(k == MANEUVER_WINGOVER && m.done_into_turn && m.done_tilt_deg == 85.0f,
        "swings into a spiral: tilt %d (not the spiral's)", (int)m.done_tilt_deg);
  man_tilt = -1.0f;
}

/* ---- tilt.c on a synthetic flight: the device mounted askew on the pilot ---- */

#define TSIM_DT 0.01f
typedef struct {
  tilt_t t;
  float R[3][3];  /* pilot frame (x forward, y left, z up) -> device frame */
  float u[3];     /* the true vertical in the pilot frame */
  float bias[3];  /* gyro bias [rad/s] */
  float v;        /* speed along the path [m/s] */
  bool valid;     /* the last update gave a tilt */
  bool ever_valid;
  float est_peak, true_peak, err_max;
} tsim_t;

/* u turned about the axis w by -|w| dt (a vector fixed in the earth frame, seen from the body) */
static void tsim_turn(float u[3], const float w[3], float dt)
{
  float n = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
  if (n <= 0.0f) {
    return;
  }
  float k[3] = {w[0] / n, w[1] / n, w[2] / n}, th = -n * dt, c = cosf(th), s = sinf(th);
  float kxu[3] = {k[1] * u[2] - k[2] * u[1], k[2] * u[0] - k[0] * u[2], k[0] * u[1] - k[1] * u[0]};
  float kd = k[0] * u[0] + k[1] * u[1] + k[2] * u[2];
  for (int i = 0; i < 3; i++) {
    u[i] = u[i] * c + kxu[i] * s + k[i] * kd * (1.0f - c);
  }
}

/* seconds of the rotation wp [rad/s] in the pilot frame; the specific force of a flight along the pilot's
 * forward axis at speed v: g u + w x (v F) */
static void tsim_run(tsim_t *s, float wx, float wy, float wz, float seconds, bool in_flight)
{
  const float wp[3] = {wx, wy, wz};
  for (int i = 0; i < (int)(seconds / TSIM_DT + 0.5f); i++) {
    tsim_turn(s->u, wp, TSIM_DT);
    float fp[3] = {9.80665f * s->u[0], 9.80665f * s->u[1] + s->v * wz, 9.80665f * s->u[2] - s->v * wy};
    float a[3], w[3];
    for (int r = 0; r < 3; r++) {
      a[r] = s->R[r][0] * fp[0] + s->R[r][1] * fp[1] + s->R[r][2] * fp[2];
      w[r] = s->R[r][0] * wp[0] + s->R[r][1] * wp[1] + s->R[r][2] * wp[2] + s->bias[r];
    }
    s->valid = tilt_update(&s->t, a, w, TSIM_DT, in_flight, s->v);
    float truth = acosf(s->u[2] > 1.0f ? 1.0f : s->u[2]) * 57.29578f;
    if (s->valid) {
      s->ever_valid = true;
      s->est_peak = fmaxf(s->est_peak, s->t.tilt_deg);
      s->true_peak = fmaxf(s->true_peak, truth);
      s->err_max = fmaxf(s->err_max, fabsf(s->t.tilt_deg - truth));
    }
  }
}

/* a coordinated turn at this bank (right positive): roll in for a second, turn, roll out */
static void tsim_turn_at(tsim_t *s, float bank_deg, float seconds)
{
  float b = bank_deg * 0.017453293f;
  float omega = 9.80665f * tanf(fabsf(b)) / s->v; /* tan(bank) = v omega / g */
  tsim_run(s, b, 0.0f, 0.0f, 1.0f, true);
  /* right turn: the rotation vector points down, -omega u */
  float sg = b > 0.0f ? -1.0f : 1.0f;
  tsim_run(s, sg * omega * s->u[0], sg * omega * s->u[1], sg * omega * s->u[2], seconds, true);
  tsim_run(s, -b, 0.0f, 0.0f, 1.0f, true);
}

static void test_tilt(void)
{
  static tsim_t s; /* tilt_t is small, the simulation too; static keeps the test's stack flat */
  memset(&s, 0, sizeof(s));
  tilt_init(&s.t);
  /* the device turned by 35 degrees about x, then 50 about y */
  float a1 = 35.0f * 0.017453293f, a2 = 50.0f * 0.017453293f;
  float Rx[3][3] = {{1, 0, 0}, {0, cosf(a1), -sinf(a1)}, {0, sinf(a1), cosf(a1)}};
  float Ry[3][3] = {{cosf(a2), 0, sinf(a2)}, {0, 1, 0}, {-sinf(a2), 0, cosf(a2)}};
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 3; c++) {
      s.R[r][c] = Ry[r][0] * Rx[0][c] + Ry[r][1] * Rx[1][c] + Ry[r][2] * Rx[2][c];
    }
  }
  s.u[2] = 1.0f;
  s.bias[0] = 0.010f; /* 0.6 deg/s */
  s.bias[1] = -0.006f;
  s.bias[2] = 0.004f;
  s.v = 11.0f;
  /* on the ground: the gyro bias */
  tsim_run(&s, 0.0f, 0.0f, 0.0f, 10.0f, false);
  CHECK(fabsf(s.t.bias[0] - 0.010f) < 0.0005f && fabsf(s.t.bias[1] + 0.006f) < 0.0005f && !s.ever_valid,
        "tilt: gyro bias learned on the ground (%d, %d mdeg/s)", (int)(s.t.bias[0] * 57295.78f),
        (int)(s.t.bias[1] * 57295.78f));
  /* straight flight, then turns both ways: not valid before the axes are learned */
  tsim_run(&s, 0.0f, 0.0f, 0.0f, 25.0f, true);
  CHECK(!s.ever_valid && s.t.n_straight >= 2000U, "tilt: no tilt from straight flight alone (%lu samples)",
        (unsigned long)s.t.n_straight);
  tsim_turn_at(&s, 30.0f, 9.0f);
  tsim_run(&s, 0.0f, 0.0f, 0.0f, 3.0f, true);
  tsim_turn_at(&s, -30.0f, 9.0f);
  tsim_run(&s, 0.0f, 0.0f, 0.0f, 5.0f, true);
  float mp[3] = {s.R[0][2], s.R[1][2], s.R[2][2]}, fp[3] = {s.R[0][0], s.R[1][0], s.R[2][0]};
  float mdot = mp[0] * s.t.M[0] + mp[1] * s.t.M[1] + mp[2] * s.t.M[2];
  float fdot = fp[0] * s.t.F[0] + fp[1] * s.t.F[1] + fp[2] * s.t.F[2];
  CHECK(s.t.axes && mdot > cosf(2.0f * 0.017453293f) && fdot > cosf(5.0f * 0.017453293f),
        "tilt: axes learned (up off by %d/10 deg, forward by %d/10 deg)", (int)(acosf(fminf(mdot, 1.0f)) * 572.9578f),
        (int)(acosf(fminf(fdot, 1.0f)) * 572.9578f));
  /* a coordinated turn: the tilt is its bank, acos(1/n) */
  s.err_max = 0.0f;
  s.est_peak = s.true_peak = 0.0f;
  tsim_turn_at(&s, 40.0f, 10.0f);
  CHECK(s.valid && fabsf(s.est_peak - 40.0f) < 2.0f && s.err_max < 3.0f, "tilt: coordinated turn at 40 deg: peak %d/10, "
        "error up to %d/10 deg", (int)(s.est_peak * 10.0f), (int)(s.err_max * 10.0f));
  /* a steep spiral, 60 deg at 18 m/s (2 g) for 20 s */
  s.v = 18.0f;
  s.err_max = 0.0f;
  s.est_peak = s.true_peak = 0.0f;
  tsim_turn_at(&s, -60.0f, 20.0f);
  CHECK(fabsf(s.est_peak - 60.0f) < 3.0f && s.err_max < 5.0f, "tilt: spiral at 60 deg: peak %d/10, error up to %d/10 deg",
        (int)(s.est_peak * 10.0f), (int)(s.err_max * 10.0f));
  /* a wingover's roll to 120 deg and back, with a gyro bias that changed since the takeoff (0.5 deg/s) */
  s.v = 11.0f;
  s.bias[2] += 0.0087f;
  tsim_run(&s, 0.0f, 0.0f, 0.0f, 10.0f, true);
  s.err_max = 0.0f;
  s.est_peak = s.true_peak = 0.0f;
  tsim_run(&s, 80.0f * 0.017453293f, 0.0f, 0.0f, 1.5f, true);
  tsim_run(&s, 0.0f, -0.3f, 0.0f, 0.4f, true); /* the top: nose down, unloaded */
  tsim_run(&s, -80.0f * 0.017453293f, 0.0f, 0.0f, 1.5f, true);
  tsim_run(&s, 0.0f, 0.3f, 0.0f, 0.4f, true);
  CHECK(fabsf(s.est_peak - s.true_peak) < 5.0f && s.true_peak > 118.0f, "tilt: roll to %d deg: peak %d/10 deg",
        (int)s.true_peak, (int)(s.est_peak * 10.0f));
}

static void test_flight_thresholds(void)
{
  flight_sim_t s;
  uint32_t at = 0;
  /* strict thresholds: the standard launch (10 m/s, sinking 1 m/s) is no takeoff within 20 s */
  flight_sim_init(&s);
  flight_takeoff_t strict = {50.0f / 3.6f, 5, 500.0f, 100.0f, 5.0f, 5};
  flight_set_takeoff(&s.d, &strict);
  flight_run(&s, 60, 0, 0, 0, true, &at);
  CHECK(flight_run(&s, 20, 10.0f, 0, -1.0f, true, &at) == FLIGHT_EVENT_NONE, "strict thresholds: no takeoff");
  /* easy thresholds: a 2 m/s walk counts (5 km/h for 3 s and 20 m within 30 s) */
  flight_sim_init(&s);
  flight_takeoff_t easy = {5.0f / 3.6f, 3, 20.0f, 100.0f, 5.0f, 5};
  flight_set_takeoff(&s.d, &easy);
  flight_run(&s, 60, 0, 0, 0, true, &at);
  CHECK(flight_run(&s, 30, 2.0f, 0, 0, true, &at) == FLIGHT_EVENT_TAKEOFF, "easy thresholds: a walk counts");
  /* out-of-range values are clamped */
  flight_takeoff_t wild = {0.0f, 0, 1e6f, 0.0f, 100.0f, 200};
  flight_set_takeoff(&s.d, &wild);
  CHECK(s.d.takeoff.speed_ms > 1.3f && s.d.takeoff.speed_s == 1 && s.d.takeoff.distance_m == 500.0f &&
            s.d.takeoff.altitude_m == 5.0f && s.d.takeoff.climb_ms == 5.0f && s.d.takeoff.climb_s == 30,
        "thresholds clamped");
}

/* 2026: March 1 and November 1 are Sundays; the switch days follow from that. */
static void test_summer_time(void)
{
  const uint32_t h = 3600U;
  uint32_t t = unix_from_civil(2026, 3, 29, 1, 0, 0); /* EU: last Sunday of March 01:00 UTC */
  CHECK(!announce_summer_time(t - 1, 60, DST_EU) && announce_summer_time(t, 60, DST_EU), "EU start");
  t = unix_from_civil(2026, 10, 25, 1, 0, 0);
  CHECK(announce_summer_time(t - 1, 60, DST_EU) && !announce_summer_time(t, 60, DST_EU), "EU end");
  t = unix_from_civil(2026, 3, 8, 7, 0, 0); /* US, New York (UTC-5): second Sunday of March 02:00 EST */
  CHECK(!announce_summer_time(t - 1, -300, DST_US) && announce_summer_time(t, -300, DST_US), "US start");
  t = unix_from_civil(2026, 11, 1, 6, 0, 0); /* first Sunday of November 02:00 EDT = 01:00 EST */
  CHECK(announce_summer_time(t - 1, -300, DST_US) && !announce_summer_time(t, -300, DST_US), "US end");
  t = unix_from_civil(2026, 4, 4, 16, 0, 0); /* AU, Sydney (UTC+10): first Sunday of April 02:00 AEST */
  CHECK(announce_summer_time(t - 1, 600, DST_AU) && !announce_summer_time(t, 600, DST_AU), "AU end");
  t = unix_from_civil(2026, 10, 3, 16, 0, 0); /* first Sunday of October 02:00 AEST */
  CHECK(!announce_summer_time(t - 1, 600, DST_AU) && announce_summer_time(t, 600, DST_AU), "AU start");
  t = unix_from_civil(2026, 9, 26, 14, 0, 0); /* NZ (UTC+12): last Sunday of September 02:00 NZST */
  CHECK(!announce_summer_time(t - 1, 720, DST_NZ) && announce_summer_time(t, 720, DST_NZ), "NZ start");
  t = unix_from_civil(2026, 9, 6, 4, 0, 0); /* Chile (UTC-4): first Sunday of September 00:00 */
  CHECK(!announce_summer_time(t - 1, -240, DST_CL) && announce_summer_time(t, -240, DST_CL), "CL start");
  CHECK(!announce_summer_time(unix_from_civil(2026, 7, 1, 12, 0, 0), 60, DST_NONE), "no rule");
  CHECK(announce_local_seconds(unix_from_civil(2026, 7, 1, 12, 0, 0), 60, DST_EU) == 14U * h, "local 14:00 CEST");
  CHECK(announce_local_seconds(unix_from_civil(2026, 1, 1, 12, 0, 0), 60, DST_EU) == 13U * h, "local 13:00 CET");
  CHECK(announce_dst_rule_parse("us") == DST_US && announce_dst_rule_parse("nz") == DST_NZ &&
            announce_dst_rule_parse("x") == DST_NONE,
        "rule names");
}

/* The stock app's time change table decides the offset; the app also stores the summer offset (120) as the
 * profile's offset, which must not get the summer hour on top. */
static void test_tz_table(void)
{
  tz_table_t t;
  announce_tz_table_parse("251026  60,260329 120,261025  60,270328 120,2710", &t);
  CHECK(t.n == 4 && t.at[1] == unix_from_civil(2026, 3, 29, 1, 0, 0) && t.min[1] == 120 && t.min[3] == 120,
        "table parsed, truncated entry dropped");
  uint32_t summer = unix_from_civil(2026, 10, 13, 17, 0, 0), winter = unix_from_civil(2026, 12, 1, 12, 0, 0);
  CHECK(announce_utc_offset_min(summer, 120, true, DST_EU, &t) == 120, "app file in summer: 120, not 180");
  CHECK(announce_utc_offset_min(winter, 120, true, DST_EU, &t) == 60, "app file in winter: the table's 60");
  CHECK(announce_utc_offset_min(summer, 60, true, DST_EU, &t) == 120, "configurator file in summer: 120");
  CHECK(announce_utc_offset_min(summer, 120, false, DST_EU, &t) == 120, "automatic off: the offset as it is");
  CHECK(announce_utc_offset_min(unix_from_civil(2025, 7, 1, 12, 0, 0), 60, true, DST_EU, &t) == 120,
        "before the table: the rule");
  CHECK(announce_utc_offset_min(unix_from_civil(2029, 7, 1, 12, 0, 0), 60, true, DST_EU, &t) == 120 &&
            announce_utc_offset_min(unix_from_civil(2028, 1, 1, 12, 0, 0), 60, true, DST_EU, &t) == 120,
        "a year past the last entry it stays, then the rule takes over");
  announce_tz_table_parse("", &t);
  CHECK(t.n == 0 && announce_utc_offset_min(summer, 60, true, DST_EU, &t) == 120 &&
            announce_utc_offset_min(summer, 60, true, DST_EU, NULL) == 120,
        "no table: the rule");
  char many[TZ_TABLE_MAX * 12 + 40] = "";
  for (unsigned i = 0; i < TZ_TABLE_MAX + 4; i++) {
    char e[16];
    snprintf(e, sizeof(e), "%02u%02u01 %3d,", 26 + i / 2, i % 2 ? 10 : 3, i % 2 ? 60 : 120);
    strcat(many, e);
  }
  announce_tz_table_parse(many, &t);
  CHECK(t.n == TZ_TABLE_MAX, "at most TZ_TABLE_MAX entries");
  announce_tz_table_parse("260329 120,251026  60", &t);
  CHECK(t.n == 1, "entries out of order are dropped");
}

static void test_flight(void)
{
  {
    flight_detector_t d;
    flight_init(&d);
    flight_force_airborne(&d, 1000);
    CHECK(d.state == FLIGHT_AIRBORNE && d.takeoff_s == 1000, "forced airborne (takeoff detection off)");
  }
  flight_sim_t s;
  uint32_t at = 0;

  /* standing at the launch, walking around it: a few metres back and forth, short walks with breaks */
  flight_sim_init(&s);
  CHECK(flight_run(&s, 120, 0, 0, 0, true, &at) == FLIGHT_EVENT_NONE, "standing: takeoff at %lu", (unsigned long)at);
  flight_event_t ev = FLIGHT_EVENT_NONE;
  for (int i = 0; i < 10 && ev == FLIGHT_EVENT_NONE; i++) { /* 15 m out, back, 20 s standing */
    ev = flight_run(&s, 10, 1.5f, 0, 0, true, &at);
    ev = ev != FLIGHT_EVENT_NONE ? ev : flight_run(&s, 10, -1.5f, 0, 0, true, &at);
    ev = ev != FLIGHT_EVENT_NONE ? ev : flight_run(&s, 20, 0, 0, 0, true, &at);
  }
  CHECK(ev == FLIGHT_EVENT_NONE, "walking around at the launch: takeoff at %lu", (unsigned long)at);
  for (int i = 0; i < 4 && ev == FLIGHT_EVENT_NONE; i++) { /* 40 m to another spot, 90 s standing there */
    ev = flight_run(&s, 30, 1.33f, 0, 0, true, &at);
    ev = ev != FLIGHT_EVENT_NONE ? ev : flight_run(&s, 90, 0, 0, 0, true, &at);
  }
  CHECK(ev == FLIGHT_EVENT_NONE, "short walks with breaks: takeoff at %lu", (unsigned long)at);

  /* GPS errors: a jump of 100 m for 10 s while standing, a position wandering 80 m without speed, a moving
   * position with matching speed but poor fixes (HDOP 5) */
  flight_sim_init(&s);
  flight_run(&s, 60, 0, 0, 0, true, &at);
  s.x += 100.0f;
  ev = flight_run(&s, 10, 0, 0, 0, true, &at);
  s.x -= 100.0f;
  ev = ev != FLIGHT_EVENT_NONE ? ev : flight_run(&s, 30, 0, 0, 0, true, &at);
  CHECK(ev == FLIGHT_EVENT_NONE, "GPS jump: takeoff at %lu", (unsigned long)at);
  CHECK(flight_run_gps(&s, 200, 0.4f, 0.0f, 1.0f, &at) == FLIGHT_EVENT_NONE, "wandering position: takeoff at %lu",
        (unsigned long)at);
  CHECK(flight_run_gps(&s, 200, 1.5f, 1.5f, 5.0f, &at) == FLIGHT_EVENT_NONE, "poor fixes: takeoff at %lu",
        (unsigned long)at);

  /* walking 60 m away from the launch point in one go is a takeoff (by design: the vario is switched on at the
   * launch, 2026-10-13), dated at the start of the walk */
  flight_sim_init(&s);
  flight_run(&s, 60, 0, 0, 0, true, &at);
  uint32_t walk = s.t + 1;
  ev = flight_run(&s, 60, 1.5f, 0, 0, true, &at);
  CHECK(ev == FLIGHT_EVENT_TAKEOFF && s.d.takeoff_s == walk, "walking away: event %d, takeoff time %+ld s", ev,
        (long)(s.d.takeoff_s - walk));

  /* Launch: 3 s run at 3 m/s, then 10 m/s ground speed, sinking 1 m/s; dated at the start of the run. */
  flight_sim_init(&s);
  flight_run(&s, 60, 0, 0, 0, true, &at);
  uint32_t run = s.t + 1;
  flight_run(&s, 3, 3.0f, 0, 0, true, &at);
  ev = flight_run(&s, 60, 10.0f, 0, -1.0f, true, &at);
  CHECK(ev == FLIGHT_EVENT_TAKEOFF && at - run <= 8U, "launch: event %d after %ld s", ev, (long)(at - run));
  CHECK(s.d.takeoff_s == run, "launch: takeoff time %+ld s from the start of the run", (long)(s.d.takeoff_s - run));

  /* Landing: after some flight the pilot stands still (landing time set to 30 s). */
  flight_set_landing(&s.d, true, 30);
  flight_run(&s, 120, 8.0f, 2.0f, -1.0f, true, &at);
  uint32_t touchdown = s.t + 1;
  ev = flight_run(&s, 60, 0, 0, 0, true, &at);
  CHECK(ev == FLIGHT_EVENT_LANDING && at - touchdown >= 29U && at - touchdown <= 31U, "landing: event %d after %ld s",
        ev, (long)(at - touchdown));
  CHECK(s.d.state == FLIGHT_LANDED, "landed state");
  /* the pilot walks away from the landing: no takeoff (the launch point counts before the first one only) */
  CHECK(flight_run(&s, 200, 1.5f, 0, 0, true, &at) == FLIGHT_EVENT_NONE, "walking away after the landing: takeoff");
  /* a relaunch after a top landing: run and fly */
  flight_run(&s, 3, 3.0f, 0, 0, true, &at);
  CHECK(flight_run(&s, 30, 10.0f, 0, -1.0f, true, &at) == FLIGHT_EVENT_TAKEOFF, "relaunch after a top landing");

  /* climbing in front of the launch, hardly moving (strong wind): the altitude decides */
  flight_sim_init(&s);
  flight_run(&s, 60, 0, 0, 0, true, &at);
  uint32_t lift = s.t + 1;
  ev = flight_run(&s, 60, 0.3f, 0, 1.0f, true, &at);
  CHECK(ev == FLIGHT_EVENT_TAKEOFF && at - lift <= 16U && s.d.takeoff_s <= lift + 2U,
        "vertical departure: event %d after %ld s, takeoff time %+ld s", ev, (long)(at - lift),
        (long)(s.d.takeoff_s - lift));

  /* Default landing time 60 s; switched off: no landing at all. */
  flight_sim_init(&s);
  flight_run(&s, 60, 0, 0, 0, true, &at);
  flight_run(&s, 60, 10.0f, 0, -1.0f, true, &at);
  touchdown = s.t + 1;
  ev = flight_run(&s, 120, 0, 0, 0, true, &at);
  CHECK(ev == FLIGHT_EVENT_LANDING && at - touchdown >= 59U && at - touchdown <= 61U, "default landing after %ld s",
        (long)(at - touchdown));
  flight_sim_init(&s);
  flight_set_landing(&s.d, false, 60);
  flight_run(&s, 60, 0, 0, 0, true, &at);
  flight_run(&s, 60, 10.0f, 0, -1.0f, true, &at);
  CHECK(flight_run(&s, 600, 0, 0, 0, true, &at) == FLIGHT_EVENT_NONE && s.d.state == FLIGHT_AIRBORNE,
        "landing detected although switched off");

  /* Calm soaring, hovering in strong wind for 45 s: no landing with the default time. */
  flight_sim_init(&s);
  flight_run(&s, 60, 0, 0, 0, true, &at);
  flight_run(&s, 60, 10.0f, 0, -1.0f, true, &at);
  CHECK(flight_run(&s, 45, 0.3f, 0, 0.1f, true, &at) == FLIGHT_EVENT_NONE, "hovering taken as landing");

  /* Ridge soaring in strong wind: 3 m/s along the ridge, climbing 1 m/s. */
  flight_sim_init(&s);
  flight_run(&s, 60, 0, 0, 0, true, &at);
  ev = flight_run(&s, 60, 0, 3.0f, 1.0f, true, &at);
  CHECK(ev == FLIGHT_EVENT_TAKEOFF, "ridge soaring not detected");

  /* No GPS fix: no takeoff whatever happens. */
  flight_sim_init(&s);
  CHECK(flight_run(&s, 120, 10.0f, 0, -1.0f, false, &at) == FLIGHT_EVENT_NONE, "takeoff without a fix");
}

/* Like flight_run, with the IMU peaks of every second (flight_input_t motion fields). */
static flight_event_t flight_run_m(flight_sim_t *s, int seconds, float ve, float vz, bool fix, float amax, float amin,
                                   float rate, uint32_t *at)
{
  const float m_per_e7 = 0.0111319f;
  const float coslat = 0.69466f;
  for (int i = 0; i < seconds; i++) {
    s->t++;
    s->x += ve;
    s->alt += vz;
    flight_input_t in = {s->t, fix, 460000000 + (int32_t)(s->y / m_per_e7), 110000000 + (int32_t)(s->x / (m_per_e7 * coslat)),
                         fabsf(ve), s->alt, vz, true, amax, amin, rate, 1.0f};
    flight_event_t ev = flight_update(&s->d, &in);
    if (ev != FLIGHT_EVENT_NONE) {
      *at = s->t;
      return ev;
    }
  }
  return FLIGHT_EVENT_NONE;
}

/* Airborne after a launch and some gliding, as recorded on 2026-10-12 (1.2 g, 40 deg/s). */
static void flight_airborne(flight_sim_t *s, bool fix)
{
  uint32_t at;
  flight_sim_init(s);
  flight_run(s, 60, 0, 0, 0, true, &at);
  flight_run(s, 60, 10.0f, 0, -1.0f, true, &at);
  flight_run_m(s, 60, 9.0f, -1.2f, fix, 1.25f, 0.9f, 40.0f, &at);
}

static void test_landing_motion(void)
{
  flight_sim_t s;
  uint32_t at = 0;

  /* touchdown with a 7 g shock, then walking away at 5.4 km/h: landing ~10 s later, dated at the shock */
  flight_airborne(&s, true);
  CHECK(s.d.state == FLIGHT_AIRBORNE, "impact: not airborne before the landing");
  flight_run_m(&s, 1, 3.0f, -1.0f, true, 7.3f, 0.15f, 860.0f, &at);
  uint32_t touchdown = s.t;
  flight_event_t ev = flight_run_m(&s, 60, 1.5f, 0.0f, true, 1.4f, 0.8f, 130.0f, &at);
  CHECK(ev == FLIGHT_EVENT_LANDING && at - touchdown <= 18U && s.d.landing_s == touchdown,
        "impact and walking: event %d after %ld s, landing at %+ld s", ev, (long)(at - touchdown),
        (long)(s.d.landing_s - touchdown));
  /* the approach (sinking 1.2 m/s until the touchdown) is no altitude change for a new takeoff */
  CHECK(flight_run_m(&s, 60, 1.5f, 0.0f, true, 1.4f, 0.8f, 130.0f, &at) == FLIGHT_EVENT_NONE &&
            flight_run(&s, 60, 0, 0, 0, true, &at) == FLIGHT_EVENT_NONE && s.d.state == FLIGHT_LANDED,
        "takeoff right after an impact landing at %lu", (unsigned long)at);

  /* switched off: walking keeps the device in flight mode (the calm rule needs < 3 km/h) */
  flight_airborne(&s, true);
  flight_landing_t off = {false, 4.0f, 10.0f / 3.6f, 10, false};
  flight_set_landing_rules(&s.d, &off);
  flight_run_m(&s, 1, 3.0f, -1.0f, true, 7.3f, 0.15f, 860.0f, &at);
  CHECK(flight_run_m(&s, 120, 1.5f, 0.0f, true, 1.4f, 0.8f, 130.0f, &at) == FLIGHT_EVENT_NONE,
        "impact rule switched off: landing while walking");

  /* a spiral pulls 4.2 g steadily (load never below 3 g): no landing, even if slow and level after it */
  flight_airborne(&s, true);
  CHECK(flight_run_m(&s, 20, 15.0f, -15.0f, true, 4.2f, 3.5f, 170.0f, &at) == FLIGHT_EVENT_NONE &&
            flight_run_m(&s, 30, 2.0f, 0.0f, true, 1.2f, 0.9f, 40.0f, &at) == FLIGHT_EVENT_NONE,
        "spiral taken as an impact");

  /* a shock in turbulence while gliding on: still sinking and fast, no landing */
  flight_airborne(&s, true);
  flight_run_m(&s, 1, 9.0f, -2.0f, true, 5.0f, 0.3f, 200.0f, &at);
  CHECK(flight_run_m(&s, 60, 9.0f, -1.2f, true, 1.25f, 0.9f, 40.0f, &at) == FLIGHT_EVENT_NONE,
        "turbulence shock in flight taken as a landing");

  /* hovering in strong wind after a shock, climbing and sinking 1 m/s in turns of 30 s: no landing */
  flight_airborne(&s, true);
  flight_run_m(&s, 1, 1.0f, -1.0f, true, 5.0f, 0.3f, 200.0f, &at);
  ev = FLIGHT_EVENT_NONE;
  for (int i = 0; i < 6 && ev == FLIGHT_EVENT_NONE; i++) {
    ev = flight_run_m(&s, 15, 0.5f, i % 2 ? 1.0f : -1.0f, true, 1.3f, 0.8f, 40.0f, &at);
  }
  CHECK(ev == FLIGHT_EVENT_NONE, "hovering with lift and sink taken as a landing");

  /* the GNSS module silent: the shock and a level barometer are enough */
  flight_airborne(&s, false);
  flight_run_m(&s, 1, 0.0f, -1.0f, false, 6.5f, 0.2f, 900.0f, &at);
  touchdown = s.t;
  ev = flight_run_m(&s, 60, 0.0f, 0.0f, false, 1.4f, 0.8f, 130.0f, &at);
  CHECK(ev == FLIGHT_EVENT_LANDING && at - touchdown <= 18U, "impact without GNSS: event %d after %ld s", ev,
        (long)(at - touchdown));

  /* soft landing without a shock, then packing: fast rotations with near free fall */
  flight_airborne(&s, true);
  flight_run_m(&s, 5, 1.0f, 0.0f, true, 1.5f, 0.8f, 60.0f, &at);
  uint32_t ground = s.t - 4U;
  ev = FLIGHT_EVENT_NONE;
  for (int i = 0; i < 12 && ev == FLIGHT_EVENT_NONE; i++) {
    ev = flight_run_m(&s, 1, 1.5f, 0.0f, true, 3.0f, 0.3f, 450.0f, &at);
    if (ev == FLIGHT_EVENT_NONE) {
      ev = flight_run_m(&s, 4, 1.5f, 0.0f, true, 1.4f, 0.8f, 120.0f, &at);
    }
  }
  CHECK(ev == FLIGHT_EVENT_LANDING && at - ground <= 30U, "ground handling: event %d after %ld s", ev,
        (long)(at - ground));

  /* handling rule off: the same stays in flight mode */
  flight_airborne(&s, true);
  flight_landing_t no_handling = {true, 4.0f, 10.0f / 3.6f, 10, false};
  flight_set_landing_rules(&s.d, &no_handling);
  ev = FLIGHT_EVENT_NONE;
  for (int i = 0; i < 12 && ev == FLIGHT_EVENT_NONE; i++) {
    ev = flight_run_m(&s, 1, 1.5f, 0.0f, true, 3.0f, 0.3f, 450.0f, &at);
    if (ev == FLIGHT_EVENT_NONE) {
      ev = flight_run_m(&s, 4, 1.5f, 0.0f, true, 1.4f, 0.8f, 120.0f, &at);
    }
  }
  CHECK(ev == FLIGHT_EVENT_NONE, "handling rule switched off: landing");
}

/* ---- IGC lines ---- */

static void test_igc(void)
{
  char b64[16];
  base64_encode((const uint8_t *)"f", 1, b64);
  CHECK(strcmp(b64, "Zg==") == 0, "base64 f: %s", b64);
  base64_encode((const uint8_t *)"fo", 2, b64);
  CHECK(strcmp(b64, "Zm8=") == 0, "base64 fo: %s", b64);
  base64_encode((const uint8_t *)"foobar", 6, b64);
  CHECK(strcmp(b64, "Zm9vYmFy") == 0, "base64 foobar: %s", b64);

  /* The example of docs/igc-vbv-records.md, byte for byte. */
  char line[IGC_LINE_MAX];
  igc_vbv_t s = {61000, 46.512345f, 11.345678f, 2104.512f, 1.234f, -8.765f, 1.321f,
                 2051.73f, 2056.3f, -1.35f, 2051.9f, 9.95f, 12, 1};
  igc_format_vbv(line, sizeof(line), &s);
  CHECK(strcmp(line, "LVBVSO4AAKQMOkLmhzVBMYgDRbbznT9xPQzBhxapP647AEXNhABFzcysv2Y+AEUzMx9BDAE=\r\n") == 0,
        "VBV record: %s", line);

  /* B record: coordinates cut to 0.001 minute, SIU extension. */
  igc_fix_t b = {unix_from_civil(2026, 10, 7, 10, 5, 51), 474554333, 132912333, true, 1524, 1617, 9};
  igc_format_b(line, sizeof(line), &b);
  CHECK(strcmp(line, "B1005514727325N01317473EA015240161709\r\n") == 0, "B record: %s", line);
  igc_fix_t b2 = {unix_from_civil(2026, 10, 7, 23, 59, 59), -338687242, -1512000000, false, -12, 3, 4};
  igc_format_b(line, sizeof(line), &b2);
  CHECK(strcmp(line, "B2359593352123S15112000WV-00120000304\r\n") == 0, "B record south-west: %s", line);

  igc_format_marker(line, sizeof(line), "TAKEOFF", unix_from_civil(2026, 10, 7, 10, 5, 49));
  CHECK(strcmp(line, "LVBVTAKEOFF100549\r\n") == 0, "marker: %s", line);

  igc_file_name(line, sizeof(line), unix_from_civil(2026, 10, 7, 10, 5, 49), "A1B", 3);
  CHECK(strcmp(line, "2026-10-07-XSD-UBA1B-03.igc") == 0, "file name: %s", line);

  uint8_t mac[32];
  memset(mac, 0xAB, sizeof(mac));
  size_t n = igc_format_g(line, sizeof(line), mac);
  CHECK(n == 4 + 44 + 2 && strncmp(line, "GXUB", 4) == 0, "G record: %s", line);

  char header[1024];
  igc_header_t h = {unix_from_civil(2026, 10, 7, 10, 5, 49), 3, "A1B", "", "Example Wing", NULL, "", "0.8.0"};
  n = igc_format_header(header, sizeof(header), &h);
  CHECK(n > 0 && strncmp(header, "AXSDUBA1B\r\n", 11) == 0, "A record");
  CHECK(strstr(header, "HFDTE071026\r\n") != NULL, "date record");
  CHECK(strstr(header, "HFPLTPILOTINCHARGE:NIL\r\n") != NULL && strstr(header, "HFGTYGLIDERTYPE:Example Wing\r\n"),
        "pilot / glider");
  CHECK(strstr(header, "I013637SIU\r\n") != NULL && strstr(header, "LXUBFIRMWARE:UltraBeep") != NULL,
        "I record and our firmware record");
  CHECK(igc_format_header(header, 100, &h) == 0, "header overflow not reported");
}

/* ---- IGC recorder (src/app/recorder.c with tests/recorder_stubs.c) ---- */

extern uint32_t test_tick;
extern char test_igc_file[];
extern uint32_t test_igc_len;
extern char test_igc_path[];
extern bool test_igc_closed;
extern unsigned test_dir_entries;
extern char test_dir_serial[5];

static recorder_sample_t rec_sample(uint32_t utc, float north_m, float alt, float speed)
{
  recorder_sample_t s;
  memset(&s, 0, sizeof(s));
  s.fix.valid = true;
  s.fix.fix_type = 3;
  s.fix.unix_time = utc;
  s.fix.ms = 250;
  s.fix.lat_e7 = 462057600 + (int32_t)(north_m / 0.0111319f);
  s.fix.lon_e7 = 113464833;
  s.fix.alt_msl_m = alt;
  s.fix.geoid_sep_m = 46.4f;
  s.fix.speed_ms = speed;
  s.fix.vel_n_ms = speed;
  s.fix.sats = 9;
  s.fix.rx_ms = test_tick;
  s.alt_std_m = alt - 30.0f;
  s.alt_raw_m = alt + 0.3f;
  s.alt_m = alt + 0.2f;
  s.vario_ms = speed > 0.0f ? -1.0f : 0.0f;
  s.accel_ms2 = 9.9f;
  s.second = true;
  return s;
}

static unsigned count_lines(const char *file, uint32_t len, const char *prefix, size_t line_len)
{
  unsigned n = 0;
  size_t plen = strlen(prefix);
  for (uint32_t i = 0; i < len;) {
    const char *end = memchr(file + i, '\n', len - i);
    size_t l = end ? (size_t)(end - (file + i)) + 1 : len - i;
    if (l >= plen && strncmp(file + i, prefix, plen) == 0 && (line_len == 0 || l == line_len)) {
      n++;
    }
    i += (uint32_t)l;
  }
  return n;
}

/*
 * 20 s on the ground, takeoff (estimated 5 s before its detection), 30 s flight, landing, 5 s, off.
 * per_second samples per second (data record rate), the first of each second marked.
 */
static void record_flight_at(uint32_t t0, unsigned per_second)
{
  recorder_reset();
  float north = 0.0f, alt = 1500.0f;
  for (int i = 0; i < 55; i++) {
    bool flying = i >= 15 && i < 45;
    if (flying) {
      north += 10.0f;
      alt -= 1.0f;
    }
    for (unsigned j = 0; j < per_second; j++) {
      test_tick = 1000U + 1000U * (uint32_t)i + 1000U * j / per_second;
      recorder_sample_t s = rec_sample(t0 + (uint32_t)i, north, alt, flying ? 10.0f : 0.0f);
      if (per_second > 1U) {
        s.fix.ms = (uint16_t)(1000U * j / per_second);
      }
      s.second = j == 0;
      recorder_sample(&s);
    }
    if (i == 19) {
      recorder_takeoff(t0 + 15U, 1);
    }
    if (i == 49) {
      recorder_landing(t0 + 45U);
    }
  }
  recorder_close();
}

static void record_flight(uint32_t t0)
{
  record_flight_at(t0, 1U);
}

static void test_recorder(void)
{
  const uint32_t t0 = unix_from_civil(2026, 10, 7, 10, 5, 30);

  /* Flight number: two earlier flights of the day -> 03. */
  test_dir_entries = 0;
  record_flight(t0);
  memcpy(test_dir_serial, test_igc_path + strlen("0:/tracks_IGC/2026/2026-10-07-XSD-UB"), 4);
  test_dir_entries = 2;
  record_flight(t0);
  CHECK(strlen(test_igc_path) > 6 && strcmp(test_igc_path + strlen(test_igc_path) - 6, "03.igc") == 0,
        "flight number: %s", test_igc_path);

  /* The file checked here and by run_logic_tests.py (tools/vbv_igc.py). */
  test_dir_entries = 0;
  record_flight(t0);
  CHECK(strncmp(test_igc_path, "0:/tracks_IGC/2026/2026-10-07-XSD-UB", 36) == 0 &&
            strcmp(test_igc_path + 40, "-01.igc") == 0,
        "file name: %s", test_igc_path);
  CHECK(test_igc_closed, "file not closed");
  CHECK(test_igc_len > 6 && strncmp(test_igc_file, "AXSDUB", 6) == 0, "A record");
  CHECK(strstr(test_igc_file, "HFPLTPILOTINCHARGE:Test Pilot\r\n") != NULL, "pilot from the profile");
  /* 15 pre-recorded seconds + 35 live: one data record and one B record each */
  unsigned b = count_lines(test_igc_file, test_igc_len, "B", 39);
  unsigned d = count_lines(test_igc_file, test_igc_len, "LVBV", 74);
  CHECK(b == 50 && d == 50, "records: %u B, %u data", b, d);
  /* marker right before the data record of 10:05:45 (followed by its B record) */
  const char *m = strstr(test_igc_file, "LVBVTAKEOFF100545\r\n");
  CHECK(m != NULL && strncmp(m + 19 + 74, "B100545", 7) == 0, "takeoff marker position");
  CHECK(strstr(test_igc_file, "LVBVLANDING100615\r\n") != NULL, "landing marker");
  /* first data record of the file: t = 1000 ms */
  const char *first = strstr(test_igc_file, "\r\nLVBV");
  char b64[12];
  base64_encode((const uint8_t *)"\xe8\x03\x00\x00", 4, b64); /* 1000 little endian: "6AMAAA==" */
  CHECK(first != NULL && strncmp(first + 6, b64, 5) == 0, "first t is not 1000 ms");
  const char *g = test_igc_file + test_igc_len - 50;
  CHECK(test_igc_len > 50 && strncmp(g, "GXUB", 4) == 0 && g[48] == '\r', "G record at the end");

  /* KML next to it: valid document with one coordinate per record */
  extern char test_kml_file[];
  extern uint32_t test_kml_len;
  static const char footer[] = "</coordinates>\n</LineString>\n</Placemark>\n</Document>\n</kml>\n";
  test_kml_file[test_kml_len] = 0;
  CHECK(strncmp(test_kml_file, "<?xml", 5) == 0 && test_kml_len > sizeof(footer) &&
            strcmp(test_kml_file + test_kml_len - (sizeof(footer) - 1), footer) == 0,
        "KML document (%lu bytes)", (unsigned long)test_kml_len);
  unsigned points = count_lines(test_kml_file, test_kml_len, "11.3464833,", 0);
  CHECK(points == 50, "KML points: %u", points);
  CHECK(strstr(test_kml_file, "<name>2026-10-07 Test Pilot</name>") != NULL, "KML name");

  /* KML only (flight_recordigc_bool off): no IGC file, the KML file as before */
  config_get()->record_igc = false;
  test_igc_path[0] = 0;
  record_flight(t0);
  test_kml_file[test_kml_len] = 0;
  points = count_lines(test_kml_file, test_kml_len, "11.3464833,", 0);
  CHECK(test_igc_path[0] == 0 && points == 50, "KML only: IGC %s, %u KML points", test_igc_path[0] ? "written" : "none",
        points);
  config_get()->record_igc = true;

  /* data records at 4 Hz: before the takeoff one sample per second, B records and KML points
   * once per second */
  test_dir_entries = 0;
  record_flight_at(t0, 4U);
  b = count_lines(test_igc_file, test_igc_len, "B", 39);
  d = count_lines(test_igc_file, test_igc_len, "LVBV", 74);
  CHECK(b == 50 && d == 15 + 35 * 4, "4 Hz records: %u B, %u data", b, d);
  test_kml_file[test_kml_len] = 0;
  points = count_lines(test_kml_file, test_kml_len, "11.3464833,", 0);
  CHECK(points == 50, "4 Hz KML points: %u", points);
  CHECK(test_igc_closed && strncmp(test_igc_file + test_igc_len - 50, "GXUB", 4) == 0, "4 Hz G record");
}

/* ---- battery, KML, flight summary ---- */

static void test_battery(void)
{
  battery_watch_t w;
  battery_watch_init(&w);
  int ev[12], n = 0;
  /* 3.70 V, then 3.58 V (low) for 4 readings, 3.44 V (critical) */
  static const uint32_t mv[] = {3700, 3580, 3580, 3580, 3580, 3440, 3440, 3440};
  for (unsigned i = 0; i < sizeof(mv) / sizeof(mv[0]); i++) {
    ev[n++] = battery_watch_update(&w, mv[i]);
  }
  CHECK(ev[0] == BATTERY_EVENT_NONE && ev[1] == BATTERY_EVENT_NONE && ev[2] == BATTERY_EVENT_NONE, "early events");
  CHECK(ev[3] == BATTERY_EVENT_LOW && ev[4] == BATTERY_EVENT_NONE, "low warning once: %d %d", ev[3], ev[4]);
  CHECK(ev[5] == BATTERY_EVENT_NONE && ev[6] == BATTERY_EVENT_NONE && ev[7] == BATTERY_EVENT_CRITICAL,
        "critical after three readings: %d %d %d", ev[5], ev[6], ev[7]);
  battery_watch_init(&w);
  /* a single dip under load is no warning */
  CHECK(battery_watch_update(&w, 3400) == BATTERY_EVENT_NONE && battery_watch_update(&w, 3800) == BATTERY_EVENT_NONE &&
            battery_watch_update(&w, 3400) == BATTERY_EVENT_NONE,
        "single dips");
  /* the stock's levels: 0 % at 3640 mV is the warning, its power off at 3580 mV or less */
  battery_watch_init(&w);
  for (int i = 0; i < 3; i++) {
    ev[i] = battery_watch_update(&w, 3620);
  }
  CHECK(ev[2] == BATTERY_EVENT_LOW, "3620 mV is low: %d", ev[2]);
  for (int i = 0; i < 3; i++) {
    ev[i] = battery_watch_update(&w, 3575);
  }
  CHECK(ev[2] == BATTERY_EVENT_CRITICAL, "3575 mV is empty: %d", ev[2]);
  /* no start on an empty battery, always with USB power or without a reading */
  CHECK(!battery_can_start(3600, false) && !battery_can_start(2600, false) && battery_can_start(3650, false) &&
            battery_can_start(3500, true) && battery_can_start(0, false) && battery_can_start(1200, false),
        "start levels");
}

static void test_kml(void)
{
  char buf[512];
  size_t n = kml_header(buf, sizeof(buf), "2026-10-07 A & B", "UltraBeep <test>");
  CHECK(n > 0 && strstr(buf, "<name>2026-10-07 A &amp; B</name>") && strstr(buf, "UltraBeep &lt;test&gt;") &&
            strstr(buf, "<coordinates>\n"),
        "KML header");
  kml_point(buf, sizeof(buf), 462057600, -113464833, 1305.26f);
  CHECK(strcmp(buf, "-11.3464833,46.2057600,1305.3\n") == 0, "KML point: %s", buf);
  kml_point(buf, sizeof(buf), -5, 5, -12.0f);
  CHECK(strcmp(buf, "0.0000005,-0.0000005,-12.0\n") == 0, "KML point small: %s", buf);
  kml_footer(buf, sizeof(buf));
  CHECK(strcmp(buf, "</coordinates>\n</LineString>\n</Placemark>\n</Document>\n</kml>\n") == 0, "KML footer");
}

static void test_summary(void)
{
  CHECK(summary_altitude_clip(1234.0f) == 1230 && summary_altitude_clip(5049.0f) == 5000 &&
            summary_altitude_clip(5160.0f) == 5200 && summary_altitude_clip(9000.0f) == 7000 &&
            summary_altitude_clip(-300.0f) == -100,
        "altitude clips");
  CHECK(summary_speed_clip(42.4f) == 42 && summary_speed_clip(81.0f) == 79 && summary_speed_clip(83.0f) == 85 &&
            summary_speed_clip(143.0f) == 145,
        "speed clips %d %d", summary_speed_clip(81.0f), summary_speed_clip(83.0f));
  CHECK(summary_lift_clip(270.0f) == 250 && summary_lift_clip(60.0f) == 50 && summary_lift_clip(80.0f) == 100,
        "lift clips %d %d", summary_lift_clip(60.0f), summary_lift_clip(80.0f));
  CHECK(summary_sink_clip(1110.0f) == 1150 && summary_sink_clip(1090.0f) == 1050 && summary_sink_clip(3.0f) == 10,
        "sink clips %d %d", summary_sink_clip(1110.0f), summary_sink_clip(1090.0f));

  flight_stats_t s;
  summary_reset(&s);
  /* 1 h 23 min: climbing 2.5 m/s for a while, sinking 4 m/s, top 2345 m, fastest 12 m/s */
  for (int i = 0; i < 83 * 60; i++) {
    float vario = (i > 100 && i < 200) ? 2.5f : (i > 300 && i < 320) ? -4.0f : 0.3f;
    summary_update(&s, 1500.0f + (i == 2000 ? 845.0f : 0.0f), i == 1000 ? 12.0f : 8.0f, vario);
  }
  summary_items_t items = {true, true, true, true, true};
  char clips[SUMMARY_GROUPS][SUMMARY_CLIPS][SUMMARY_NAME];
  unsigned counts[SUMMARY_GROUPS];
  unsigned g = summary_clips(&s, &items, clips, counts);
  CHECK(g == 4, "summary groups %u", g);
  CHECK(counts[0] == 3 && strcmp(clips[0][0], "time/duration") == 0 && strcmp(clips[0][1], "time/duration_1") == 0 &&
            strcmp(clips[0][2], "time/min_23") == 0,
        "duration clips %s %s %s", clips[0][0], clips[0][1], clips[0][2]);
  CHECK(strcmp(clips[1][1], "al/al_2350") == 0, "altitude clip %s", clips[1][1]);
  CHECK(strcmp(clips[2][1], "sp/sp_43") == 0, "speed clip %s", clips[2][1]);
  CHECK(counts[3] == 4 && strcmp(clips[3][1], "vario_lift_250") == 0 && strcmp(clips[3][2], "pause") == 0 &&
            strcmp(clips[3][3], "vario_sink_400") == 0,
        "climb clips %s %s %s", clips[3][1], clips[3][2], clips[3][3]);
  items.enable = false;
  CHECK(summary_clips(&s, &items, clips, counts) == 0, "summary switched off");
  summary_reset(&s);
  items.enable = true;
  CHECK(summary_clips(&s, &items, clips, counts) == 0, "summary without a flight");

  CHECK(crc32_update(crc32("1234", 4), "56789", 5) == 0xCBF43926U, "crc32 continued");
}

/* ---- SHA-256 / HMAC-SHA-256 (FIPS 180-4 and RFC 4231 test vectors) ---- */

static bool digest_is(const uint8_t *d, const char *hex)
{
  static const char digits[] = "0123456789abcdef";
  for (int i = 0; i < SHA256_SIZE; i++) {
    if (hex[2 * i] != digits[d[i] >> 4] || hex[2 * i + 1] != digits[d[i] & 15]) {
      return false;
    }
  }
  return true;
}

static void test_sha256(void)
{
  uint8_t d[SHA256_SIZE];
  sha256_t s;
  sha256_init(&s);
  sha256_update(&s, "abc", 3);
  sha256_final(&s, d);
  CHECK(digest_is(d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256(abc)");

  const char *two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  sha256_init(&s);
  for (const char *p = two_blocks; *p; p++) {
    sha256_update(&s, p, 1); /* byte by byte: exercises the buffering */
  }
  sha256_final(&s, d);
  CHECK(digest_is(d, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "sha256 two blocks");

  hmac_sha256_t h;
  hmac_sha256_init(&h, (const uint8_t *)"Jefe", 4);
  hmac_sha256_update(&h, "what do ya want for nothing?", 28);
  hmac_sha256_final(&h, d);
  CHECK(digest_is(d, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"), "hmac rfc4231 case 2");

  uint8_t key[131];
  memset(key, 0xaa, sizeof(key));
  hmac_sha256_init(&h, key, sizeof(key));
  hmac_sha256_update(&h, "Test Using Larger Than Block-Size Key - Hash Key First", 54);
  hmac_sha256_final(&h, d);
  CHECK(digest_is(d, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"), "hmac rfc4231 case 6");
}

/* Several satellite systems: one GSA per system; one without satellites must not lower the fix. */
static void test_nmea_multi(void)
{
  nmea_epoch_t e;
  gps_fix_t fix;
  char buf[128];
  char *f[24];
  int n;
  nmea_epoch_init(&e);
  n = fields(buf, sizeof(buf), "GNGGA,120512.00,4612.3456,N,01120.7890,E,1,14,0.7,1305.3,M,46.4,M,,", f, 24);
  nmea_epoch_gga(&e, f, n);
  n = fields(buf, sizeof(buf), "GNGSA,A,3,01,02,03,04,05,06,07,08,,,,,1.2,0.7,1.0,1", f, 24);
  nmea_epoch_gsa(&e, f, n);
  n = fields(buf, sizeof(buf), "GNGSA,A,1,,,,,,,,,,,,,1.2,0.7,1.0,3", f, 24);
  nmea_epoch_gsa(&e, f, n);
  n = fields(buf, sizeof(buf), "GNRMC,120512.00,A,4612.3456,N,01120.7890,E,10.0,90.0,081026,,,A", f, 24);
  CHECK(nmea_epoch_rmc(&e, f, n, &fix) && fix.fix_type == 3 && fix.sats == 14,
        "multi-GNSS epoch: type %u, %u satellites", fix.fix_type, fix.sats);
}

/* 10 Hz: every epoch a fix; the vertical speed over 1 s, not from the 0.1 m steps of 0.1 s. */
static void test_nmea_10hz(void)
{
  nmea_epoch_t e;
  gps_fix_t fix;
  char buf[128];
  char *f[24];
  int n;
  int fixes = 0;
  bool short_span = false;
  nmea_epoch_init(&e);
  for (int k = 0; k < 25; k++) {
    char line[100];
    int cs = k * 10; /* centiseconds after 12:05:12 */
    /* climbing 1 m/s, the height in 0.1 m steps; one epoch 0.3 m off (noise); no float printf here */
    int alt_dm = 13050 + k + (k == 14 ? 3 : 0);
    snprintf(line, sizeof(line), "GNGGA,1205%02d.%02d,4612.3456,N,01120.7890,E,1,14,0.7,%d.%d,M,46.4,M,,", 12 + cs / 100,
             cs % 100, alt_dm / 10, alt_dm % 10);
    n = fields(buf, sizeof(buf), line, f, 24);
    nmea_epoch_gga(&e, f, n);
    n = fields(buf, sizeof(buf), "GNGSA,A,3,01,02,03,04,05,06,07,08,,,,,1.2,0.7,1.0,1", f, 24);
    nmea_epoch_gsa(&e, f, n);
    snprintf(line, sizeof(line), "GNRMC,1205%02d.%02d,A,4612.3456,N,01120.7890,E,10.0,90.0,081026,,,A", 12 + cs / 100,
             cs % 100);
    n = fields(buf, sizeof(buf), line, f, 24);
    if (!nmea_epoch_rmc(&e, f, n, &fix)) {
      continue;
    }
    fixes++;
    CHECK(fix.ms == (uint16_t)(cs % 100 * 10), "10 Hz fix time .%03u", fix.ms);
    if (k < 9) {
      short_span = short_span || fix.vel_d_valid;
    } else if (k != 14 && k != 23) {
      /* 1 m/s up; the epochs 1 s after and at the outlier are 0.3 m/s off */
      CHECK(fix.vel_d_valid && fabsf(fix.vel_d_ms + 1.0f) < 0.05f, "10 Hz vertical speed at %d: %d mm/s", k,
            (int)(fix.vel_d_ms * 1000.0f));
    }
  }
  CHECK(fixes == 25, "10 Hz: %d fixes of 25", fixes);
  CHECK(!short_span, "vertical speed from less than 0.9 s");

  /* the 1 Hz users: one fix per UTC second, epochs not aligned to the second (108 ms apart) */
  nmea_rate_t sec = {0};
  gps_fix_t fx = {0};
  int taken = 0;
  uint32_t last_taken = 0;
  bool twice = false;
  for (int k = 0; k < 100; k++) {
    uint32_t t_ms = 190U + 108U * (uint32_t)k;
    fx.unix_time = 1000000U + t_ms / 1000U;
    fx.ms = (uint16_t)(t_ms % 1000U);
    fx.rx_ms = 5000U + t_ms;
    if (nmea_rate_due(&sec, &fx, 1U)) {
      twice = twice || (taken && fx.unix_time == last_taken);
      last_taken = fx.unix_time;
      taken++;
    }
  }
  /* 0.19 s .. 10.88 s: the seconds 0 to 10 */
  CHECK(taken == 11 && !twice, "whole seconds at 10 Hz: %d", taken);
  /* without a valid time: the device clock */
  sec = (nmea_rate_t){0};
  taken = 0;
  for (int k = 0; k < 100; k++) {
    fx.unix_time = 0;
    fx.ms = 0;
    fx.rx_ms = 5000U + 108U * (uint32_t)k;
    taken += nmea_rate_due(&sec, &fx, 1U);
  }
  CHECK(taken >= 10 && taken <= 12, "seconds without time at 10 Hz: %d", taken);
  /* 1 Hz: every fix, also with some jitter on the device clock */
  sec = (nmea_rate_t){0};
  taken = 0;
  for (int k = 0; k < 20; k++) {
    fx.unix_time = 0;
    fx.rx_ms = 5000U + 1000U * (uint32_t)k + (k % 2 ? 30U : 0U);
    taken += nmea_rate_due(&sec, &fx, 1U);
  }
  CHECK(taken == 20, "1 Hz without time: %d of 20", taken);
  /* 4 Hz out of 10 Hz: .0 .3 .5 .8 of each second, the first of each second included */
  sec = (nmea_rate_t){0};
  nmea_rate_t whole = {0};
  taken = 0;
  bool first_missing = false;
  uint32_t mask = 0;
  for (int k = 0; k < 30; k++) {
    fx.unix_time = 1000000U + (uint32_t)k / 10U;
    fx.ms = (uint16_t)(k % 10 * 100);
    fx.rx_ms = 5000U + 100U * (uint32_t)k;
    bool due = nmea_rate_due(&sec, &fx, 4U);
    first_missing = first_missing || (nmea_rate_due(&whole, &fx, 1U) && !due);
    taken += due;
    if (due && k < 10) {
      mask |= 1U << k;
    }
  }
  CHECK(taken == 12 && !first_missing && mask == 0x129U, "4 Hz of 10 Hz: %d, mask %03lx", taken,
        (unsigned long)mask);
}

/* ---- double tap ---- */

static uint32_t tap_noise_state = 12345U;

static float tap_noise(float amp) /* deterministic, in [-amp, amp] */
{
  tap_noise_state = tap_noise_state * 1664525U + 1013904223U;
  return amp * ((float)(tap_noise_state >> 8) / 8388608.0f - 1.0f);
}

/*
 * 100 Hz samples for `seconds`: 1 g on z with noise, a knock on x (spike and ringing) at each
 * time in taps_s, and a 15 Hz vibration on x. Returns the number of double taps and the sample
 * of the first one in *at.
 */
static float tap_gyro_dps; /* rotation reported with every sample of tap_run */
static float tap_load_g = 1.0f; /* the steady load on z */

static int tap_run(int sensitivity, const float *taps_s, int ntaps, float knock_g, float vib_g, int seconds, int *at)
{
  tap_t t;
  tap_init(&t, sensitivity);
  int found = 0;
  *at = -1;
  for (int i = 0; i < seconds * 100; i++) {
    float a[3] = {tap_noise(0.003f), tap_noise(0.003f), tap_load_g + tap_noise(0.003f)};
    a[0] += vib_g * sinf(2.0f * 3.14159265f * 15.0f * (float)i / 100.0f);
    for (int k = 0; k < ntaps; k++) {
      int s = (int)(taps_s[k] * 100.0f + 0.5f);
      if (i == s) {
        a[0] += knock_g;
      } else if (i == s + 1) {
        a[0] -= 0.4f * knock_g;
      }
    }
    if (tap_update(&t, a, tap_gyro_dps)) {
      if (!found) {
        *at = i;
      }
      found++;
    }
  }
  return found;
}

static void test_tap(void)
{
  int at;
  const float two[] = {1.0f, 1.25f};
  CHECK(tap_run(3, two, 2, 2.5f, 0.0f, 3, &at) == 1 && at >= 150 && at <= 156,
        "double tap found once, 250 ms after the second tap (sample %d)", at);
  const float one[] = {1.0f};
  CHECK(tap_run(3, one, 1, 2.5f, 0.0f, 3, &at) == 0, "single tap");
  const float slow[] = {1.0f, 1.7f};
  CHECK(tap_run(3, slow, 2, 2.5f, 0.0f, 3, &at) == 0, "taps 700 ms apart");
  const float fast[] = {1.0f, 1.08f};
  CHECK(tap_run(3, fast, 2, 2.5f, 0.0f, 3, &at) == 0, "taps 80 ms apart");
  const float three[] = {1.0f, 1.25f, 1.5f};
  CHECK(tap_run(3, three, 3, 2.5f, 0.0f, 3, &at) == 0, "triple tap");
  const float again[] = {1.0f, 1.25f, 2.5f, 2.8f};
  CHECK(tap_run(3, again, 4, 2.5f, 0.0f, 4, &at) == 2, "two double taps");
  CHECK(tap_run(0, two, 2, 0.7f, 0.0f, 3, &at) == 1, "light double tap, sensitivity 0 (extremely easy)");
  CHECK(tap_run(4, two, 2, 1.5f, 0.0f, 3, &at) == 0, "medium double tap, sensitivity 4 (difficult)");
  CHECK(tap_run(0, NULL, 0, 0.0f, 0.3f, 10, &at) == 0, "vibration alone, sensitivity 0");
  const float later[] = {2.0f, 2.25f};
  CHECK(tap_run(3, later, 2, 3.0f, 0.3f, 4, &at) == 1, "hard double tap during vibration");
  CHECK(tap_run(0, later, 2, 0.6f, 0.3f, 4, &at) == 0, "light double tap hidden by vibration");
  /* calm gate: the same double tap while the device turns or carries a load is handling */
  tap_gyro_dps = 400.0f;
  CHECK(tap_run(3, two, 2, 2.5f, 0.0f, 3, &at) == 0, "double tap while rotating at 400 deg/s: no");
  tap_gyro_dps = 100.0f;
  CHECK(tap_run(3, two, 2, 2.5f, 0.0f, 3, &at) == 1, "double tap while rotating at 100 deg/s: yes");
  tap_gyro_dps = 0.0f;
  tap_load_g = 2.5f;
  CHECK(tap_run(3, two, 2, 2.5f, 0.0f, 3, &at) == 0, "double tap under 2.5 g: no");
  tap_load_g = 1.3f;
  CHECK(tap_run(3, two, 2, 2.5f, 0.0f, 3, &at) == 1, "double tap in a 1.3 g turn: yes");
  tap_load_g = 1.0f;
}

/* ---- announcements ---- */

static bool group_is(const announcement_t *a, unsigned g, const char *c0, const char *c1, const char *c2)
{
  const char *want[3] = {c0, c1, c2};
  unsigned n = c2 ? 3U : c1 ? 2U : 1U;
  if (g >= a->groups || a->count[g] != n) {
    return false;
  }
  for (unsigned i = 0; i < n; i++) {
    if (strcmp(a->clip[g][i], want[i]) != 0) {
      return false;
    }
  }
  return true;
}

static void test_announce(void)
{
  announce_list_t all;
  announce_list_parse(&all, "altitude,climbrate,speed:ble, heading,duration,time,battery");
  CHECK(all.set && all.count == 7 && all.block[2] == ANN_SPEED && all.block[3] == ANN_HEADING && all.ble_mute == (1U << 2),
        "list parse: %u blocks, ble %x", all.count, all.ble_mute);
  announce_input_t in = {0};
  in.in_flight = true;
  in.alt_m = 1234.0f;
  in.climb_ms[1] = 1.27f; /* over 10 s, the default span */
  in.fix = true;
  in.speed_kmh = 42.4f;
  in.course_deg = 100.0f;
  in.airtime_s = 3725U;
  in.time_valid = true;
  in.local_s = 14U * 3600U + 35U * 60U + 10U;
  in.battery_percent = 63;
  announcement_t a;
  announce_build(&all, false, &in, &a);
  CHECK(a.groups == 7, "all blocks with data: %u groups", a.groups);
  CHECK(group_is(&a, 0, "al/al_1230", NULL, NULL), "altitude %s", a.clip[0][0]);
  CHECK(group_is(&a, 1, "vario_lift_150", NULL, NULL), "climb %s", a.clip[1][0]);
  CHECK(group_is(&a, 2, "sp/sp_42", NULL, NULL), "speed %s", a.clip[2][0]);
  CHECK(group_is(&a, 3, "nav/heading", "nav/e", NULL), "heading %s", a.clip[3][1]);
  CHECK(group_is(&a, 4, "time/duration", "time/duration_1", "time/min_2"), "duration %s %s", a.clip[4][1], a.clip[4][2]);
  CHECK(group_is(&a, 5, "time/time", "time/hour_14", "time/min_35"), "time %s %s", a.clip[5][1], a.clip[5][2]);
  CHECK(group_is(&a, 6, "battery", "75_percent", NULL), "battery %s", a.clip[6][1]);

  in.climb_ms[1] = -1.3f;
  in.course_deg = 350.0f;
  in.airtime_s = 125U;
  announce_build(&all, false, &in, &a);
  CHECK(group_is(&a, 1, "vario_sink_150", NULL, NULL), "sink %s", a.clip[1][0]);
  CHECK(group_is(&a, 3, "nav/heading", "nav/n", NULL), "heading 350 %s", a.clip[3][1]);
  CHECK(group_is(&a, 4, "time/duration", "time/min_2", NULL), "duration under an hour %s", a.clip[4][1]);
  in.course_deg = 337.0f;
  announce_build(&all, false, &in, &a);
  CHECK(group_is(&a, 3, "nav/heading", "nav/nw", NULL), "heading 337 %s", a.clip[3][1]);

  /* without a fix: no speed or heading; on the ground: no flight time */
  in.fix = false;
  in.in_flight = false;
  announce_build(&all, false, &in, &a);
  CHECK(a.groups == 4 && group_is(&a, 0, "al/al_1230", NULL, NULL) && group_is(&a, 2, "time/time", "time/hour_14",
                                                                             "time/min_35"),
        "no fix, on the ground: %u groups", a.groups);
  /* blocks marked :ble are left out while a phone is connected */
  in.fix = true;
  in.in_flight = true;
  announce_build(&all, true, &in, &a);
  CHECK(a.groups == 6 && group_is(&a, 2, "nav/heading", "nav/nw", NULL), "BLE leaves out the speed: %u groups", a.groups);

  /* the signal tone of the block "tone": ":<name>" in any order with ":ble", the first without one */
  announce_list_t sl;
  announce_list_parse(&sl, "altitude,tone:ble:trill");
  CHECK(sl.count == 2 && ANN_OPT_VARIANT(sl.opt[1]) == 5 && sl.ble_mute == (1U << 1), "tone:ble:trill -> %u, ble %x",
        ANN_OPT_VARIANT(sl.opt[1]), sl.ble_mute);
  announce_list_parse(&sl, "tone : low_chirp_up : ble,altitude");
  CHECK(sl.count == 2 && ANN_OPT_VARIANT(sl.opt[0]) == 10 && sl.ble_mute == 1U, "tone:low_chirp_up:ble -> %u, ble %x",
        ANN_OPT_VARIANT(sl.opt[0]), sl.ble_mute);
  announce_list_parse(&sl, "tone:nonsense,altitude:blex");
  CHECK(sl.count == 2 && sl.opt[0] == 0 && sl.opt[1] == 0 && sl.ble_mute == 0U, "unknown options ignored: %u, ble %x",
        sl.opt[0], sl.ble_mute);
  announce_list_parse(&sl, "climbrate,tone:tock_tock");
  announce_build(&sl, false, &in, &a);
  CHECK(a.groups == 2 && a.count[1] == 0 && a.signal[1] == 9 && strcmp(announce_signal_names[a.signal[1]], "tock_tock") == 0,
        "the tone group carries its signal: %u", a.signal[1]);

  /* the options of 0.58: a span "<n>s" and a word before the number, in any order with ":ble" */
  announce_list_parse(&sl, "climbrate:ble:30s:climb,thermalgain:heightgain:60s,glide:15s,altitude:60s:climb,tone:10s");
  CHECK(sl.count == 5 && sl.ble_mute == 1U && ANN_OPT_SPAN(sl.opt[0]) == 4 && ANN_OPT_VARIANT(sl.opt[0]) == 1 &&
            ANN_OPT_SPAN(sl.opt[1]) == 5 && ANN_OPT_VARIANT(sl.opt[1]) == 3 && ANN_OPT_SPAN(sl.opt[2]) == 2 &&
            ANN_OPT_VARIANT(sl.opt[2]) == 0 && ANN_OPT_SPAN(sl.opt[4]) == 1,
        "options: spans %u %u %u, words %u %u", ANN_OPT_SPAN(sl.opt[0]), ANN_OPT_SPAN(sl.opt[1]), ANN_OPT_SPAN(sl.opt[2]),
        ANN_OPT_VARIANT(sl.opt[0]), ANN_OPT_VARIANT(sl.opt[1]));
  CHECK(announce_list_span_s(&sl, ANN_CLIMBRATE, 10) == 30 && announce_list_span_s(&sl, ANN_GLIDE, 20) == 15 &&
            announce_list_span_s(&sl, ANN_TONE, 0) == 10 && announce_list_span_s(&sl, ANN_SPEED, 20) == 0,
        "the span of a block in the list, 0 when it is not there");
  announce_list_parse(&sl, "glide:25s:gains,climbrate:9s");
  CHECK(sl.count == 2 && sl.opt[0] == 0 && sl.opt[1] == 0 && announce_list_span_s(&sl, ANN_GLIDE, 20) == 20,
        "spans and words that do not exist are ignored: %u %u", sl.opt[0], sl.opt[1]);
  for (unsigned k = 0; k < ANN_SPANS; k++) {
    in.climb_ms[k] = 0.1f * (float)k + 1.0f; /* 1.0 1.1 1.2 1.3 1.4 1.5: the span tells */
    in.gain_m[k] = 20.0f * (float)k;
    in.glide_valid[k] = true;
    in.glide_ratio[k] = 5.0f + (float)k;
  }
  in.thermal_valid = true;
  in.thermal_gain_m = 347.0f;
  announce_list_parse(&sl, "climbrate,thermalgain,glide");
  announce_build(&sl, false, &in, &a);
  CHECK(group_is(&a, 0, "vario_lift_100", NULL, NULL) && group_is(&a, 1, "al/al_350", NULL, NULL) && group_is(&a, 2, "8", "dot", "0"),
        "defaults: the climb over 10 s (%s), the phase's gain (%s), the glide over 20 s (%s)", a.clip[0][0], a.clip[1][0],
        a.clip[2][0]);
  announce_list_parse(&sl, "climbrate:30s:climb,thermalgain:60s:heightgain,glide:60s");
  announce_build(&sl, false, &in, &a);
  CHECK(group_is(&a, 0, "climb", "vario_lift_150", NULL) && group_is(&a, 1, "height_gain", "al/al_100", NULL) &&
            group_is(&a, 2, "10", "dot", "0"),
        "spans and words: %s %s, %s %s, %s", a.clip[0][0], a.clip[0][1], a.clip[1][0], a.clip[1][1], a.clip[2][0]);
  announce_list_parse(&sl, "climbrate:15s:gain,thermalgain:30s:climb,glide:30s");
  in.glide_valid[4] = false;
  announce_build(&sl, false, &in, &a);
  CHECK(a.groups == 2 && group_is(&a, 0, "vario_lift_100", NULL, NULL) && group_is(&a, 1, "al/al_80", NULL, NULL),
        "a word of another block is not said; 1.2 rounds to the 1.0 clip; no glide over 30 s: %u groups", a.groups);
  in.thermal_valid = false;
  announce_list_parse(&sl, "thermalgain:60s:gain,thermalgain");
  announce_build(&sl, false, &in, &a);
  CHECK(a.groups == 1 && group_is(&a, 0, "gain", "al/al_100", NULL), "the last 60 s need no thermal: %u groups", a.groups);
  in.gain_m[5] = -23.0f;
  announce_build(&sl, false, &in, &a);
  CHECK(group_is(&a, 0, "gain", "al/al_-20", NULL), "a loss is said as such: %s", a.clip[0][1]);
  in.in_flight = false;
  announce_build(&sl, false, &in, &a);
  CHECK(a.groups == 0, "no height gained on the ground");
  in.in_flight = true;
  /* the glide ratio as one clip of the extension series, rounded to the steps that exist */
  in.glide_series = true;
  in.glide_word = true;
  in.glide_ratio[3] = 8.66f;
  announce_list_parse(&sl, "glide");
  announce_build(&sl, false, &in, &a);
  CHECK(group_is(&a, 0, "glide_ratio", "glide/glide_87", NULL), "glide series 8.66: %s %s", a.clip[0][0], a.clip[0][1]);
  in.glide_word = false;
  in.glide_ratio[3] = 3.3f;
  announce_build(&sl, false, &in, &a);
  CHECK(group_is(&a, 0, "glide/glide_35", NULL, NULL), "glide series 3.3 -> halves: %s", a.clip[0][0]);
  in.glide_ratio[3] = 15.2f;
  announce_build(&sl, false, &in, &a);
  CHECK(group_is(&a, 0, "glide/glide_150", NULL, NULL), "glide series 15.2 -> halves: %s", a.clip[0][0]);
  in.glide_ratio[3] = 27.0f;
  announce_build(&sl, false, &in, &a);
  CHECK(group_is(&a, 0, "glide/glide_270", NULL, NULL), "glide series 27 -> whole: %s", a.clip[0][0]);
  in.glide_ratio[3] = 48.0f;
  announce_build(&sl, false, &in, &a);
  CHECK(group_is(&a, 0, "glide/glide_300", NULL, NULL), "glide series beyond 30: %s", a.clip[0][0]);
  CHECK(summary_glide_clip(12.96f) == 130 && summary_glide_clip(13.2f) == 130 && summary_glide_clip(13.3f) == 135 &&
            summary_glide_clip(5.04f) == 50 && summary_glide_clip(5.06f) == 51 && summary_glide_clip(0.2f) == 0 &&
            summary_glide_clip(22.4f) == 220 && summary_glide_clip(22.6f) == 230,
        "glide clips at the edges of the ranges");
  in.glide_series = false;
  for (unsigned k = 0; k < ANN_SPANS; k++) {
    in.climb_ms[k] = 1.27f;
    in.gain_m[k] = 0.0f;
    in.glide_valid[k] = false;
  }
  in.thermal_gain_m = 0.0f;

  /* the blocks of 0.24: tone, thermal, g-load, bank, summary, glide; unknown names (thermalclimb since 0.58) are skipped */
  announce_list_t l;
  announce_list_parse(&l, "tone, thermalclimb,thermalgain,maxg:ble,bank,maxaltitude,maxspeed,maxclimb,glide,nonsense,tone");
  CHECK(l.count == 8 && (l.ble_mute & (1U << 2)), "unknown and repeated blocks skipped: %u", l.count);
  in.thermal_valid = true;
  in.thermal_gain_m = 347.0f;
  in.g_load = 2.3f;
  in.bank_deg = 64.2f;
  in.stats_valid = true;
  in.max_alt_m = 2345.0f;
  in.max_speed_kmh = 43.2f;
  in.max_climb_ms = 2.5f;
  in.max_sink_ms = 4.0f;
  in.glide_valid[3] = true;
  in.glide_ratio[3] = 8.66f;
  in.g_word = true;
  announce_build(&l, false, &in, &a);
  CHECK(a.groups == 8 && a.count[0] == 0 && a.signal[0] == 0, "tone group (the default signal) and %u groups", a.groups);
  CHECK(group_is(&a, 1, "al/al_350", NULL, NULL), "thermal gain %s", a.clip[1][0]);
  CHECK(a.count[2] == 4 && strcmp(a.clip[2][0], "g_load") == 0 && strcmp(a.clip[2][1], "2") == 0 &&
            strcmp(a.clip[2][2], "dot") == 0 && strcmp(a.clip[2][3], "3") == 0,
        "g-load 2 dot 3: %s %s %s %s", a.clip[2][0], a.clip[2][1], a.clip[2][2], a.clip[2][3]);
  CHECK(group_is(&a, 3, "60", NULL, NULL), "bank 64 -> clip %s", a.clip[3][0]);
  CHECK(group_is(&a, 4, "altitude_max", "al/al_2350", NULL), "max altitude %s", a.clip[4][1]);
  CHECK(group_is(&a, 5, "speed_max", "sp/sp_43", NULL), "max speed %s", a.clip[5][1]);
  CHECK(a.count[6] == 4 && strcmp(a.clip[6][0], "vz_max") == 0 && strcmp(a.clip[6][1], "vario_lift_250") == 0 &&
            strcmp(a.clip[6][2], "pause") == 0 && strcmp(a.clip[6][3], "vario_sink_400") == 0,
        "max climb, pause, sink: %s %s %s", a.clip[6][1], a.clip[6][2], a.clip[6][3]);
  CHECK(group_is(&a, 7, "8", "dot", "7"), "glide %s %s %s", a.clip[7][0], a.clip[7][1], a.clip[7][2]);
  /* with the g/g_<tenths> series the load factor is one clip, above 10 g the last one */
  in.g_series = true;
  announce_build(&l, false, &in, &a);
  CHECK(group_is(&a, 2, "g_load", "g/g_23", NULL), "g-load series: %s %s", a.clip[2][0], a.clip[2][1]);
  in.g_word = false;
  in.g_load = 12.4f;
  announce_build(&l, false, &in, &a);
  CHECK(group_is(&a, 2, "g/g_100", NULL, NULL), "g-load series above 10 g: %s", a.clip[2][0]);
  in.g_load = 0.96f;
  announce_build(&l, false, &in, &a);
  CHECK(group_is(&a, 2, "g/g_10", NULL, NULL), "g-load series rounds to tenths: %s", a.clip[2][0]);
  in.g_series = false;
  in.g_word = true;
  in.g_load = 2.3f;
  /* with the bank/bank_<deg> series the bank angle is one clip, rounded to 5 degrees */
  in.bank_series = true;
  in.bank_word = true;
  announce_build(&l, false, &in, &a);
  CHECK(group_is(&a, 3, "bank_angle", "bank/bank_65", NULL), "bank series 64.2: %s %s", a.clip[3][0], a.clip[3][1]);
  in.bank_word = false;
  in.bank_deg = 62.4f;
  announce_build(&l, false, &in, &a);
  CHECK(group_is(&a, 3, "bank/bank_60", NULL, NULL), "bank series 62.4: %s", a.clip[3][0]);
  in.bank_deg = 89.9f;
  announce_build(&l, false, &in, &a);
  CHECK(group_is(&a, 3, "bank/bank_90", NULL, NULL), "bank series 89.9: %s", a.clip[3][0]);
  in.bank_deg = 133.0f; /* a measured roll angle over the top */
  announce_build(&l, false, &in, &a);
  CHECK(group_is(&a, 3, "bank/bank_135", NULL, NULL), "bank series 133: %s", a.clip[3][0]);
  in.bank_deg = 200.0f;
  announce_build(&l, false, &in, &a);
  CHECK(group_is(&a, 3, "bank/bank_180", NULL, NULL), "bank series above 180: %s", a.clip[3][0]);
  in.bank_deg = 0.0f;
  announce_build(&l, false, &in, &a);
  CHECK(group_is(&a, 3, "bank/bank_0", NULL, NULL), "bank series 0: %s", a.clip[3][0]);
  in.bank_series = false;
  in.bank_deg = 31.0f;
  in.g_load = 0.0f;
  announce_build(&l, true, &in, &a);
  CHECK(a.groups == 6, "no load: no g-load and bank, %u groups", a.groups);

  /* the demo values of the configurator's play button give every block something to say */
  announce_list_t every;
  announce_list_parse(&every, "altitude,climbrate,speed,heading,duration,time,wind,glide,battery,tone");
  announce_input_t demo = {0};
  demo.glide_word = true;
  announce_demo_input(&demo);
  announce_build(&every, false, &demo, &a);
  CHECK(a.groups == 10 && demo.glide_word && group_is(&a, 7, "glide_ratio", "8", "dot") == false &&
            a.count[7] == 4 && strcmp(a.clip[7][0], "glide_ratio") == 0,
        "demo: %u groups, glide %s", a.groups, a.clip[7][0]);
  announce_list_parse(&every, "thermalgain,maxg,bank,maxaltitude,maxspeed,maxclimb,climbrate:30s:climb,thermalgain:60s:gain,glide:60s");
  announce_build(&every, false, &demo, &a);
  CHECK(a.groups == 8 && group_is(&a, 6, "climb", "vario_lift_150", NULL) && group_is(&a, 0, "al/al_350", NULL, NULL),
        "demo: the thermal, g-load and summary blocks, every span, %u groups", a.groups);
  demo.wind_words = true;
  demo.glide_series = true;
  announce_list_parse(&every, "wind,glide,thermalgain:60s:heightgain");
  announce_build(&every, false, &demo, &a);
  CHECK(a.groups == 3 && group_is(&a, 0, "wind", "sp/sp_12", "nav/from_w") && group_is(&a, 1, "glide_ratio", "glide/glide_87", NULL) &&
            group_is(&a, 2, "height_gain", "al/al_90", NULL),
        "demo with the wind words and the glide series: %s %s %s, %s", a.clip[0][0], a.clip[0][1], a.clip[0][2], a.clip[1][1]);

  /* lists without a key: derived from the stock keys */
  announce_config_t c;
  announce_config_defaults(&c);
  c.tap_item[MATRIX_ALTITUDE] = true;
  c.tap_item[MATRIX_CLIMBRATE] = true;
  c.tap_item[MATRIX_SPEED] = true;
  c.auto_item[MATRIX_TIME] = true;
  c.enable[TRIG_TAP] = c.enable_set[TRIG_TAP] = true;
  c.enable[TRIG_AUTO] = c.enable_set[TRIG_AUTO] = true;
  c.summary_item[2] = false; /* flightsummary_speed_bool=false */
  announce_list_parse(&c.list[TRIG_WIND], "wind,altitude");
  c.enable[TRIG_WIND] = c.enable_set[TRIG_WIND] = true;
  announce_config_resolve(&c);
  CHECK(c.list[TRIG_TAP].count == 3 && c.list[TRIG_TAP].block[0] == ANN_ALTITUDE && c.list[TRIG_TAP].block[1] == ANN_CLIMBRATE &&
            c.list[TRIG_TAP].block[2] == ANN_SPEED && c.list[TRIG_TAP].ble_mute == 5U,
        "tap list from the matrix: %u blocks, ble %x", c.list[TRIG_TAP].count, c.list[TRIG_TAP].ble_mute);
  CHECK(c.list[TRIG_AUTO].count == 1 && c.list[TRIG_AUTO].block[0] == ANN_TIME && c.list[TRIG_ELEVATION].count == 1 &&
            c.enable[TRIG_ELEVATION],
        "time/distance and elevation lists from voiceauto_*");
  CHECK(c.list[TRIG_SUMMARY].count == 3 && c.enable[TRIG_SUMMARY] && c.list[TRIG_SUMMARY].block[2] == ANN_MAXCLIMB,
        "summary list from flightsummary_*: %u", c.list[TRIG_SUMMARY].count);
  CHECK(c.list[TRIG_THERMALEXIT].count == 1 && c.list[TRIG_THERMALEXIT].block[0] == ANN_TONE && c.enable[TRIG_THERMALEXIT] &&
            !c.enable[TRIG_THERMALING] && c.enable[TRIG_GLIDE] && !c.enable[TRIG_WIND] == false &&
            c.list[TRIG_WIND].count == 2 && c.list[TRIG_WINGOVER].count == 2 && c.enable[TRIG_WINGOVER],
        "event triggers resolved");
  CHECK(announce_elevation_gain(&c) == 0 && announce_elevation_loss(&c) == 0, "elevation off without a stock trigger");
  c.alt_trigger_m = 100;
  c.elevation_loss_m = 50;
  CHECK(announce_elevation_gain(&c) == 100 && announce_elevation_loss(&c) == 50, "elevation thresholds");

  /* automatic triggers: 1000 m or 15 min; 100 m gained, 50 m lost; never within 30 s */
  c.time_trigger_min = 15;
  c.dist_trigger_m = 1000;
  announce_auto_t t;
  int32_t lat = 470000000, lon = 110000000;
  announce_auto_start(&t, 1000U, 1500.0f);
  CHECK(announce_auto_due(&t, &c, 1020U, 1650.0f, true, lat, lon, false) == 0, "altitude change within 30 s");
  CHECK(announce_auto_due(&t, &c, 1031U, 1650.0f, true, lat, lon, false) == ANN_DUE_ELEVATION, "150 m gained");
  CHECK(announce_auto_due(&t, &c, 1100U, 1700.0f, true, lat, lon, false) == 0, "50 m since the last one");
  unsigned due = 0;
  uint32_t s2 = 1101U;
  for (; s2 < 1200U && !due; s2++) {
    lat += 1000; /* 0.0001 deg north = 11.1 m per second */
    due = announce_auto_due(&t, &c, s2, 1700.0f, true, lat, lon, false);
  }
  CHECK(due == ANN_DUE_AUTO && s2 - 1U >= 1185U && s2 - 1U <= 1195U, "1000 m flown after %lu s", (unsigned long)(s2 - 1U));
  CHECK(announce_auto_due(&t, &c, s2 + 15U * 60U, 1700.0f, false, lat, lon, false) == ANN_DUE_AUTO, "15 min without a fix");
  CHECK(announce_auto_due(&t, &c, s2 + 15U * 60U + 1U, 1595.0f, false, lat, lon, false) == ANN_DUE_ELEVATION, "55 m lost");
  c.enable[TRIG_AUTO] = false;
  CHECK(announce_auto_due(&t, &c, s2 + 40U * 60U, 1595.0f, false, lat, lon, false) == 0, "time/distance switched off");
  /* the global gap holds a due trigger until it is over; a double tap restarts them */
  c.enable[TRIG_AUTO] = true;
  c.time_trigger_min = 1;
  c.dist_trigger_m = 0;
  announce_auto_start(&t, 5000U, 1000.0f);
  CHECK(announce_auto_due(&t, &c, 5061U, 1000.0f, true, lat, lon, true) == 0, "held while within the gap");
  CHECK(announce_auto_due(&t, &c, 5070U, 1000.0f, true, lat, lon, false) == ANN_DUE_AUTO, "fires once the gap is over");
  announce_auto_restart(&t, 5100U, 1000.0f);
  CHECK(announce_auto_due(&t, &c, 5130U, 1000.0f, true, lat, lon, false) == 0 &&
            announce_auto_due(&t, &c, 5161U, 1000.0f, true, lat, lon, false) == ANN_DUE_AUTO,
        "a double tap restarts the minute");
}

/* ---- thermal detector ---- */

typedef struct {
  unsigned climb, exit, glide, wind;
} thermal_counts_t;

static bool step_gyro_ok;    /* gyro input of thermal_step: off in the older tests */
static float step_gyro_dps;

/* One second of flight: airspeed vector (heading in deg) plus the wind; the altitude changes by `climb`. */
static void thermal_step(thermal_t *t, const thermal_config_t *c, uint32_t *s, float heading, float airspeed, float wind_e,
                         float wind_n, float *alt, float climb, thermal_counts_t *n)
{
  float h = heading / 57.29578f;
  *alt += climb;
  unsigned ev = thermal_update(t, c, (*s)++, true, airspeed * sinf(h) + wind_e, airspeed * cosf(h) + wind_n, *alt,
                               step_gyro_ok, step_gyro_dps);
  n->climb += (ev & THERMAL_CLIMB) ? 1U : 0U;
  n->exit += (ev & THERMAL_EXIT) ? 1U : 0U;
  n->glide += (ev & THERMAL_GLIDE) ? 1U : 0U;
  n->wind += (ev & THERMAL_WIND) ? 1U : 0U;
}

static void test_thermal(void)
{
  const thermal_config_t c = {20, 100, 60, 50, 20, 0, 0}; /* the glide ratio over its default 20 s */
  thermal_t t;
  thermal_counts_t n = {0};
  uint32_t s = 1000;
  float alt = 1000.0f, heading = 0.0f;

  /* glide north, a thermal (120 s circling at 18 deg/s, +1.5 m/s), glide north; wind 3 m/s towards east */
  thermal_init(&t);
  for (int i = 0; i < 30; i++) {
    thermal_step(&t, &c, &s, 0.0f, 10.0f, 3.0f, 0.0f, &alt, -1.0f, &n);
  }
  CHECK(!t.circling && n.climb == 0, "straight glide: not circling");
  for (int i = 0; i < 120; i++) {
    heading += 18.0f;
    thermal_step(&t, &c, &s, heading, 10.0f, 3.0f, 0.0f, &alt, 1.5f, &n);
    if (i == 25) {
      CHECK(t.circling, "circling after one turn");
    }
  }
  CHECK(n.climb >= 4 && n.climb <= 6, "climb reports every 20 s: %u", n.climb);
  CHECK(fabsf(t.thermal_ms - 1.5f) < 0.2f, "thermal mean %d cm/s", (int)(t.thermal_ms * 100.0f));
  CHECK(n.exit == 0 && n.wind == 0, "nothing else while circling");
  for (int i = 0; i < 60; i++) {
    thermal_step(&t, &c, &s, 0.0f, 10.0f, 3.0f, 0.0f, &alt, -1.2f, &n);
  }
  CHECK(!t.circling, "circling ended");
  CHECK(n.exit == 1, "thermal exit once: %u", n.exit);
  float kmh = 0.0f, from = 0.0f;
  CHECK(n.wind == 1 && thermal_wind(&t, &kmh, &from) && kmh > 9.0f && kmh < 12.5f && from > 255.0f && from < 285.0f,
        "wind 10.8 km/h from the west: %u, %d km/h from %d", n.wind, (int)kmh, (int)from);
  CHECK(n.glide == 1 && t.glide > 8.2f && t.glide < 9.2f, "glide ratio 8.7 once: %u, %d/10", n.glide,
        (int)(t.glide * 10.0f));

  /* the lift dies while still circling: exit while circling, not again when it stops */
  thermal_init(&t);
  memset(&n, 0, sizeof(n));
  for (int i = 0; i < 90; i++) {
    heading += 18.0f;
    thermal_step(&t, &c, &s, heading, 10.0f, 0.0f, 0.0f, &alt, 1.5f, &n);
  }
  CHECK(n.exit == 0, "no exit while climbing");
  for (int i = 0; i < 60; i++) {
    heading += 18.0f;
    thermal_step(&t, &c, &s, heading, 10.0f, 0.0f, 0.0f, &alt, 0.0f, &n);
  }
  CHECK(n.exit == 1 && t.circling, "exit while circling when the lift has gone: %u", n.exit);
  for (int i = 0; i < 30; i++) {
    thermal_step(&t, &c, &s, 0.0f, 10.0f, 0.0f, 0.0f, &alt, -1.0f, &n);
  }
  CHECK(n.exit == 1, "no second exit when the circling stops: %u", n.exit);

  /* a car in a roundabout: one turn in 12 s, level, no thermal */
  thermal_init(&t);
  memset(&n, 0, sizeof(n));
  heading = 0.0f;
  for (int i = 0; i < 60; i++) {
    thermal_step(&t, &c, &s, 0.0f, 8.0f, 0.0f, 0.0f, &alt, 0.0f, &n);
  }
  for (int i = 0; i < 12; i++) {
    heading += 30.0f;
    thermal_step(&t, &c, &s, heading, 8.0f, 0.0f, 0.0f, &alt, 0.0f, &n);
  }
  for (int i = 0; i < 60; i++) {
    thermal_step(&t, &c, &s, 0.0f, 8.0f, 0.0f, 0.0f, &alt, 0.0f, &n);
  }
  CHECK(n.climb == 0 && n.exit == 0 && n.glide == 0, "roundabout: no thermal events (%u %u %u)", n.climb, n.exit,
        n.glide);

  /* gyro only: no GNSS velocity, the gyro turns at 18 deg/s: circling after one turn, climb reports,
   * the circling ends when the gyro says straight */
  thermal_init(&t);
  memset(&n, 0, sizeof(n));
  bool circling_at_25 = false;
  for (int i = 0; i < 60; i++) {
    alt += 1.5f;
    unsigned ev = thermal_update(&t, &c, s++, false, 0.0f, 0.0f, alt, true, 18.0f);
    n.climb += (ev & THERMAL_CLIMB) ? 1U : 0U;
    if (i == 25) {
      circling_at_25 = t.circling;
    }
  }
  CHECK(circling_at_25 && t.circling, "gyro: circling without a GNSS track");
  CHECK(n.climb >= 2, "gyro: climb reports %u", n.climb);
  for (int i = 0; i < 30; i++) {
    alt -= 1.0f;
    thermal_update(&t, &c, s++, false, 0.0f, 0.0f, alt, true, 0.0f);
  }
  CHECK(!t.circling, "gyro: circling ends when the gyro says straight");

  /* the track says straight (a poor fix at low ground speed) while the gyro keeps turning: still circling */
  thermal_init(&t);
  memset(&n, 0, sizeof(n));
  heading = 0.0f;
  step_gyro_ok = true;
  step_gyro_dps = 18.0f;
  for (int i = 0; i < 40; i++) {
    heading += 18.0f;
    thermal_step(&t, &c, &s, heading, 10.0f, 0.0f, 0.0f, &alt, 1.0f, &n);
  }
  CHECK(t.circling, "gyro and track: circling");
  for (int i = 0; i < 20; i++) {
    thermal_step(&t, &c, &s, heading, 10.0f, 0.0f, 0.0f, &alt, 1.0f, &n);
  }
  CHECK(t.circling, "track straight, gyro turning: still circling");
  step_gyro_dps = 0.0f;
  for (int i = 0; i < 20; i++) {
    thermal_step(&t, &c, &s, heading, 10.0f, 0.0f, 0.0f, &alt, -1.0f, &n);
  }
  CHECK(!t.circling, "both straight: circling ended");
  step_gyro_ok = false;

  /* the glide ratio over longer spans: 60 s of straight flight after the thermal, not before */
  thermal_init(&t);
  memset(&n, 0, sizeof(n));
  heading = 0.0f;
  const thermal_config_t c60 = {20, 100, 60, 50, 30, 0, 60}; /* the glide block says "glide:60s" */
  for (int i = 0; i < 120; i++) {
    heading += 18.0f;
    thermal_step(&t, &c60, &s, heading, 10.0f, 0.0f, 0.0f, &alt, 1.5f, &n);
  }
  float ratio = 0.0f;
  for (int i = 0; i < 45; i++) {
    thermal_step(&t, &c60, &s, 0.0f, 10.0f, 0.0f, 0.0f, &alt, -1.0f, &n);
  }
  CHECK(!t.circling && n.glide == 0 && thermal_glide_over(&t, 20, &ratio) && ratio > 9.5f && ratio < 10.5f &&
            !thermal_glide_over(&t, 60, &ratio),
        "45 s after the thermal: the 20 s ratio is there (%d/10), the 60 s one not yet, no report", (int)(ratio * 10.0f));
  for (int i = 0; i < 20; i++) {
    thermal_step(&t, &c60, &s, 0.0f, 10.0f, 0.0f, 0.0f, &alt, -1.0f, &n);
  }
  CHECK(n.glide == 0 && !thermal_glide_over(&t, 60, &ratio),
        "65 s after the thermal: the end of the circling was seen about 58 s ago, the 60 s span still waits");
  for (int i = 0; i < 10; i++) {
    thermal_step(&t, &c60, &s, 0.0f, 10.0f, 0.0f, 0.0f, &alt, -1.0f, &n);
  }
  CHECK(n.glide == 1 && t.glide > 9.9f && t.glide < 10.1f && thermal_glide_over(&t, 60, &ratio) && thermal_glide_over(&t, 63, &ratio) &&
            !thermal_glide_over(&t, 0, &ratio),
        "the report waits for 60 s of straight flight and leaves the thermal out: %u, %d/10", n.glide, (int)(t.glide * 10.0f));
  heading = 0.0f;
  for (int i = 0; i < 10; i++) {
    heading += 10.0f; /* a 100 degree course change: the 20 s ratio goes, the 60 s one too */
    thermal_step(&t, &c60, &s, heading, 10.0f, 0.0f, 0.0f, &alt, -1.0f, &n);
  }
  CHECK(!t.circling && !thermal_glide_over(&t, 20, &ratio) && !thermal_glide_over(&t, 60, &ratio), "a turn of 100 deg: no glide ratio");

  /* announcements of the event items */
  announce_list_t items;
  announce_list_parse(&items, "wind,glide");
  announce_input_t in = {0};
  in.wind_valid = true;
  in.wind_kmh = 10.8f;
  in.wind_from_deg = 268.0f;
  in.glide_valid[3] = true;
  in.glide_ratio[3] = 8.66f;
  announcement_t a;
  announce_build(&items, false, &in, &a);
  CHECK(a.groups == 2 && group_is(&a, 0, "nav/wind", "nav/w", "sp/sp_11"), "wind %s %s", a.clip[0][1], a.clip[0][2]);
  CHECK(a.count[1] == 3 && strcmp(a.clip[1][0], "8") == 0 && strcmp(a.clip[1][1], "dot") == 0 &&
            strcmp(a.clip[1][2], "7") == 0,
        "glide 8 dot 7: %s %s %s", a.clip[1][0], a.clip[1][1], a.clip[1][2]);
  in.glide_ratio[3] = 23.4f;
  in.glide_word = true;
  in.wind_kmh = 1.0f;
  announce_build(&items, false, &in, &a);
  CHECK(group_is(&a, 0, "nav/wind", "sp/sp_1", NULL), "calm wind without direction");
  CHECK(group_is(&a, 1, "glide_ratio", "23", NULL), "glide 23 with the word: %s %s", a.clip[1][0], a.clip[1][1]);
  /* with the extension clips: "wind", the speed, "from the west"; a calm wind has no direction */
  in.wind_words = true;
  in.wind_kmh = 10.8f;
  announce_build(&items, false, &in, &a);
  CHECK(group_is(&a, 0, "wind", "sp/sp_11", "nav/from_w"), "wind words: %s %s %s", a.clip[0][0], a.clip[0][1], a.clip[0][2]);
  in.wind_from_deg = 30.0f;
  in.wind_kmh = 1.5f;
  announce_build(&items, false, &in, &a);
  CHECK(group_is(&a, 0, "wind", "sp/sp_2", NULL), "wind words, calm: %s %s", a.clip[0][0], a.clip[0][1]);
  in.wind_kmh = 25.0f;
  announce_build(&items, false, &in, &a);
  CHECK(group_is(&a, 0, "wind", "sp/sp_25", "nav/from_ne"), "wind words from 30 deg: %s", a.clip[0][2]);
}

/* ---- QOA decoder: streaming (slice by slice) vs. the frame-at-once decoder of 0.16 ---- */

static uint8_t qoa_bytes[8 + 3 * QOA_FRAME_MAX];
static uint32_t qoa_len;

typedef struct {
  uint32_t pos;
} qoa_cursor_t;

static uint32_t qoa_mem(void *ctx, uint8_t *buf, uint32_t len)
{
  qoa_cursor_t *c = (qoa_cursor_t *)ctx;
  uint32_t n = qoa_len - c->pos < len ? qoa_len - c->pos : len;
  memcpy(buf, qoa_bytes + c->pos, n);
  c->pos += n;
  return n;
}

static void put_u64(uint8_t *p, uint64_t v)
{
  for (int i = 7; i >= 0; i--) {
    p[i] = (uint8_t)v;
    v >>= 8;
  }
}

/* A valid mono file of three frames (the last one short) with pseudo-random slices. */
static uint32_t make_qoa(void)
{
  static const uint32_t frames[] = {5120, 5120, 1234};
  uint32_t total = frames[0] + frames[1] + frames[2];
  uint32_t seed = 4242U;
  put_u64(qoa_bytes, ((uint64_t)0x716f6166U << 32) | total);
  uint32_t p = 8;
  for (int f = 0; f < 3; f++) {
    uint32_t slices = (frames[f] + 19U) / 20U;
    uint32_t size = 8U + 16U + slices * 8U;
    put_u64(qoa_bytes + p, ((uint64_t)1 << 56) | ((uint64_t)22050 << 32) | ((uint64_t)frames[f] << 16) | size);
    put_u64(qoa_bytes + p + 8, 0); /* history */
    put_u64(qoa_bytes + p + 16, ((uint64_t)(uint16_t)(-8192) << 16) | (uint16_t)16384); /* weights 0 0 -2^13 2^14 */
    for (uint32_t s = 0; s < slices; s++) {
      uint64_t v = 0;
      for (int k = 0; k < 2; k++) {
        seed = seed * 1664525U + 1013904223U;
        v = (v << 32) | seed;
      }
      v = (v & 0x0FFFFFFFFFFFFFFFULL) | ((uint64_t)(seed % 8U) << 60); /* scale factors 0..7 */
      put_u64(qoa_bytes + p + 24 + s * 8U, v);
    }
    p += size;
  }
  qoa_len = p;
  return total;
}

static void test_qoa(void)
{
  static qoa_ref_t ref;
  static qoa_stream_t qs;
  static int16_t a[1000], b[1000];
  uint32_t expected = make_qoa();
  qoa_cursor_t ca = {0}, cb = {0};
  CHECK(qoa_ref_open(&ref, qoa_mem, &ca) && qoa_open(&qs, qoa_mem, &cb), "QOA open");
  static const uint32_t chunks[] = {1, 37, 256, 999, 20, 5};
  uint32_t total = 0, mismatches = 0;
  for (unsigned k = 0;; k++) {
    uint32_t n = chunks[k % 6U];
    uint32_t na = qoa_ref_read(&ref, a, n);
    uint32_t nb = qoa_read(&qs, b, n);
    if (na != nb) {
      mismatches++;
      break;
    }
    for (uint32_t i = 0; i < na; i++) {
      mismatches += a[i] != b[i];
    }
    total += na;
    if (na < n) {
      break;
    }
  }
  CHECK(mismatches == 0 && total == expected, "streaming QOA decoder: %lu of %lu samples, %lu mismatches",
        (unsigned long)total, (unsigned long)expected, (unsigned long)mismatches);
}

const char *test_main(void)
{
  test_ini();
  test_time();
  test_crc();
  test_gesture();
  test_vario_filter();
  test_vario_tones();
  test_identity();
  test_console_pack();
  test_sha256();
  test_fusion();
  test_fusion_maneuvers();
  test_nmea();
  test_nmea_multi();
  test_nmea_10hz();
  test_flight();
  test_landing_motion();
  test_flight_thresholds();
  test_summer_time();
  test_tz_table();
  test_igc();
  test_recorder();
  test_battery();
  test_kml();
  test_summary();
  test_tap();
  test_announce();
  test_thermal();
  test_maneuver();
  test_tilt();
  test_qoa();
  report_len += (size_t)snprintf(report + report_len, sizeof(report) - report_len, "%d passed, %d failed\n",
                                 passed, failed);
  return report;
}
