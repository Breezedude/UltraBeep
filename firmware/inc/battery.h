#ifndef BATTERY_H
#define BATTERY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Battery watch without hardware access (unit-tested), fed every 10 s with
 * the battery voltage under load, while switched on and in the Bluetooth
 * window after switching off:
 *   below BATTERY_LOW_MV       -> one warning (logged; again only after the
 *                                 battery recovered, e.g. charging)
 *   below BATTERY_CRITICAL_MV  -> power off, so the IGC file is closed and
 *                                 signed long before the voltage collapses
 * Each level needs BATTERY_CONFIRM readings in a row (load peaks). Below
 * BATTERY_START_MV the device does not switch on at all: right after such a
 * power off the voltage recovers a little, and it would only run again until
 * the next reading. The levels are the stock firmware's: 0 % at 3640 mV, its
 * auto power off "from low power" at 3580 mV or less.
 */
#define BATTERY_LOW_MV       3640U
#define BATTERY_CRITICAL_MV  3580U
#define BATTERY_RECOVER_MV   3700U
#define BATTERY_START_MV     3640U
#define BATTERY_PLAUSIBLE_MV 2500U /* lower readings are no cell voltage (its protection cuts off above) */
#define BATTERY_CONFIRM      3U

typedef enum { BATTERY_EVENT_NONE, BATTERY_EVENT_LOW, BATTERY_EVENT_CRITICAL } battery_event_t;

typedef struct {
  uint8_t low_count;
  uint8_t critical_count;
  bool warned;
} battery_watch_t;

void battery_watch_init(battery_watch_t *w);
battery_event_t battery_watch_update(battery_watch_t *w, uint32_t vbat_mv);
/* At power on, before anything starts: enough to switch on? Always with USB power; a missing or
 * implausible reading never keeps the device off. */
bool battery_can_start(uint32_t vbat_mv, bool usb_power);

#endif
