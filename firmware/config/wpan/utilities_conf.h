#ifndef UTILITIES_CONF_H
#define UTILITIES_CONF_H

#include <string.h>

#include "cmsis_compiler.h"
#include "app_conf.h"

#define UTILS_ENTER_CRITICAL_SECTION() \
  uint32_t primask_bit = __get_PRIMASK(); \
  __disable_irq()
#define UTILS_EXIT_CRITICAL_SECTION() __set_PRIMASK(primask_bit)
#define UTILS_MEMSET8(dest, value, size) memset(dest, value, size)

#endif
