#include "battery.h"

#include <string.h>

void battery_watch_init(battery_watch_t *w)
{
  memset(w, 0, sizeof(*w));
}

battery_event_t battery_watch_update(battery_watch_t *w, uint32_t vbat_mv)
{
  if (vbat_mv == 0U) {
    return BATTERY_EVENT_NONE; /* no reading */
  }
  w->critical_count = vbat_mv < BATTERY_CRITICAL_MV ? (uint8_t)(w->critical_count + 1U) : 0U;
  w->low_count = vbat_mv < BATTERY_LOW_MV ? (uint8_t)(w->low_count + 1U) : 0U;
  if (vbat_mv >= BATTERY_RECOVER_MV) {
    w->warned = false;
  }
  if (w->critical_count >= BATTERY_CONFIRM) {
    w->critical_count = BATTERY_CONFIRM; /* stays critical */
    return BATTERY_EVENT_CRITICAL;
  }
  if (w->low_count >= BATTERY_CONFIRM && !w->warned) {
    w->warned = true;
    return BATTERY_EVENT_LOW;
  }
  if (w->low_count > BATTERY_CONFIRM) {
    w->low_count = BATTERY_CONFIRM;
  }
  return BATTERY_EVENT_NONE;
}

bool battery_can_start(uint32_t vbat_mv, bool usb_power)
{
  return usb_power || vbat_mv < BATTERY_PLAUSIBLE_MV || vbat_mv >= BATTERY_START_MV;
}
