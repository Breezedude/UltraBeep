#ifndef CONSOLE_PACK_H
#define CONSOLE_PACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Console output as status lines of the Bluetooth file service (docs/ble-file-service.md):
 * "CON <text>\n" for each complete line, "CONP <text>\n" for text whose line end has not come yet
 * (the client continues the line with the next one). '\r' is dropped.
 *
 * ring/mask: the waiting output, read at ring[i & mask] for i from `from` to `to` (free running
 * positions; mask = size - 1 of a power-of-two ring, UINT32_MAX for a plain buffer). flush: text
 * without a line end goes out as well (nothing more came for a moment). A line longer than one
 * packet goes out in "CONP" parts, never cut inside a UTF-8 character.
 *
 * Returns the bytes written to dst (at most max; max >= 16), *used the bytes of the ring they
 * cover. Both can be 0: nothing to send yet. A lone '\r' at a flush is used up without output.
 */
size_t console_pack(const char *ring, uint32_t mask, uint32_t from, uint32_t to, bool flush, char *dst,
                    size_t max, uint32_t *used);

#endif
