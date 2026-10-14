/* Minimal newlib system call stubs (no file I/O through the C library). */
#include <errno.h>
#include <sys/stat.h>

int _close(int fd) { (void)fd; return -1; }
int _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _getpid(void) { return 1; }
int _isatty(int fd) { (void)fd; return 1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
int _lseek(int fd, int ptr, int dir) { (void)fd; (void)ptr; (void)dir; return 0; }
int _read(int fd, char *ptr, int len) { (void)fd; (void)ptr; (void)len; return 0; }
int _write(int fd, const char *ptr, int len) { (void)fd; (void)ptr; return len; }

void _exit(int status)
{
  (void)status;
  for (;;) {
  }
}

/* Heap from the end of .bss up to the reserved stack area (see linker script). */
void *_sbrk(int incr)
{
  extern char end;
  extern char _estack;
  extern char _Min_Stack_Size;
  static char *heap_end;
  char *stack_limit = &_estack - (unsigned)&_Min_Stack_Size;

  if (!heap_end) {
    heap_end = &end;
  }
  if (heap_end + incr > stack_limit) {
    errno = ENOMEM;
    return (void *)-1;
  }
  char *prev = heap_end;
  heap_end += incr;
  return prev;
}

/* newlib's malloc (used by printf for floating point) from several tasks. */
#include "FreeRTOS.h"
#include "task.h"

struct _reent;

void __malloc_lock(struct _reent *r)
{
  (void)r;
  vTaskSuspendAll();
}

void __malloc_unlock(struct _reent *r)
{
  (void)r;
  xTaskResumeAll();
}
