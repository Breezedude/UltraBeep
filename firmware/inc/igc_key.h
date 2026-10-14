#ifndef IGC_KEY_H
#define IGC_KEY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Secret key for the IGC G record (HMAC-SHA-256). Built from
 * keys/igc_signing.key, which is not in the repository; without it
 * igc_key_available is false and no G record is written.
 */
#define IGC_KEY_SIZE 32

extern const bool igc_key_available;
extern const uint8_t igc_key[IGC_KEY_SIZE];

#endif
