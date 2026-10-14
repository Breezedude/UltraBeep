#ifndef POWER_H
#define POWER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  BOOT_POWER_ON, /* battery connected or brown-out */
  BOOT_BUTTON,   /* woken from standby by the button */
  BOOT_USB,      /* woken from standby by USB power */
  BOOT_UPDATE,   /* the bootloader just ran an update */
  BOOT_RESUME,   /* unexpected reset while switched on (watchdog, fault) */
  BOOT_RECOVERY, /* the bootloader's recovery drive was left with a button press */
  BOOT_PIN_RESET, /* reset pin only: the board's long-press reset (button held some seconds) */
  BOOT_OTHER,
} boot_reason_t;

/* Decodes the bootloader report (RTC BKP14/17/18) of this boot. */
boot_reason_t power_boot_reason(void);
const char *power_boot_reason_name(boot_reason_t reason);
void power_log_boot(void);
bool power_update_succeeded(void);

/* Remembers "switched on" across resets (resume after a watchdog reset). */
void power_set_running(bool running);
/* Counts unexpected resets in a row; power_reset_count_clear() after stable operation. */
uint32_t power_reset_count_increment(void);
void power_reset_count_clear(void);

/* Switches off: waits for the button release and resets; after the reset
 * main() calls power_enter_standby(). The caller has switched off GPS, SD card,
 * audio and USB. */
void power_standby(void) __attribute__((noreturn));
/* Standby with wake-up by button (falling) or USB power (rising); only right
 * after the reset requested by power_standby(). */
void power_enter_standby(void) __attribute__((noreturn));

#endif
