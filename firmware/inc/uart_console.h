#ifndef UART_CONSOLE_H
#define UART_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * The debug console on the stock firmware's serial pins as well (system_uartconsole_bool, default
 * off): LPUART1, RX on PC0, TX on PC1 (AF8), 1,000,000 baud 8N1 as the stock, 3.3 V levels. The
 * log and the console answers go to the USB console and here at the same time; commands typed on
 * either side are executed. Transmission runs from a ring buffer by interrupt; when it is full,
 * further output is dropped rather than blocking the tasks.
 */
#define UART_CONSOLE_BAUD 1000000U

void uart_console_enable(bool on);
bool uart_console_enabled(void);
void uart_console_write(const char *data, size_t len);
/* The next received character, or -1. */
int uart_console_getc(void);

#endif
