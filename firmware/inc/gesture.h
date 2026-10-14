#ifndef GESTURE_H
#define GESTURE_H

#include <stdbool.h>
#include <stdint.h>

#include "button.h"

/*
 * Power-on gesture: a short press (wake-up), then a long press. Every step
 * has a limit of GESTURE_STEP_MS:
 *   wake-up press held longer            -> abort
 *   no second press within the limit     -> abort
 *   second press released before HOLD_MS -> abort
 *   second press held for HOLD_MS        -> power on
 * No hardware access: system.c feeds button events and its tick.
 */
#define GESTURE_STEP_MS 1000U
#define GESTURE_HOLD_MS 500U

typedef enum { GESTURE_WAIT_RELEASE, GESTURE_WAIT_PRESS, GESTURE_WAIT_HOLD } gesture_phase_t;
typedef enum { GESTURE_CONTINUE, GESTURE_POWER_ON, GESTURE_ABORT } gesture_result_t;

typedef struct {
  gesture_phase_t phase;
  uint32_t since;    /* tick at the start of the phase */
  const char *why;   /* reason of the last GESTURE_ABORT */
} gesture_t;

/* The wake-up press is still held (started at `now`). */
void gesture_start_pressed(gesture_t *g, uint32_t now);
/* The wake-up press was released at `release_tick`. */
void gesture_start_released(gesture_t *g, uint32_t release_tick);

/*
 * One step: `event` is the button event or NULL for a tick, `pressed` the
 * current button state. Returns GESTURE_CONTINUE while the gesture is open.
 */
gesture_result_t gesture_step(gesture_t *g, uint32_t now, const button_event_t *event, bool pressed);

#endif
