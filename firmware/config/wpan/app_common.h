/*
 * Minimal replacement of the CubeWB application headers required by the
 * STM32_WPAN transport layer (tl_mbox, shci, hci_tl) and hw_ipcc.c.
 */
#ifndef APP_COMMON_H
#define APP_COMMON_H

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_conf.h"

#define BACKUP_PRIMASK()  uint32_t primask_bit = __get_PRIMASK()
#define DISABLE_IRQ()     __disable_irq()
#define RESTORE_PRIMASK() __set_PRIMASK(primask_bit)

#ifndef MAX
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#endif
#ifndef MIN
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#endif
#define DIVC(x, y) (((x) + (y)-1) / (y))

#define PLACE_IN_SECTION(__x__) __attribute__((section(__x__)))
#define ALIGN(n)                __attribute__((aligned(n)))

#endif
