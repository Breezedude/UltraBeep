#include "console_pack.h"

#include <string.h>

#define PREFIX_LINE "CON "
#define PREFIX_PART "CONP "

static size_t put(char *dst, size_t n, const char *s)
{
  size_t len = strlen(s);
  memcpy(dst + n, s, len);
  return n + len;
}

size_t console_pack(const char *ring, uint32_t mask, uint32_t from, uint32_t to, bool flush, char *dst,
                    size_t max, uint32_t *used)
{
  size_t n = 0;
  uint32_t pos = from;
  *used = 0;
  while (pos != to) {
    uint32_t end = pos;
    size_t visible = 0;
    while (end != to && ring[end & mask] != '\n') {
      if (ring[end & mask] != '\r') {
        visible++;
      }
      end++;
    }
    bool complete = end != to;
    size_t need = strlen(complete ? PREFIX_LINE : PREFIX_PART) + visible + 1U;
    if (need <= max - n) {
      if (!complete && (!flush || n > 0)) {
        break; /* the line may go on: wait, or send it on its own in the next packet */
      }
      if (!complete && visible == 0) {
        pos = end; /* only '\r': nothing to show */
        break;
      }
      n = put(dst, n, complete ? PREFIX_LINE : PREFIX_PART);
      for (uint32_t i = pos; i != end; i++) {
        if (ring[i & mask] != '\r') {
          dst[n++] = ring[i & mask];
        }
      }
      dst[n++] = '\n';
      pos = complete ? end + 1U : end;
      continue;
    }
    if (n > 0) {
      break; /* the next packet starts with this line */
    }
    /* longer than one packet: a part of it */
    n = put(dst, n, PREFIX_PART);
    size_t start = n;
    size_t room = max - n - 1U;
    uint32_t i = pos;
    while (i != end && n - start < room) {
      if (ring[i & mask] != '\r') {
        dst[n++] = ring[i & mask];
      }
      i++;
    }
    /* never split a UTF-8 character: end the part before its first byte */
    for (int k = 0; k < 3 && i != end && ((unsigned char)ring[i & mask] & 0xC0U) == 0x80U && n - start > 4U; k++) {
      i--;
      if (ring[i & mask] != '\r') {
        n--;
      }
    }
    dst[n++] = '\n';
    pos = i;
    break;
  }
  *used = pos - from;
  return n;
}
