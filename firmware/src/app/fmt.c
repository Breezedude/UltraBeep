#include "fmt.h"

#include <math.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "board.h"

static SemaphoreHandle_t lock;

void fmt_init(void)
{
  lock = xSemaphoreCreateMutex();
}

static bool take(void)
{
  if (!lock || __get_IPSR() != 0U || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
    return false;
  }
  return xSemaphoreTake(lock, portMAX_DELAY) == pdTRUE;
}

int fmt_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
  bool held = take();
  int n = vsnprintf(buf, size, fmt, ap);
  if (held) {
    xSemaphoreGive(lock);
  }
  return n;
}

int fmt_snprintf(char *buf, size_t size, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int n = fmt_vsnprintf(buf, size, fmt, ap);
  va_end(ap);
  return n;
}

int fmt_fixed(char *out, size_t size, float value, unsigned decimals)
{
  unsigned d = decimals < 6U ? decimals : 6U;
  uint32_t scale = 1;
  for (unsigned i = 0; i < d; i++) {
    scale *= 10U;
  }
  float v = fabsf(value) * (float)scale + 0.5f;
  uint32_t u = v < 4.0e9f ? (uint32_t)v : (v > 0.0f ? 4000000000U : 0U); /* NaN: 0 */
  const char *sign = value < 0.0f && u != 0U ? "-" : "";
  if (d == 0) {
    return snprintf(out, size, "%s%lu", sign, (unsigned long)u);
  }
  char frac[8];
  uint32_t rest = u % scale;
  for (unsigned i = d; i > 0; i--) {
    frac[i - 1] = (char)('0' + rest % 10U);
    rest /= 10U;
  }
  frac[d] = 0;
  return snprintf(out, size, "%s%lu.%s", sign, (unsigned long)(u / scale), frac);
}
