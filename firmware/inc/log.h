#ifndef LOG_H
#define LOG_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Debug log: lines with a millisecond time stamp go to the USB serial console
 * (when connected), the serial and Bluetooth consoles (when on) and into a RAM
 * ring that is replayed to a console that connects later. Callable from tasks and before the scheduler starts, not
 * from interrupts.
 */
void log_init(void);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Raw console output without time stamp (command replies, NMEA monitor). */
void console_write(const char *data, size_t len);
void console_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Sends the ring buffer to the console (after a console connected). */
void log_replay(void);
/* The same for the serial console (uart_console.h) only. */
void log_replay_uart(void);
/* The same for the Bluetooth console (ble_console.h) only; from the console task. */
void log_replay_ble(void);

#endif
