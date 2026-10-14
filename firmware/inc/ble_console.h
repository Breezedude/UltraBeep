#ifndef BLE_CONSOLE_H
#define BLE_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>

#include "health.h"

/*
 * The debug console over Bluetooth (the configurator's console view): after "CONSOLE on" on the
 * file service the log and the console answers go to the phone as status lines too
 * (console_pack.h), and "CON <line>" types a command line. The buffer is small: when Bluetooth
 * cannot keep up, log lines of other tasks are dropped (counted and reported); the console task's
 * own output waits instead, so that a long answer arrives complete.
 */

/* Console side (log.c, console.c) */

/* The calling task (the console) waits for room instead of dropping its output, and meanwhile
 * reports alive under this id. */
void ble_console_patient_task(health_task_t alive);
void ble_console_write(const char *data, size_t len);
bool ble_console_active(void);
/* The next character typed on the phone, or -1. */
int ble_console_getc(void);
/* True once after "CONSOLE on": the console greets and replays the log. */
bool ble_console_greeting(void);

/* Bluetooth side (ble.c, its task only) */

void ble_console_enable(bool on);
/* A command line from the phone; false when it does not fit (or the console is off). */
bool ble_console_input(const char *line);
/* The next status notification, 0 bytes when nothing is to be sent now; ble_console_sent()
 * once it went out. */
size_t ble_console_take(char *dst, size_t max);
void ble_console_sent(void);
/* Output is waiting: the Bluetooth task should come back soon. */
bool ble_console_busy(void);

#endif
