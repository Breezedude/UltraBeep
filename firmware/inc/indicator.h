#ifndef INDICATOR_H
#define INDICATOR_H

#include <stdint.h>

#include "board.h"

/* LED patterns, run by a 25 ms software timer. A call with the running pattern keeps its phase. */
void indicator_init(void);
void indicator_off(void);
void indicator_solid(led_color_t color);
/* n short blinks, then 1 s pause; n = 0: one very short blink every 2 s. */
void indicator_count(led_color_t color, uint8_t n);
/* on_ms lit every period_ms. */
void indicator_pulse(led_color_t color, uint16_t on_ms, uint16_t period_ms);
/* Nothing else to show, but the device is on: a very short flash every 4 s (green while switched on,
 * blue in the Bluetooth window after switching off). */
void indicator_heartbeat(led_color_t color);
/* n quick blinks every period_ms (the power indicator on battery). */
void indicator_blinks(led_color_t color, uint8_t n, uint16_t period_ms);
/* Brightness rising and falling over period_ms (charging). */
void indicator_breathe(led_color_t color, uint16_t period_ms);
/* Lit at a reduced brightness, 1 .. LED_BRIGHTNESS_DEFAULT (charged). */
void indicator_dim(led_color_t color, uint8_t duty_of_32);
/* Follows the vario tones: the colour fed by indicator_vario_feed() (vario_audio.c, every 10 ms). */
void indicator_vario(void);
void indicator_vario_feed(led_color_t color);
/* Red, green, blue in quick succession, then off. */
void indicator_power_on(void);

#endif
