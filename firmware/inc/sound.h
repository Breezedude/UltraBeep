#ifndef SOUND_H
#define SOUND_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Sound task: melodies on the piezo and voice clips (QOA files of the voice
 * pack, 1:/voices/<clip>.qoa) on the speaker. Requests are queued and played
 * one after the other; sound_stop() cancels the current one and the queue.
 */

typedef enum {
  MELODY_ACK,       /* short click: a button press was registered */
  MELODY_NEXT,      /* profile selection: next */
  MELODY_POWER_ON,
  MELODY_POWER_OFF, /* ~2.5 s, held button switches off at its end */
  MELODY_CONFIRM,
  MELODY_ERROR,
  MELODY_THERMAL_EXIT, /* falling three tones: the climb phase has ended */
  MELODY_PROMPT,       /* two rising tones: the device asks for a button press (a new phone or browser) */
  MELODY_COUNT
} melody_t;

#define SOUND_MAX_CLIPS 4
/* A clip name that plays silence instead of a file: a short break inside an announcement. */
#define SOUND_PAUSE_CLIP "pause"
#define SOUND_PAUSE_MS   350U

void sound_init(void);
void sound_melody(melody_t melody, uint8_t level);
/* The power-off melodies (system_poweroffmelody_string): names, the choice, a test of one of them. */
#define SOUND_POWER_OFF_COUNT 4
extern const char *const sound_power_off_names[SOUND_POWER_OFF_COUNT];
void sound_set_power_off_melody(unsigned index);
void sound_play_power_off(unsigned index, uint8_t level);
/* Length of the chosen power-off melody in ms. */
uint32_t sound_power_off_ms(void);
/* Plays up to SOUND_MAX_CLIPS voice clips in a row (names without path/extension). */
void sound_say(const char *const *clips, unsigned count, uint8_t level);
void sound_say1(const char *clip, uint8_t level);
/* Single test tone on the piezo or the speaker (configurator). */
void sound_tone(uint16_t freq_hz, uint16_t ms, uint8_t level, bool speaker);
void sound_stop(void);
/*
 * Headphones on a USB-C audio adapter (system.c detects it): voice clips, melodies and the vario
 * beeps go to the headphone line (PA6) instead of the speaker and the piezo, the amplifier stays
 * off - as in the stock firmware.
 */
void sound_set_headphones(bool on);
bool sound_headphones(void);
/* One vario beep on the headphones; dropped while something else plays. */
void sound_headphone_beep(uint16_t freq_hz, uint16_t ms, uint8_t level);
bool sound_busy(void);

/* Direct piezo beep for the early boot phase (no task, blocking); level in percent. */
void sound_beep_blocking(uint32_t freq_hz, uint32_t ms, uint8_t level);

/*
 * Piezo volume: the hardware has steps, no continuous volume. Settings stay in
 * percent: 0, 20, 40, 60, 80, 100 = off, very quiet, quiet, medium, loud, very
 * loud; other values round to the nearest step.
 */
#define SOUND_PIEZO_STEPS 6
typedef struct {
  int pump_mode;     /* charge pump: 0 off, 2 one leg, 3 both legs */
  uint32_t pump_hz;  /* charge pump switching frequency */
  uint32_t pulse_ns; /* PIEZO_PULSE_FULL, or short: the piezo is charged only partly */
} piezo_level_t;
unsigned sound_piezo_step(uint8_t level);
/* Drive for a setting in percent; false for the step "off". */
bool sound_piezo_drive(uint8_t level, piezo_level_t *out);
/* Pulse of the step "very quiet" (configurator test until power off; 200..16000 ns). */
void sound_set_quiet_pulse(uint32_t ns);
uint32_t sound_quiet_pulse(void);
/* Charge pump frequency of the step "loud", both legs (configurator test until power off; 5..500 kHz). */
void sound_set_loud_pump(uint32_t hz);
uint32_t sound_loud_pump(void);
bool sound_voice_playing(void);

/*
 * Piezo sharing with the vario tones: the vario takes the piezo when it is
 * free and gives it back within one tick once sound_piezo_wanted() (a melody
 * is waiting).
 */
bool sound_piezo_try_take(void);
bool sound_piezo_wanted(void);
void sound_piezo_give(void);

#endif
