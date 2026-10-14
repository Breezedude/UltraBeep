#ifndef REMOTE_H
#define REMOTE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Bluetooth remote (bluetooth_remoteenable_bool), as the stock firmware does it: the device is a
 * Bluetooth keyboard (HID over GATT, boot keyboard report map) and a double tap presses the space
 * bar for 50 ms. Flight apps such as XCTrack can bind an action to that key. The phone pairs and
 * bonds with the device (no PIN), which the HID service needs.
 *
 * The service and the pairing settings exist only when the remote is switched on at power on.
 * All calls except remote_press() and remote_ready() run in the BLE task.
 */

#define REMOTE_KEY        0x2C /* HID usage: space bar */
#define REMOTE_PRESS_MS   50U

/* Pairing settings and the HID service; call in the BLE start-up before advertising. */
bool remote_add_service(void);
void remote_on_connect(void);
void remote_on_disconnect(void);
/* A write to one of the HID attributes (the report's notifications); true when handled. */
bool remote_on_attribute_modified(uint16_t attr_handle, const uint8_t *data, uint16_t len);
/* Once per BLE task loop while connected: sends the key press and its release. */
void remote_tick(void);
/* A key press or release is pending: the BLE task should loop quickly. */
bool remote_busy(void);

/* A host listens to the key reports (paired and subscribed). Any task. */
bool remote_ready(void);
/* Asks for one press of REMOTE_KEY. Any task; the caller wakes the BLE task. */
void remote_press(void);

#endif
