#ifndef HEALTH_H
#define HEALTH_H

#include <stdint.h>

/*
 * Watchdog supervision: the independent watchdog is only fed while every
 * registered task reported alive within the last few seconds.
 */
typedef enum {
  HEALTH_SYSTEM = 1U << 0,
  HEALTH_SOUND = 1U << 1,
  HEALTH_GPS = 1U << 2,
  HEALTH_CONSOLE = 1U << 3,
  HEALTH_BLE = 1U << 4,
  HEALTH_SENSORS = 1U << 5,
} health_task_t;

void health_start(uint32_t expected_tasks);
void health_alive(health_task_t task);

#endif
