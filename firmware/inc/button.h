#ifndef BUTTON_H
#define BUTTON_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  BUTTON_DOWN,
  BUTTON_UP,   /* ms = how long it was held */
  BUTTON_HOLD, /* while held, every 100 ms; ms = held so far */
} button_event_type_t;

typedef struct {
  button_event_type_t type;
  uint32_t ms;
} button_event_t;

typedef void (*button_handler_t)(const button_event_t *event);

/*
 * Debounced button sampling every 10 ms (software timer). pressed_since_tick
 * != 0 when the button is already held at start-up (wake-up press): the first
 * events then count from that moment.
 */
void button_init(button_handler_t handler, uint32_t pressed_since_tick);

#endif
