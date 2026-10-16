#ifndef FMT_H
#define FMT_H

#include <stdarg.h>
#include <stddef.h>

/*
 * Formatting with floats, serialised. newlib's %f (_dtoa_r) works on the one global reent: its result
 * buffer and its free list of big-number blocks are shared by every task, and __malloc_lock covers only
 * the malloc inside. Two tasks formatting a float at the same time garble each other's digits and can
 * corrupt that free list. Every vsnprintf that may carry a %f goes through here. The lock is skipped
 * before the scheduler runs, in interrupts and while the scheduler is suspended (nothing formats there).
 */
void fmt_init(void);
int fmt_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int fmt_snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
/* value with `decimals` (up to 6) digits after the point, rounded, e.g. "-3.25"; no newlib float path. */
int fmt_fixed(char *out, size_t size, float value, unsigned decimals);

#endif
