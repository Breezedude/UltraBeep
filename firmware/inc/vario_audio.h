#ifndef VARIO_AUDIO_H
#define VARIO_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#include "vario.h"

/*
 * Vario tones on the piezo. No own task: the sensor task calls
 * vario_audio_tick() every 10 ms with the filtered climb rate. The piezo is
 * borrowed from the sound task; a waiting melody gets it within one tick.
 * While a voice clip plays, the vario is muffled (half the loudness, as in
 * the stock firmware).
 */

/* Tone map and perceived loudness in percent (0 = silent, e.g. on the ground or muted). */
void vario_audio_set(const vario_tones_t *tones, uint8_t level);
/* LED mode "vario colours": the LED follows the tone pattern, also when the level is 0 (muted). */
void vario_audio_led_enable(bool on);
/* The weak-lift tone (vario_tone_weak) may sound: in flight or during the configurator's test. */
void vario_audio_weak_lift(bool on);
void vario_audio_tick(float climb_ms, uint32_t now_ms);
/* Stops a tone at once and gives the piezo back. */
void vario_audio_release(void);
/* Test from the configurator: tones for this climb rate instead of the measured one. */
void vario_audio_simulate(bool on, float climb_ms);

#endif
