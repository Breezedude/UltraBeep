#include "gesture.h"

#include <stddef.h>

static void phase(gesture_t *g, gesture_phase_t p, uint32_t since)
{
  g->phase = p;
  g->since = since;
}

static gesture_result_t abort_with(gesture_t *g, const char *why)
{
  g->why = why;
  return GESTURE_ABORT;
}

void gesture_start_pressed(gesture_t *g, uint32_t now)
{
  phase(g, GESTURE_WAIT_RELEASE, now);
  g->why = NULL;
}

void gesture_start_released(gesture_t *g, uint32_t release_tick)
{
  phase(g, GESTURE_WAIT_PRESS, release_tick);
  g->why = NULL;
}

gesture_result_t gesture_step(gesture_t *g, uint32_t now, const button_event_t *event, bool pressed)
{
  uint32_t elapsed = now - g->since;
  switch (g->phase) {
  case GESTURE_WAIT_RELEASE:
    if (event && event->type == BUTTON_UP) {
      phase(g, GESTURE_WAIT_PRESS, now);
    } else if (elapsed > GESTURE_STEP_MS) {
      return abort_with(g, "first press too long");
    }
    break;
  case GESTURE_WAIT_PRESS:
    if (event && event->type == BUTTON_DOWN) {
      phase(g, GESTURE_WAIT_HOLD, now);
    } else if (elapsed > GESTURE_STEP_MS) {
      return abort_with(g, "no second press");
    }
    break;
  case GESTURE_WAIT_HOLD:
    if (event && event->type == BUTTON_UP) {
      return abort_with(g, "second press too short");
    }
    if (elapsed >= GESTURE_HOLD_MS && pressed) {
      return GESTURE_POWER_ON;
    }
    if (elapsed > GESTURE_STEP_MS) {
      return abort_with(g, "second press lost"); /* released, but the event never came */
    }
    break;
  }
  return GESTURE_CONTINUE;
}
