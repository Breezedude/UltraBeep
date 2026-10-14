#ifndef IDENTITY_H
#define IDENTITY_H

#include <stddef.h>
#include <stdint.h>

/*
 * The Bluetooth identity the stock firmware gives a device, rebuilt so that a device keeps its
 * address and name across a firmware change (the BipLink app and the phone know it by them).
 * Both come from the 64-bit unique device id (UID64): the unique number (UDN, low word) and the
 * device id (low byte of the high word). No hardware access: unit-tested in tests/logic_tests.c.
 */

/* Longest advertised name: 31 bytes of advertising data minus the flags and the name's header. */
#define IDENTITY_NAME_MAX 26

/*
 * Public address 57:0D:35:<device id>:<UDN bits 15..8>:<UDN bits 7..0>, in the controller's
 * byte order (addr[0] = least significant byte).
 */
void identity_ble_address(uint8_t addr[6], uint32_t udn, uint8_t device_id);

/*
 * "UltraBip" + U+1FA82 (parachute) + the first word of the pilot's name (up to a space or ':'),
 * e.g. "UltraBip🪂Florian". Without a pilot name the last four hex digits of the serial number
 * take its place ("UltraBip🪂EFF7"). When the word and the 4-byte emoji do not fit into
 * IDENTITY_NAME_MAX, "_" separates instead and the word is cut. Returns the length in bytes.
 */
size_t identity_device_name(char *out, size_t size, const char *pilot, uint32_t udn);

/*
 * The same with an own text after the emoji (system_blename_string), taken as it is, spaces
 * included. Cut at a character boundary when it is too long. "UltraBip" stays in front: the
 * BipLink app recognises the device model by it.
 */
size_t identity_device_name_with(char *out, size_t size, const char *text, uint32_t udn);

#endif
