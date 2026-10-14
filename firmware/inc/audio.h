#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Audio hardware (piezo H-bridge with charge pump, speaker PWM + DMA).
 * Not thread safe: only the sound task (sound.c) uses it after start-up.
 */

typedef enum {
  PIEZO_OUT_BRIDGE, /* PA0/PA1/PB10/PB11 H-bridge (internal piezo) */
  PIEZO_OUT_PA15,   /* single-ended TIM2_CH1 on PA15 (stock "output 1") */
} piezo_output_t;

typedef enum {
  PIEZO_DRIVE_CLASSIC, /* stock: bridge off between the pulses; loudness only from the charge pump */
  PIEZO_DRIVE_PWM,     /* experimental: piezo shorted between the pulses; the pulse width sets the loudness */
} piezo_drive_t;

typedef enum {
  VOICE_OUT_SPEAKER,   /* PB8 + PB6 complementary */
  VOICE_OUT_ALTERNATE, /* PA6 */
} voice_output_t;

void audio_init(void);

bool charge_pump_start(uint32_t freq_hz, int mode);
void charge_pump_stop(void);

/* pulse_ns: on-time of a high side per half period, PIEZO_PULSE_FULL = 90 % of it.
 * Shorter than a few us, the pulse charges the piezo only partly (quieter). */
#define PIEZO_PULSE_FULL 0U
bool piezo_start(uint32_t freq_hz, uint32_t pulse_ns, piezo_output_t output);
void piezo_set_frequency(uint32_t freq_hz, uint32_t pulse_ns);
void piezo_stop(void);
/* Applies from the next tone or frequency change; any task. Classic after every start. */
void piezo_set_drive(piezo_drive_t d);
piezo_drive_t piezo_get_drive(void);

bool voice_start(voice_output_t output, float gain);
bool voice_play_tone(uint32_t freq_hz, uint32_t duration_ms, float amplitude);
/* Plays a QOA file; returns the number of samples played (0 = error). */
uint32_t voice_play_qoa(const char *path);
/* Underruns since voice_start: blocks filled too late (stale audio repeated) and silence for missing data. */
void voice_stats(unsigned *late, unsigned *starved);
/* Ends a running voice_play_qoa() early (any task). */
void voice_abort(void);
void voice_stop(void);

#endif
