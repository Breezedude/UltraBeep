#ifndef BLE_APP_H
#define BLE_APP_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Bluetooth Low Energy on the radio core (CPU2 keeps the wireless stack that
 * is installed on the device). Offers the file service of the configurator
 * (docs/ble-file-service.md).
 */
typedef struct {
  bool running;     /* host started, advertising or connected */
  bool connected;   /* at least one link */
  uint8_t links;    /* connected centrals: the phone's app and the configurator at once (at most 2) */
  uint16_t mtu;
  uint16_t interval; /* connection interval in units of 1.25 ms */
  uint8_t phy;       /* 1 or 2 (Mbit/s) */
  int8_t rssi;
  uint8_t fw_major, fw_minor, fw_sub; /* wireless stack */
  uint8_t fus_major, fus_minor, fus_sub;
  uint8_t stack_type;
} ble_info_t;

void ble_init(void);
/* Boots CPU2 (once) and starts advertising; asynchronous. */
void ble_enable(void);
/* Stops advertising and disconnects. */
void ble_disable(void);
void ble_get_info(ble_info_t *info);
const char *ble_name(void);
/* Bluetooth remote (remote.h): a paired phone listens to key reports; one key press. */
bool ble_remote_ready(void);
void ble_remote_press(void);

#endif
