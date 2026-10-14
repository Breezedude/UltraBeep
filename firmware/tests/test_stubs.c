/* newlib system calls for the emulator tests: a heap in RAM, no I/O. */
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

void *_sbrk(int incr)
{
  static char heap[16 * 1024];
  static unsigned used;
  if (used + (unsigned)incr > sizeof(heap)) {
    errno = ENOMEM;
    return (void *)-1;
  }
  void *p = &heap[used];
  used += (unsigned)incr;
  return p;
}
