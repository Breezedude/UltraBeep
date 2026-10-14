#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdbool.h>

/*
 * Console task: keeps USB in step with the cable (serial port always, SD card
 * as drive when the system task asks for it), replays the log to a terminal
 * that connects and executes debug commands ("help").
 */
void console_init(void);
bool console_usb_drive_active(void);

#endif
