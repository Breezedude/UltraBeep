#ifndef INI_H
#define INI_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Minimal INI handling for the UltraBip configuration files:
 * "[section]" headers, "key=value" lines, comments starting with '#' or ';'.
 */

/* Called for every key; section is "" before the first header. Return false to stop. */
typedef bool (*ini_callback_t)(void *ctx, const char *section, const char *key, const char *value);

void ini_parse(const char *text, ini_callback_t cb, void *ctx);

/*
 * Sets key=value in section, editing the text in place (zero-terminated,
 * buffer size cap). Keeps every other line; adds the key or the section if
 * missing, using the file's line ending. Returns false if the buffer is too small.
 */
bool ini_set(char *text, size_t cap, const char *section, const char *key, const char *value);

bool ini_parse_bool(const char *value);

#endif
