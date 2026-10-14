#ifndef USB_DEVICE_H
#define USB_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Starts the composite device. with_msc exposes the SD card as a drive; the
 * file system must not be mounted by the firmware while USB is running. */
bool usb_start(bool with_msc);
void usb_stop(void);
bool usb_is_configured(void);
uint32_t usb_msc_idle_ms(void);

bool usb_cdc_connected(void);
/* Next character typed in the terminal, -1 if none. */
int usb_cdc_getc(void);
/* Shows/hides the SD card as drive while USB stays connected. */
void usb_msc_set_ready(bool ready);
bool usb_is_started(void);
/* Start USB (COM port only) when the cable is plugged, stop when unplugged. */
void usb_follow_cable(void);
void usb_cdc_write(const char *data, size_t len);

#endif
