#ifndef BIPLINK_H
#define BIPLINK_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The GATT services the stock firmware offers and the BipLink app looks for before it does
 * anything (it treats a device without them as a firmware that needs an update by computer):
 *
 *   0x180A Device Information   manufacturer, model, serial, firmware / hardware / software revision
 *   0x180F Battery              level in percent, read + notify
 *   0xFFE0 "FlightData"         0xFFE1: NMEA text, "$LK8EX1,..." once a second (XCTrack, XCSoar)
 *   0x570D "FileTransfer"       0x0200: the stock's binary file transfer protocol (requests written
 *                               without response, answers notified; see biplink.c)
 *
 * The firmware revision string has the stock's form "YYYY-MM-DD:hash"; the app compares the date
 * with its catalogue and offers the stock firmware when the device is older. Reporting the latest
 * stock date keeps it quiet; a stock update over BipLink would replace this firmware.
 */
#define BIPLINK_FIRMWARE_VERSION "2026-06-23:48048a99"
#define BIPLINK_AUTH_WINDOW_MS   60000U /* the pilot has this long to confirm a new app by a button press */
#define BIPLINK_SECRET_MAX       8U     /* an app's or configurator's secret: up to 8 characters */

/* Adds the services to the GATT table; call once after the file service. */
bool biplink_add_services(void);
/* A new connection: the Device Information values that change (config file time) are refreshed. */
void biplink_on_connect(void);
void biplink_on_disconnect(void);
/* A write to one of the characteristics; true when handled. */
bool biplink_on_attribute_modified(uint16_t attr_handle, const uint8_t *data, uint16_t len);
/* Once per BLE task loop while connected: file streaming, NMEA and battery notifications. */
void biplink_tick(void);
/* A file is being streamed: the BLE task should loop quickly. */
bool biplink_busy(void);
/* True once after the app uploaded a new configuration file (config.new.ultrabip). */
bool biplink_take_config_update(void);

/*
 * Authorisation by button press, shared by the app and the configurator's file service (ble.c): a client
 * presents a secret it made up once; a known one is accepted at once, a new one waits for a short button
 * press within BIPLINK_AUTH_WINDOW_MS and is then remembered in biplink_secrets_string (for this session
 * only when that list is full). A new request replaces one still waiting.
 */
void biplink_auth_request(const char *secret, bool for_app);
/* A request waits for the button. */
bool biplink_auth_pending(void);
/* The button press: the secret is stored, the client may go on. */
void biplink_auth_confirm(void);
/* The secret is in the stored list, or was confirmed in this session. */
bool biplink_secret_accepted(const char *secret);

#endif
