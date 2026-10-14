#include "identity.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

void identity_ble_address(uint8_t addr[6], uint32_t udn, uint8_t device_id)
{
  addr[0] = (uint8_t)udn;
  addr[1] = (uint8_t)(udn >> 8);
  addr[2] = device_id;
  addr[3] = 0x35;
  addr[4] = 0x0D;
  addr[5] = 0x57;
}

/* Appends up to max - *n bytes of s, never splitting a UTF-8 character. */
static void append(char *out, size_t *n, size_t max, const char *s)
{
  size_t len = strlen(s);
  if (len > max - *n) {
    len = max - *n;
    while (len && ((unsigned char)s[len] & 0xC0U) == 0x80U) {
      len--; /* s[len] continues a character: cut before it starts */
    }
  }
  memcpy(out + *n, s, len);
  *n += len;
}

static size_t build_name(char *out, size_t size, const char *word)
{
  size_t max = size - 1 < IDENTITY_NAME_MAX ? size - 1 : IDENTITY_NAME_MAX;
  size_t n = 0;
  append(out, &n, max, "UltraBip");
  append(out, &n, max, strlen(word) + 4U <= max - n ? "\xF0\x9F\xAA\x82" : "_");
  append(out, &n, max, word);
  out[n] = 0;
  return n;
}

size_t identity_device_name_with(char *out, size_t size, const char *text, uint32_t udn)
{
  if (size == 0) {
    return 0;
  }
  while (text && *text == ' ') {
    text++;
  }
  if (!text || !*text) {
    return identity_device_name(out, size, "", udn);
  }
  return build_name(out, size, text);
}

size_t identity_device_name(char *out, size_t size, const char *pilot, uint32_t udn)
{
  if (size == 0) {
    return 0;
  }
  char word[32];
  size_t w = 0;
  for (const char *p = pilot; p && *p && w < sizeof(word) - 1; p++) {
    unsigned char ch = (unsigned char)*p;
    if (isspace(ch) || ch == ':') {
      break;
    }
    word[w++] = (char)ch;
  }
  word[w] = 0;
  if (w == 0) {
    snprintf(word, sizeof(word), "%02X%02X", (unsigned)((udn >> 8) & 0xFFU), (unsigned)(udn & 0xFFU));
    w = strlen(word);
  }

  return build_name(out, size, word);
}
