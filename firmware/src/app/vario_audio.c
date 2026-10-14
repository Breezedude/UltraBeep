#include "vario_audio.h"

#include <stdbool.h>

#include "audio.h"
#include "indicator.h"
#include "sound.h"

#define IDLE_GIVE_MS  2000U /* silent this long: the piezo is given back */
#define PUMP_LEAD_MS  20U   /* the charge pump starts this long before a beep (as before melodies) */
#define PUMP_KEEP_MS  20U   /* pauses shorter than lead + keep leave it running */
#define HYSTERESIS_M_S 0.05f /* a running beep or sink tone continues this far past its threshold */

typedef enum { KIND_LIFT, KIND_SINK, KIND_WEAK } tone_kind_t;

static const vario_tones_t *volatile tones;
static volatile uint8_t level;
static volatile bool led_wanted;  /* LED mode "vario colours": the pattern runs even without sound */
static volatile bool weak_wanted; /* weak-lift tone allowed (in flight, or the configurator's test) */
static volatile bool sim_on;      /* configurator: a set climb rate instead of the sensors */
static volatile float sim_climb;

static bool owned;      /* holds the piezo */
static bool driven;     /* drive holds the step for drive_level */
static uint8_t drive_level;
static piezo_level_t drive;
static bool pump_on;    /* charge pump running for drive */
static bool tone_on;
static bool in_period;
static vario_tone_t cur;
static tone_kind_t cur_kind;
static uint32_t period_start;
static uint32_t silent_since;
static float damped;        /* the climb rate the tones follow (integration time) */
static uint32_t damped_tick;
static bool damped_valid;

void vario_audio_set(const vario_tones_t *t, uint8_t lvl)
{
  tones = t;
  level = lvl;
}

void vario_audio_led_enable(bool on)
{
  led_wanted = on;
}

void vario_audio_weak_lift(bool on)
{
  weak_wanted = on;
}

void vario_audio_simulate(bool on, float climb_ms)
{
  sim_climb = climb_ms;
  sim_on = on;
}

/*
 * The charge pump runs only around the beeps: it draws its current also while
 * the piezo is silent, and the pauses are half or more of a vario period.
 */
static void pump_set(bool on)
{
  bool want = on && driven && drive.pump_mode != 0;
  if (want == pump_on) {
    return;
  }
  if (want) {
    charge_pump_start(drive.pump_hz, drive.pump_mode);
  } else {
    charge_pump_stop();
  }
  pump_on = want;
}

/* The piezo goes back to the sound task; the tone pattern itself goes on (LED). */
static void release_piezo(void)
{
  if (tone_on) {
    piezo_stop();
    tone_on = false;
  }
  pump_set(false);
  driven = false;
  if (owned) {
    sound_piezo_give();
    owned = false;
  }
}

void vario_audio_release(void)
{
  release_piezo();
  in_period = false;
  indicator_vario_feed(LED_OFF);
}

static bool prepare(uint8_t lvl)
{
  if (!owned) {
    if (!sound_piezo_try_take()) {
      return false;
    }
    owned = true;
  }
  piezo_level_t want;
  sound_piezo_drive(lvl, &want);
  if (!driven || drive_level != lvl || want.pump_mode != drive.pump_mode || want.pump_hz != drive.pump_hz ||
      want.pulse_ns != drive.pulse_ns) {
    pump_set(false); /* restarted with the new step at the beep */
    drive = want;
    driven = true;
    drive_level = lvl;
  }
  return true;
}

/* LED mode "vario colours": green while a lift beep sounds, red during the sink tone. */
static void led_feed(bool on)
{
  if (led_wanted) {
    indicator_vario_feed(!on || cur_kind == KIND_WEAK ? LED_OFF : cur_kind == KIND_LIFT ? LED_GREEN : LED_RED);
  }
}

/*
 * The tone pattern (period, beep length) runs whenever tones are set and either the
 * piezo may sound (level, piezo free) or the LED follows it; the piezo actions are
 * skipped without sound, so the LED still blinks at the flight level mute.
 */
void vario_audio_tick(float climb_ms, uint32_t now)
{
  const vario_tones_t *t = tones;
  uint8_t lvl = level;
  if (sim_on) {
    climb_ms = sim_climb;
  }
  /* integration time: the tones follow a damped climb rate (vario_integration_tau_s) */
  if (t) {
    float dt = damped_valid ? (float)(now - damped_tick) / 1000.0f : 0.0f;
    damped_tick = now;
    if (!damped_valid || dt > 1.0f) {
      damped = climb_ms; /* a fresh start (sensors restarted, tones set again) */
      damped_valid = true;
    } else {
      float tau = vario_integration_tau_s(t->integration_pct);
      damped += dt / (tau + dt) * (climb_ms - damped);
    }
    climb_ms = damped;
  } else {
    damped_valid = false;
  }
  if (lvl && sound_voice_playing()) {
    lvl /= 2;
  }
  bool phones = t && lvl && sound_headphones(); /* beeps as sine tones on the headphone line */
  bool audio = t && lvl && !phones && !sound_piezo_wanted();
  if (!audio && !phones) {
    release_piezo();
    if (!t || !led_wanted) {
      in_period = false;
      indicator_vario_feed(LED_OFF);
      return;
    }
  }

  /* the running period goes on unless lift should be heard at once (vario_tone_cut_short) */
  if (in_period && now - period_start < cur.period_ms &&
      !vario_tone_cut_short(t, climb_ms, cur_kind == KIND_LIFT, cur.on_ms, now - period_start)) {
    uint32_t t_in = now - period_start;
    bool on = t_in < cur.on_ms;
    if (tone_on && !on) {
      piezo_stop();
      tone_on = false;
    }
    if (tone_on && t->sweep && cur_kind != KIND_WEAK) {
      /* sweep tone: the pitch follows the climb rate while the beep sounds */
      vario_tone_t live = vario_tone(t, climb_ms);
      if (live.sound && live.freq_hz != cur.freq_hz) {
        cur.freq_hz = live.freq_hz;
        piezo_set_frequency(cur.freq_hz, drive.pulse_ns);
      }
    }
    if (audio && !tone_on) {
      uint32_t left = cur.period_ms - t_in; /* until the next beep */
      if (left <= PUMP_LEAD_MS) {
        pump_set(true);
      } else if (left > PUMP_LEAD_MS + PUMP_KEEP_MS) {
        pump_set(false);
      }
    }
    led_feed(on);
    return;
  }

  /* Start of the next period (or silence); hysteresis against on/off flicker at the thresholds. */
  float lift = t->lift_start_cms / 100.0f;
  float sink = t->sink_start_cms / 100.0f;
  float c = climb_ms;
  if (in_period && cur_kind == KIND_LIFT && c < lift && c >= lift - HYSTERESIS_M_S) {
    c = lift;
  } else if (in_period && cur_kind == KIND_SINK && t->sink_alarm && c > sink && c <= sink + HYSTERESIS_M_S) {
    c = sink;
  }
  vario_tone_t next = vario_tone(t, c);
  tone_kind_t kind = c >= lift ? KIND_LIFT : KIND_SINK;
  if (!next.sound && weak_wanted) {
    next = vario_tone_weak(t, c); /* the tick between sink and lift ("zero sink") */
    kind = KIND_WEAK;
  }
  if (!next.sound) {
    if (tone_on) {
      piezo_stop();
      tone_on = false;
    }
    pump_set(false);
    if (in_period) {
      silent_since = now;
    }
    in_period = false;
    if (owned && now - silent_since > IDLE_GIVE_MS) {
      release_piezo();
    }
    led_feed(false);
    return;
  }
  if (audio) {
    if (!prepare(lvl)) {
      led_feed(false);
      return;
    }
    pump_set(true); /* already running unless the pause was long */
  }
  cur = next;
  cur_kind = kind;
  period_start = now;
  in_period = true;
  if (phones) {
    release_piezo();
    sound_headphone_beep(cur.freq_hz, cur.on_ms, lvl);
  }
  if (audio) {
    if (tone_on) {
      piezo_set_frequency(cur.freq_hz, drive.pulse_ns); /* continuous tone: no gap */
    } else {
      tone_on = piezo_start(cur.freq_hz, drive.pulse_ns, PIEZO_OUT_BRIDGE);
    }
  }
  led_feed(true);
}
