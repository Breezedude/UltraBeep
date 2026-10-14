#ifndef FAULTS_H
#define FAULTS_H

enum {
  FAULT_NONE = 0,
  FAULT_HARD = 1,
  FAULT_ASSERT = 2,
  FAULT_STACK = 3,
  FAULT_MALLOC = 4,
};

void fault_assert(const char *file, int line) __attribute__((noreturn));
/* Logs (and clears) the cause of the previous reset, if it was a fault. */
void faults_log_last(void);

#endif
