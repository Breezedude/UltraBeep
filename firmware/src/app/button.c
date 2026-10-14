#include "button.h"

#include "FreeRTOS.h"
#include "timers.h"

#include "backup.h"
#include "board.h"
#include "rtc_time.h"

#define SAMPLE_MS   10U
#define STABLE_N    3U
#define HOLD_REPORT 100U

static button_handler_t handler;
static bool state;
static uint8_t stable;
static uint32_t down_tick;
static uint32_t last_hold_report;

static void sample(TimerHandle_t t)
{
  (void)t;
  bool now = button_pressed();
  if (now == state) {
    stable = 0;
    if (state && HAL_GetTick() - last_hold_report >= HOLD_REPORT) {
      last_hold_report = HAL_GetTick();
      button_event_t e = {BUTTON_HOLD, HAL_GetTick() - down_tick};
      handler(&e);
    }
    return;
  }
  if (++stable < STABLE_N) {
    return;
  }
  stable = 0;
  state = now;
  /* Kept across a reset: shows how long the button was held before the board's long-press reset. */
  backup_write(BACKUP_PRESS_MS, state ? (rtc_time_ms32() | 1U) : 0U);
  if (state) {
    down_tick = HAL_GetTick() - SAMPLE_MS * STABLE_N;
    last_hold_report = HAL_GetTick();
    button_event_t e = {BUTTON_DOWN, 0};
    handler(&e);
  } else {
    button_event_t e = {BUTTON_UP, HAL_GetTick() - down_tick};
    handler(&e);
  }
}

void button_init(button_handler_t h, uint32_t pressed_since_tick)
{
  handler = h;
  if (pressed_since_tick && button_pressed()) {
    state = true;
    down_tick = pressed_since_tick;
    last_hold_report = HAL_GetTick();
  }
  TimerHandle_t t = xTimerCreate("button", pdMS_TO_TICKS(SAMPLE_MS), pdTRUE, NULL, sample);
  xTimerStart(t, 0);
}
