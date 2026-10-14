#include "sound.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include "audio.h"
#include "board.h"
#include "health.h"
#include "log.h"
#include "rawlog.h"
#include "storage.h"

typedef struct {
  uint16_t freq; /* 0 = pause */
  uint16_t ms;
  uint8_t vol;   /* percent of the level's drive pulse, 0 = full (decrescendo) */
} note_t;

static const note_t mel_ack[] = {{2600, 40, 0}, {0, 0, 0}};
static const note_t mel_next[] = {{2200, 30, 0}, {0, 0, 0}};
static const note_t mel_power_on[] = {{1320, 90, 0}, {0, 30, 0}, {1760, 90, 0}, {0, 30, 0}, {2640, 180, 0}, {0, 0, 0}};
/*
 * Power off (about 1 s, the held button switches off at its end): a falling sequence in
 * decrescendo, chosen with system_poweroffmelody_string (sound_power_off_names).
 */
static const note_t mel_off_scale[] = { /* five steps down the scale E D C B A */
  {1319, 130, 100}, {0, 30, 0}, {1175, 130, 82}, {0, 30, 0}, {1047, 130, 65}, {0, 30, 0}, {988, 130, 48}, {0, 30, 0},
  {880, 380, 30},   {0, 0, 0}};
static const note_t mel_off_chromatic[] = { /* chromatic steps E D# D C# C, quick */
  {1319, 80, 100}, {0, 15, 0}, {1245, 80, 82}, {0, 15, 0}, {1175, 80, 65}, {0, 15, 0}, {1109, 80, 48}, {0, 15, 0},
  {1047, 380, 30}, {0, 0, 0}};
static const note_t mel_off_sighs[] = { /* three sighs, each lower: E-C D-B C-A */
  {1319, 110, 100}, {0, 30, 0}, {1047, 170, 86}, {0, 30, 0}, {1175, 110, 72}, {0, 30, 0}, {988, 170, 58}, {0, 30, 0},
  {1047, 110, 44},  {0, 30, 0}, {880, 300, 30},  {0, 0, 0}};
static const note_t mel_off_triplet[] = { /* G E C, then a low soft G */
  {1568, 110, 100}, {0, 30, 0}, {1319, 110, 75}, {0, 30, 0}, {1047, 110, 50}, {0, 30, 0}, {784, 460, 25}, {0, 0, 0}};

const char *const sound_power_off_names[SOUND_POWER_OFF_COUNT] = {"scale", "chromatic", "sighs", "triplet"};
static const note_t *const power_off_melodies[SOUND_POWER_OFF_COUNT] = {mel_off_scale, mel_off_chromatic,
                                                                         mel_off_sighs, mel_off_triplet};
static volatile uint8_t power_off_choice;
static const note_t mel_confirm[] = {{1760, 80, 0}, {0, 40, 0}, {2640, 140, 0}, {0, 0, 0}};
static const note_t mel_error[] = {{440, 300, 0}, {0, 100, 0}, {330, 500, 0}, {0, 0, 0}};
static const note_t mel_thermal_exit[] = {{1200, 120, 0}, {0, 40, 0}, {900, 120, 0}, {0, 40, 0}, {600, 280, 0}, {0, 0, 0}};
static const note_t mel_prompt[] = {{1760, 90, 0}, {0, 70, 0}, {2349, 170, 0}, {0, 0, 0}}; /* A6 up to D7, a question */

static const note_t *const melodies[MELODY_COUNT] = {
  [MELODY_ACK] = mel_ack,           [MELODY_NEXT] = mel_next,       [MELODY_POWER_ON] = mel_power_on,
  [MELODY_POWER_OFF] = mel_off_scale, [MELODY_CONFIRM] = mel_confirm, [MELODY_ERROR] = mel_error,
  [MELODY_THERMAL_EXIT] = mel_thermal_exit, [MELODY_PROMPT] = mel_prompt,
};

typedef struct {
  uint8_t kind; /* 0 melody, 1 voice, 2 piezo tone, 3 speaker tone, 4 power-off melody, 5 headphone beep */
  uint8_t id;
  uint8_t level;
  uint8_t count;
  uint16_t freq; /* tones */
  uint16_t ms;
  char clips[SOUND_MAX_CLIPS][24];
} request_t;

static QueueHandle_t queue;
static volatile bool abort_request;
static volatile bool playing;
static volatile bool voice_playing;
static volatile bool headphones; /* USB-C audio adapter: everything on the headphone line */
/* Headphones are driven straight from the PWM filter and sit at the ears: far less gain (by ear). */
#define HEADPHONE_MAX_GAIN 0.5f
#define HEADPHONE_TONE_AMPLITUDE 0.5f /* melodies and vario beeps as sine tones */

/* The piezo is shared with the vario tones (vario_audio.c); melodies have priority. */
static SemaphoreHandle_t piezo_mutex;
static volatile bool melody_wants_piezo;

/* ---- piezo ---- */

/*
 * Speaker volume settings are perceived loudness: +10 dB sounds twice as loud,
 * so level 100 % is 10 dB above 50 %. Relative sound pressure p = (level/100)^1.66.
 */
static float loudness_to_pressure(uint8_t level)
{
  return powf((float)level / 100.0f, 1.661f);
}

/*
 * Piezo steps. Far below its resonance the piezo sounds mainly from the
 * switching edges, so only the voltage changes its loudness (tests with 0.10
 * and 0.11): the battery, one or both charge pump legs (the stock firmware
 * also runs both legs at 7 kHz, presumably for a lower voltage), and below that
 * a short pulse that charges the piezo only partly (heard in the 0.10 test
 * below pulses of about 16 us). Order by ear; levels not measured yet.
 */
#define QUIET_PULSE_NS 2500U /* first guess; tuned with the configurator (PIEZO pulse) */
/* Both legs at 7 kHz (stock table) were no louder than one leg at 500 kHz (test 0.13):
 * the frequency is tuned with the configurator (PIEZO pump). */
#define LOUD_PUMP_HZ   7000U

static volatile uint32_t quiet_pulse_ns = QUIET_PULSE_NS;
static volatile uint32_t loud_pump_hz = LOUD_PUMP_HZ;

unsigned sound_piezo_step(uint8_t level)
{
  unsigned s = (level + 10U) / 20U;
  return s < SOUND_PIEZO_STEPS ? s : SOUND_PIEZO_STEPS - 1U;
}

bool sound_piezo_drive(uint8_t level, piezo_level_t *out)
{
  static const piezo_level_t steps[SOUND_PIEZO_STEPS] = {
    {0, 0, PIEZO_PULSE_FULL},      /* off */
    {0, 0, PIEZO_PULSE_FULL},      /* very quiet: pulse set below */
    {0, 0, PIEZO_PULSE_FULL},      /* quiet: battery voltage */
    {2, 500000, PIEZO_PULSE_FULL}, /* medium: one charge pump leg */
    {3, LOUD_PUMP_HZ, PIEZO_PULSE_FULL}, /* loud: both legs, frequency set below */
    {3, 500000, PIEZO_PULSE_FULL}, /* very loud: both legs at 500 kHz */
  };
  unsigned s = sound_piezo_step(level);
  *out = steps[s];
  if (s == 1U) {
    out->pulse_ns = quiet_pulse_ns;
  } else if (s == 4U) {
    out->pump_hz = loud_pump_hz;
  }
  return s > 0U;
}

void sound_set_loud_pump(uint32_t hz)
{
  loud_pump_hz = hz < 5000U ? 5000U : hz > 500000U ? 500000U : hz;
}

uint32_t sound_loud_pump(void)
{
  return loud_pump_hz;
}

void sound_set_quiet_pulse(uint32_t ns)
{
  quiet_pulse_ns = ns < 200U ? 200U : ns > 16000U ? 16000U : ns;
}

uint32_t sound_quiet_pulse(void)
{
  return quiet_pulse_ns;
}

/* The drive pulse of a note: shorter for a quieter one (vol percent, 0 = the level's full pulse). */
static uint32_t note_pulse_ns(const piezo_level_t *d, const note_t *n)
{
  if (n->vol == 0U || n->vol >= 100U) {
    return d->pulse_ns;
  }
  uint32_t full = d->pulse_ns == PIEZO_PULSE_FULL ? 450000000U / n->freq : d->pulse_ns; /* 45 % of the period */
  uint32_t p = full * n->vol / 100U;
  return p < 200U ? 200U : p;
}

/* A melody as sine tones on the headphone line; a note's vol scales its amplitude. */
static bool speaker_on(uint8_t level);
static void speaker_off(void);
static void play_melody_headphones(const note_t *notes, uint8_t level)
{
  if (level == 0) {
    return;
  }
  if (speaker_on(level)) {
    for (const note_t *n = notes; n->ms && !abort_request; n++) {
      health_alive(HEALTH_SOUND);
      float amp = n->freq ? HEADPHONE_TONE_AMPLITUDE * (n->vol && n->vol < 100U ? (float)n->vol / 100.0f : 1.0f) : 0.0f;
      voice_play_tone(n->freq, n->ms, amp);
    }
  }
  speaker_off();
}

static void play_melody(const note_t *notes, uint8_t level)
{
  if (headphones) {
    play_melody_headphones(notes, level);
    return;
  }
  piezo_level_t d;
  if (!sound_piezo_drive(level, &d)) {
    return;
  }
  melody_wants_piezo = true;
  bool owned = xSemaphoreTake(piezo_mutex, pdMS_TO_TICKS(200)) == pdTRUE; /* the vario lets go within 10 ms */
  melody_wants_piezo = false;
  if (d.pump_mode) {
    charge_pump_start(d.pump_hz, d.pump_mode);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  bool running = false;
  for (const note_t *n = notes; n->ms && !abort_request; n++) {
    health_alive(HEALTH_SOUND);
    if (n->freq) {
      uint32_t pulse = note_pulse_ns(&d, n);
      if (!running) {
        running = piezo_start(n->freq, pulse, PIEZO_OUT_BRIDGE);
      } else {
        piezo_set_frequency(n->freq, pulse);
      }
    } else if (running) {
      piezo_stop();
      running = false;
    }
    vTaskDelay(pdMS_TO_TICKS(n->ms));
  }
  piezo_stop();
  charge_pump_stop();
  if (owned) {
    xSemaphoreGive(piezo_mutex);
  }
}

void sound_beep_blocking(uint32_t freq_hz, uint32_t ms, uint8_t level)
{
  piezo_level_t d;
  if (!sound_piezo_drive(level, &d)) {
    return;
  }
  if (d.pump_mode) {
    charge_pump_start(d.pump_hz, d.pump_mode);
    board_delay_ms(20);
  }
  piezo_start(freq_hz, d.pulse_ns, PIEZO_OUT_BRIDGE);
  board_delay_ms(ms);
  piezo_stop();
  charge_pump_stop();
}

/* ---- voice ---- */

/*
 * Digital gain of the speaker at 100 %: the loudest level before the stock voice
 * clips start to distort. In the listening test with 6.4 at 100 % that began at
 * 70 %, i.e. 6.4 * 0.7^1.661 = 3.5 (4.4 times the former 0.8, +13 dB).
 * Samples beyond full scale are clipped.
 */
#define SPEAKER_MAX_GAIN 3.5f
#define AMP_SETTLE_MS    5U /* PWM at mid level before the amplifier comes on, and the amplifier's start */

/*
 * Speaker path on and off without a click: the amplifier is enabled only while
 * the PWM already sits at mid level (its input filter has settled), and it is
 * disabled before the PWM pins are released.
 */
static bool speaker_on(uint8_t level)
{
  voice_playing = true;
  if (headphones) {
    return voice_start(VOICE_OUT_ALTERNATE, HEADPHONE_MAX_GAIN * loudness_to_pressure(level));
  }
  if (!voice_start(VOICE_OUT_SPEAKER, SPEAKER_MAX_GAIN * loudness_to_pressure(level))) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(AMP_SETTLE_MS));
  board_amp_enable(true);
  vTaskDelay(pdMS_TO_TICKS(AMP_SETTLE_MS));
  return true;
}

static void speaker_off(void)
{
  board_amp_enable(false);
  voice_stop();
  voice_playing = false;
}

static void play_voice(const request_t *r)
{
  char path[64];
  if (!storage_voice_pack_ready() || r->level == 0) {
    return;
  }
  if (speaker_on(r->level)) {
    for (unsigned i = 0; i < r->count && !abort_request; i++) {
      health_alive(HEALTH_SOUND);
      if (strcmp(r->clips[i], SOUND_PAUSE_CLIP) == 0) {
        voice_play_tone(0, SOUND_PAUSE_MS, 0.0f); /* silence */
        continue;
      }
      if (!storage_voice_locate(r->clips[i], path, sizeof(path)) || voice_play_qoa(path) == 0) {
        log_printf("Voice clip %s missing", r->clips[i]);
      }
    }
    unsigned late, starved;
    voice_stats(&late, &starved);
    if (late || starved) {
      log_printf("Voice %s: %u blocks late, %u starved", r->clips[0], late, starved);
      rawlog_event("voice underrun %s late %u starved %u", r->clips[0], late, starved);
    }
  }
  speaker_off();
}

/* Test tone on the speaker (configurator: volumes and frequencies). */
static void play_speaker_tone(const request_t *r)
{
  if (r->level == 0) {
    return;
  }
  if (speaker_on(r->level)) {
    voice_play_tone(r->freq, r->ms, 0.9f);
  }
  speaker_off();
}

/* ---- task ---- */

static void sound_task(void *arg)
{
  (void)arg;
  request_t r;
  for (;;) {
    health_alive(HEALTH_SOUND);
    if (xQueueReceive(queue, &r, pdMS_TO_TICKS(500)) != pdTRUE) {
      continue;
    }
    playing = true;
    abort_request = false;
    if (r.kind == 0 && r.id < MELODY_COUNT) {
      play_melody(r.id == MELODY_POWER_OFF ? power_off_melodies[power_off_choice] : melodies[r.id], r.level);
    } else if (r.kind == 4 && r.id < SOUND_POWER_OFF_COUNT) {
      play_melody(power_off_melodies[r.id], r.level);
    } else if (r.kind == 1) {
      play_voice(&r);
    } else if (r.kind == 2) {
      const note_t tone[] = {{r.freq, r.ms, 0}, {0, 0, 0}};
      play_melody(tone, r.level);
    } else if (r.kind == 3) {
      play_speaker_tone(&r);
    } else if (r.kind == 5 && headphones && r.level) {
      if (speaker_on(r.level)) {
        voice_play_tone(r.freq, r.ms, HEADPHONE_TONE_AMPLITUDE);
      }
      speaker_off();
    }
    playing = false;
  }
}

void sound_init(void)
{
  audio_init();
  queue = xQueueCreate(6, sizeof(request_t));
  piezo_mutex = xSemaphoreCreateMutex();
  xTaskCreate(sound_task, "sound", 512, NULL, 4, NULL);
}

void sound_melody(melody_t melody, uint8_t level)
{
  request_t r = {0};
  r.kind = 0;
  r.id = (uint8_t)melody;
  r.level = level;
  xQueueSend(queue, &r, 0);
}

void sound_set_power_off_melody(unsigned index)
{
  power_off_choice = (uint8_t)(index < SOUND_POWER_OFF_COUNT ? index : 0U);
}

uint32_t sound_power_off_ms(void)
{
  uint32_t ms = 0;
  for (const note_t *n = power_off_melodies[power_off_choice]; n->ms; n++) {
    ms += n->ms;
  }
  return ms;
}

void sound_play_power_off(unsigned index, uint8_t level)
{
  request_t r = {0};
  r.kind = 4;
  r.id = (uint8_t)(index < SOUND_POWER_OFF_COUNT ? index : 0U);
  r.level = level;
  xQueueSend(queue, &r, 0);
}

void sound_say(const char *const *clips, unsigned count, uint8_t level)
{
  request_t r = {0};
  r.kind = 1;
  r.level = level;
  for (unsigned i = 0; i < count && i < SOUND_MAX_CLIPS; i++) {
    snprintf(r.clips[i], sizeof(r.clips[i]), "%s", clips[i]);
    r.count++;
  }
  xQueueSend(queue, &r, 0);
}

void sound_say1(const char *clip, uint8_t level)
{
  sound_say(&clip, 1, level);
}

void sound_stop(void)
{
  xQueueReset(queue);
  abort_request = true;
  voice_abort();
}

bool sound_busy(void)
{
  return playing || uxQueueMessagesWaiting(queue) > 0;
}

bool sound_voice_playing(void)
{
  return voice_playing;
}

bool sound_piezo_try_take(void)
{
  return !melody_wants_piezo && xSemaphoreTake(piezo_mutex, 0) == pdTRUE;
}

bool sound_piezo_wanted(void)
{
  return melody_wants_piezo;
}

void sound_piezo_give(void)
{
  xSemaphoreGive(piezo_mutex);
}

void sound_set_headphones(bool on)
{
  headphones = on;
}

bool sound_headphones(void)
{
  return headphones;
}

void sound_headphone_beep(uint16_t freq_hz, uint16_t ms, uint8_t level)
{
  if (playing || uxQueueMessagesWaiting(queue) != 0U) {
    return; /* a clip or a melody plays: the beep is left out */
  }
  request_t r = {0};
  r.kind = 5;
  r.freq = freq_hz;
  r.ms = ms;
  r.level = level;
  xQueueSend(queue, &r, 0);
}

void sound_tone(uint16_t freq_hz, uint16_t ms, uint8_t level, bool speaker)
{
  request_t r = {0};
  r.kind = speaker ? 3 : 2;
  r.level = level;
  r.freq = freq_hz;
  r.ms = ms;
  xQueueSend(queue, &r, 0);
}
