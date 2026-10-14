#include "ini.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define LINE_MAX_LEN 160
#define VALUE_MAX_LEN 400 /* global_timechanges_longstring: 31 entries of 11 characters */

static const char *skip_space(const char *p, const char *end)
{
  while (p < end && (*p == ' ' || *p == '\t')) {
    p++;
  }
  return p;
}

static const char *trim_end(const char *start, const char *end)
{
  while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) {
    end--;
  }
  return end;
}

static void copy_span(char *dst, size_t size, const char *a, const char *b)
{
  size_t n = (size_t)(b - a);
  if (n >= size) {
    n = size - 1U;
  }
  memcpy(dst, a, n);
  dst[n] = 0;
}

void ini_parse(const char *text, ini_callback_t cb, void *ctx)
{
  char section[32] = "";
  char key[64];
  char value[VALUE_MAX_LEN];
  const char *p = text;
  while (*p) {
    const char *eol = strchr(p, '\n');
    const char *end = eol ? eol : p + strlen(p);
    const char *a = skip_space(p, end);
    const char *b = trim_end(a, end);
    if (a < b && *a == '[') {
      const char *close = memchr(a, ']', (size_t)(b - a));
      if (close) {
        copy_span(section, sizeof(section), a + 1, close);
      }
    } else if (a < b && *a != '#' && *a != ';') {
      const char *eq = memchr(a, '=', (size_t)(b - a));
      if (eq) {
        copy_span(key, sizeof(key), a, trim_end(a, eq));
        copy_span(value, sizeof(value), skip_space(eq + 1, b), b);
        if (!cb(ctx, section, key, value)) {
          return;
        }
      }
    }
    if (!eol) {
      break;
    }
    p = eol + 1;
  }
}

bool ini_parse_bool(const char *value)
{
  return strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "TRUE") == 0 ||
         strcmp(value, "True") == 0;
}

/* Inserts s at position pos; false if it does not fit. */
static bool insert_at(char *text, size_t cap, size_t pos, const char *s)
{
  size_t len = strlen(text);
  size_t n = strlen(s);
  if (len + n + 1U > cap) {
    return false;
  }
  memmove(text + pos + n, text + pos, len - pos + 1U);
  memcpy(text + pos, s, n);
  return true;
}

static bool replace_span(char *text, size_t cap, size_t from, size_t to, const char *s)
{
  size_t len = strlen(text);
  size_t n = strlen(s);
  size_t old = to - from;
  if (len - old + n + 1U > cap) {
    return false;
  }
  memmove(text + from + n, text + to, len - to + 1U);
  memcpy(text + from, s, n);
  return true;
}

bool ini_set(char *text, size_t cap, const char *section, const char *key, const char *value)
{
  const char *nl = strstr(text, "\r\n") ? "\r\n" : "\n";
  char current[32] = "";
  bool in_section = section[0] == 0;
  bool section_found = in_section;
  size_t insert_pos = 0; /* after the last key line of the section */
  size_t key_len = strlen(key);
  char *p = text;

  while (*p) {
    char *eol = strchr(p, '\n');
    char *end = eol ? eol : p + strlen(p);
    const char *a = skip_space(p, end);
    const char *b = trim_end(a, end);
    size_t line_end = (size_t)((eol ? eol + 1 : end) - text);

    if (a < b && *a == '[') {
      const char *close = memchr(a, ']', (size_t)(b - a));
      if (close) {
        if (in_section) {
          break; /* end of our section */
        }
        copy_span(current, sizeof(current), a + 1, close);
        in_section = strcmp(current, section) == 0;
        if (in_section) {
          section_found = true;
          insert_pos = line_end;
        }
      }
    } else if (in_section && a < b && *a != '#' && *a != ';') {
      const char *eq = memchr(a, '=', (size_t)(b - a));
      if (eq && (size_t)(trim_end(a, eq) - a) == key_len && strncmp(a, key, key_len) == 0) {
        size_t vstart = (size_t)(skip_space(eq + 1, b) - text);
        size_t vend = (size_t)(b - text);
        return replace_span(text, cap, vstart, vend, value);
      }
      insert_pos = line_end;
    }
    if (!eol) {
      break;
    }
    p = eol + 1;
  }

  char line[LINE_MAX_LEN];
  if (section_found) {
    size_t len = strlen(text);
    bool need_nl = insert_pos == len && len > 0 && text[len - 1] != '\n';
    snprintf(line, sizeof(line), "%s%s=%s%s", need_nl ? nl : "", key, value, nl);
    return insert_at(text, cap, insert_pos, line);
  }
  size_t len = strlen(text);
  bool need_nl = len > 0 && text[len - 1] != '\n';
  snprintf(line, sizeof(line), "%s%s[%s]%s%s=%s%s", need_nl ? nl : "", len > 0 ? nl : "", section, nl, key, value,
           nl);
  return insert_at(text, cap, len, line);
}
