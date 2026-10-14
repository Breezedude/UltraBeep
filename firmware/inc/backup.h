#ifndef BACKUP_H
#define BACKUP_H

#include <stdbool.h>
#include <stdint.h>

/*
 * RTC backup registers (kept in standby). The stock bootloader owns BKP14 to
 * BKP19 (its report); this firmware uses BKP0 to BKP10.
 */
typedef enum {
  BACKUP_FLAGS = 0,     /* magic in the upper 16 bits + BACKUP_FLAG_* */
  BACKUP_LAST_SYNC = 1, /* unix time of the last GPS time synchronisation */
  BACKUP_LAST_LAT = 2,  /* last position, 1e-7 degrees */
  BACKUP_LAST_LON = 3,
  BACKUP_RUN_STATE = 4, /* BACKUP_RUNNING while switched on */
  BACKUP_RESETS = 5,    /* unexpected resets in a row */
  BACKUP_FAULT = 6,     /* last fault: code << 24 | detail */
  BACKUP_FAULT_PC = 7,  /* program counter of the last fault */
  BACKUP_STANDBY = 8,   /* BACKUP_STANDBY_REQUEST: enter standby right after the next reset */
  BACKUP_PRESS_MS = 9,  /* rtc_time_ms32() when the button went down, 0 = released */
  BACKUP_AFTER_UPDATE = 10, /* BACKUP_UPDATE_THEN_OFF: announce the result, then stay off */
  BACKUP_RTC_DRIFT = 11, /* RTC rate error measured at the last GPS sync, ppm (int32), see BACKUP_FLAG_DRIFT_KNOWN */
} backup_reg_t;

#define BACKUP_FLAG_TIME_VALID 0x0001U
#define BACKUP_FLAG_DRIFT_KNOWN 0x0002U
#define BACKUP_RUNNING         0x52554E31U /* "RUN1" */
#define BACKUP_STANDBY_REQUEST 0x53544259U /* "STBY" */
#define BACKUP_UPDATE_THEN_OFF 0x55504F46U /* "UPOF" */

void backup_enable_write(void);
uint32_t backup_read(backup_reg_t reg);
void backup_write(backup_reg_t reg, uint32_t value);
bool backup_get_flag(uint32_t flag);
void backup_set_flag(uint32_t flag, bool on);

/* Bootloader report of the current boot (see power.c). */
uint32_t backup_bootloader_report(void); /* BKP14 */
uint32_t backup_wakeup_flags(void);      /* BKP17 = PWR_SR1 at boot */
uint32_t backup_reset_flags(void);       /* BKP18 = RCC_CSR at boot */
bool backup_report_valid(void);          /* BKP19 = 0x00BAD455 */

#endif
