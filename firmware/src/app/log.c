#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "ble_console.h"
#include "board.h"
#include "fmt.h"
#include "uart_console.h"
#include "usb_device.h"

#define RING_SIZE (4 * 1024) /* the start-up log is about 3 KB: a terminal connected late sees it */

static SemaphoreHandle_t lock;
static char ring[RING_SIZE];
static size_t ring_head; /* next write position */
static bool ring_wrapped;

static bool in_isr(void)
{
  return __get_IPSR() != 0U;
}

static bool take(void)
{
  if (in_isr()) {
    return false;
  }
  if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
    return true; /* single-threaded start-up */
  }
  return xSemaphoreTake(lock, pdMS_TO_TICKS(200)) == pdTRUE;
}

static void give(void)
{
  if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
    xSemaphoreGive(lock);
  }
}

static void ring_put(const char *s, size_t n)
{
  size_t first = RING_SIZE - ring_head; /* n is at most one line, far below RING_SIZE */
  if (n < first) {
    memcpy(&ring[ring_head], s, n);
    ring_head += n;
    return;
  }
  memcpy(&ring[ring_head], s, first);
  memcpy(ring, s + first, n - first);
  ring_head = n - first;
  ring_wrapped = true;
}

void log_init(void)
{
  lock = xSemaphoreCreateMutex();
}

void log_printf(const char *fmt, ...)
{
  char line[256];
  uint32_t ms = HAL_GetTick();
  int n = snprintf(line, sizeof(line), "[%5lu.%03lu] ", (unsigned long)(ms / 1000U), (unsigned long)(ms % 1000U));
  va_list ap;
  va_start(ap, fmt);
  int m = fmt_vsnprintf(line + n, sizeof(line) - (size_t)n - 2U, fmt, ap);
  va_end(ap);
  if (m < 0) {
    return;
  }
  size_t len = (size_t)n + (size_t)m;
  if (len > sizeof(line) - 3U) {
    len = sizeof(line) - 3U; /* truncated */
  }
  line[len++] = '\r';
  line[len++] = '\n';
  if (!take()) {
    return;
  }
  ring_put(line, len);
  usb_cdc_write(line, len);
  uart_console_write(line, len);
  give();
  ble_console_write(line, len); /* outside the lock: the console task may wait for Bluetooth there */
}

void console_write(const char *data, size_t len)
{
  if (!take()) {
    return;
  }
  usb_cdc_write(data, len);
  uart_console_write(data, len);
  give();
  ble_console_write(data, len);
}

void console_printf(const char *fmt, ...)
{
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  int n = fmt_vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n > 0) {
    console_write(buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1U);
  }
}

void log_replay(void)
{
  if (!take()) {
    return;
  }
  if (ring_wrapped) {
    usb_cdc_write(&ring[ring_head], RING_SIZE - ring_head);
  }
  usb_cdc_write(ring, ring_head);
  give();
}

void log_replay_uart(void)
{
  if (!take()) {
    return;
  }
  if (ring_wrapped) {
    uart_console_write(&ring[ring_head], RING_SIZE - ring_head);
  }
  uart_console_write(ring, ring_head);
  give();
}

void log_replay_ble(void)
{
  static char chunk[256]; /* the console task only */
  if (!take()) {
    return;
  }
  size_t start = ring_wrapped ? ring_head : 0;
  size_t total = ring_wrapped ? RING_SIZE : ring_head;
  give();
  /* in pieces without holding the lock while Bluetooth sends; lines logged meanwhile arrive live */
  for (size_t off = 0; off < total && ble_console_active();) {
    size_t n = total - off < sizeof(chunk) ? total - off : sizeof(chunk);
    if (!take()) {
      return;
    }
    for (size_t i = 0; i < n; i++) {
      chunk[i] = ring[(start + off + i) % RING_SIZE];
    }
    give();
    if (off + n < total) {
      size_t m = n; /* up to a line end: a live line must not land inside a replayed one */
      while (m && chunk[m - 1U] != '\n') {
        m--;
      }
      n = m ? m : n;
    }
    ble_console_write(chunk, n);
    off += n;
  }
}
