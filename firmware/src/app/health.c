#include "health.h"

#include "FreeRTOS.h"
#include "timers.h"

#include "board.h"
#include "log.h"

#define CHECK_PERIOD_MS 1000U
#define ALLOWED_SILENCE_MS 4000U

static uint32_t expected;
static volatile uint32_t last_seen[8];

void health_alive(health_task_t task)
{
  for (unsigned i = 0; i < 8; i++) {
    if ((uint32_t)task == (1U << i)) {
      last_seen[i] = HAL_GetTick();
    }
  }
}

static void check(TimerHandle_t t)
{
  (void)t;
  uint32_t now = HAL_GetTick();
  for (unsigned i = 0; i < 8; i++) {
    if ((expected & (1U << i)) && now - last_seen[i] > ALLOWED_SILENCE_MS) {
      return; /* a task hangs: let the watchdog reset the device */
    }
  }
  watchdog_feed();
}

void health_start(uint32_t expected_tasks)
{
  expected = expected_tasks;
  uint32_t now = HAL_GetTick();
  for (unsigned i = 0; i < 8; i++) {
    last_seen[i] = now;
  }
  TimerHandle_t t = xTimerCreate("health", pdMS_TO_TICKS(CHECK_PERIOD_MS), pdTRUE, NULL, check);
  xTimerStart(t, 0);
}
