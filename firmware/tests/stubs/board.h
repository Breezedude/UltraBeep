#ifndef BOARD_H
#define BOARD_H

/* Test replacement of inc/board.h for code under test in the emulator (tests/recorder_stubs.c). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint32_t HAL_GetTick(void);
uint32_t HAL_GetUIDw0(void);
uint32_t HAL_GetUIDw1(void);
uint32_t HAL_GetUIDw2(void);
void board_serial_number(char *out, size_t size);

#endif
