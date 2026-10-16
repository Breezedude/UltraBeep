#include "ble_console.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "board.h"
#include "console_pack.h"

#define OUT_SIZE 1024U /* power of two; the console task's answers wait for room, other lines are dropped and counted */
#define IN_SIZE 128U   /* power of two; a console line has at most 95 characters */
#define FLUSH_MS 40U   /* text without a line end goes out when nothing more came for this long */
#define STALL_MS 1000U /* the console task gives up its output when nothing drained for this long */

/* Producers are tasks only; they copy with the scheduler suspended, so neither another producer
 * nor the Bluetooth task (the only consumer) sees a half-written piece. Interrupts keep running. */
static char out[OUT_SIZE];
static volatile uint32_t out_head; /* free running */
static volatile uint32_t out_tail; /* advanced by the Bluetooth task */
static char in[IN_SIZE];
static volatile uint32_t in_head; /* advanced by the Bluetooth task */
static volatile uint32_t in_tail; /* advanced by the console task */
static volatile bool active;
static volatile bool greet;
static volatile uint32_t last_write;
static volatile uint32_t dropped; /* lines dropped since the last report */
static TaskHandle_t patient;
static health_task_t patient_alive;
static uint32_t pending_used, pending_dropped; /* the packet of ble_console_take */

static bool in_isr(void)
{
  return __get_IPSR() != 0U;
}

void ble_console_patient_task(health_task_t alive)
{
  patient_alive = alive;
  patient = xTaskGetCurrentTaskHandle();
}

/* Copies all of data when it fits. Otherwise: whole = nothing (a dropped line), else the longest
 * part that fits and ends at a line end. Returns the bytes copied. */
static size_t put_out(const char *data, size_t len, bool whole)
{
  vTaskSuspendAll();
  uint32_t h = out_head;
  uint32_t room = OUT_SIZE - (h - out_tail);
  size_t n = len;
  if (n > room) {
    n = 0;
    if (whole) {
      dropped++;
    } else {
      n = room;
      while (n && data[n - 1U] != '\n') {
        n--;
      }
    }
  }
  for (size_t i = 0; i < n; i++) {
    out[(h + i) & (OUT_SIZE - 1U)] = data[i];
  }
  if (n) {
    out_head = h + n;
    last_write = HAL_GetTick();
  }
  (void)xTaskResumeAll();
  return n;
}

void ble_console_write(const char *data, size_t len)
{
  if (!active || len == 0 || in_isr() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
    return;
  }
  if (!patient || xTaskGetCurrentTaskHandle() != patient) {
    (void)put_out(data, len, true);
    return;
  }
  uint32_t t0 = HAL_GetTick();
  while (active) {
    size_t n = put_out(data, len, false);
    data += n;
    len -= n;
    if (len == 0) {
      return;
    }
    if (n) {
      t0 = HAL_GetTick();
    } else if (HAL_GetTick() - t0 > STALL_MS) {
      vTaskSuspendAll();
      dropped++;
      (void)xTaskResumeAll();
      return;
    }
    health_alive(patient_alive);
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

bool ble_console_active(void)
{
  return active;
}

int ble_console_getc(void)
{
  uint32_t t = in_tail;
  if (t == in_head) {
    return -1;
  }
  char c = in[t & (IN_SIZE - 1U)];
  in_tail = t + 1U;
  return (unsigned char)c;
}

bool ble_console_greeting(void)
{
  if (!greet) {
    return false;
  }
  greet = false;
  return true;
}

void ble_console_enable(bool on)
{
  vTaskSuspendAll();
  out_tail = out_head; /* nothing left over from an earlier session */
  dropped = 0;
  pending_used = 0;
  pending_dropped = 0;
  active = on;
  greet = on;
  (void)xTaskResumeAll();
}

bool ble_console_input(const char *line)
{
  size_t len = strlen(line);
  if (!active || len + 1U > IN_SIZE - (in_head - in_tail)) {
    return false;
  }
  vTaskSuspendAll();
  uint32_t h = in_head;
  for (size_t i = 0; i < len; i++) {
    in[(h + i) & (IN_SIZE - 1U)] = line[i];
  }
  in[(h + len) & (IN_SIZE - 1U)] = '\r';
  in_head = h + len + 1U;
  (void)xTaskResumeAll();
  return true;
}

size_t ble_console_take(char *dst, size_t max)
{
  pending_used = 0;
  pending_dropped = 0;
  if (!active) {
    return 0;
  }
  uint32_t from = out_tail;
  uint32_t to = out_head;
  if (from == to) {
    uint32_t d = dropped;
    if (!d) {
      return 0;
    }
    int n = snprintf(dst, max, "CON (%lu console lines dropped: Bluetooth too slow)\n", (unsigned long)d);
    if (n <= 0 || (size_t)n >= max) {
      return 0;
    }
    pending_dropped = d;
    return (size_t)n;
  }
  uint32_t used;
  size_t n = console_pack(out, OUT_SIZE - 1U, from, to, HAL_GetTick() - last_write >= FLUSH_MS, dst, max, &used);
  if (n == 0) {
    out_tail = from + used; /* only a '\r' */
    return 0;
  }
  pending_used = used;
  return n;
}

void ble_console_sent(void)
{
  vTaskSuspendAll();
  out_tail += pending_used;
  dropped -= pending_dropped;
  (void)xTaskResumeAll();
  pending_used = 0;
  pending_dropped = 0;
}

bool ble_console_busy(void)
{
  return active && (out_head != out_tail || dropped);
}
