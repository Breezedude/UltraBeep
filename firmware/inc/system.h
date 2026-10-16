#ifndef SYSTEM_H
#define SYSTEM_H

#include <stdbool.h>
#include <stdint.h>

#include "power.h"
#include "vario.h"

/*
 * Device states:
 *   OFF/standby -> power-on gesture -> profile selection -> ground
 *   -> (later: flight) -> power-off melody -> standby
 *   USB mode: switched off with USB power, SD card as USB drive.
 */
typedef struct {
  boot_reason_t reason;
  uint32_t first_release_tick; /* wake-up press released (gesture timing) */
  bool first_press_valid;      /* false: held too long, no gesture */
} system_boot_t;

void system_start(const system_boot_t *boot);
const char *system_state_name(void);
/* Takeoff detection: ground, flight or landed. */
const char *system_flight_state_name(void);
uint8_t system_profile(void);

/* Requests from the debug console. */
void system_request_power_off(void);
void system_request_update(void);
/* Configurator: speaks a block list with fixed demo values (announce_demo_input); switched on, not in flight. */
void system_test_announce(const char *trigger, const char *items);
/* Vario tones on the ground for testing at this volume in percent; 0 = off. */
void system_vario_test(uint8_t level);

/* Configurator tests over Bluetooth: only on the ground (switched on, not in flight). */
bool system_test_allowed(void);
/* A tone curve to try instead of the profile's (copied); system_test_stop() ends all tests. */
void system_test_tones(const vario_tones_t *tones);
void system_test_stop(void);
uint32_t system_battery_mv(void); /* last reading every 10 s, 0 = none yet */
bool system_in_flight(void);
/* The device may take a second Bluetooth link now (ble.c advertises while connected). */
bool system_second_link_allowed(void);
/* The configurator saved new files and asks for them at once (BLE task); the system task reloads. */
void system_request_config_reload(void);
/* A clip in voices/custom was written or removed: the extension clips are looked up again. */
void system_voice_clips_changed(void);
/* Local time for a UTC time: the profile's offset, plus European summer time if automatic. */
uint32_t system_local_unix(uint32_t utc);
/* Double taps accepted since the start (configurator: live values). */
uint32_t system_double_taps(void);

/* Console task: is the SD card offered as USB drive? */
bool system_wants_usb_drive(void);

#endif
