/*
 * UltraBeep - start-up.
 *
 * A standby request from before the reset is served first (power.c). Before
 * the scheduler starts, a wake-up press of the button is acknowledged (beep +
 * LED) and timed: the power-on gesture continues in the system task.
 *
 * The way back if this firmware misbehaves needs no firmware support: holding
 * the button for some seconds resets the board through its reset pin; with
 * USB connected the stock bootloader then shows the SD card as USB drive
 * (recovery). A short press leaves recovery and starts the firmware.
 */
#include "FreeRTOS.h"
#include "task.h"

#include "audio.h"
#include "backup.h"
#include "ble_app.h"
#include "board.h"
#include "config.h"
#include "console.h"
#include "faults.h"
#include "fmt.h"
#include "gps.h"
#include "health.h"
#include "indicator.h"
#include "log.h"
#include "power.h"
#include "rawlog.h"
#include "rtc_time.h"
#include "sensors.h"
#include "sound.h"
#include "storage.h"
#include "system.h"
#include "version.h"

#define FIRST_PRESS_MAX_MS 1000U /* longer is no power-on gesture: back to standby */

static void early_wake_press(system_boot_t *boot)
{
  config_defaults(config_get()); /* the SD card is read later: default ground level */
  led_set(LED_WHITE);
  sound_beep_blocking(2600, 40, config_get()->volume_buzzer[LEVEL_GROUND]);
  uint32_t start = HAL_GetTick();
  while (button_pressed() && HAL_GetTick() - start < FIRST_PRESS_MAX_MS) {
  }
  led_set(LED_OFF);
  boot->first_release_tick = HAL_GetTick();
  boot->first_press_valid = !button_pressed();
}

/* How long the button was held before a reset by the reset pin (board's long-press reset). */
static void log_press_before_reset(boot_reason_t reason)
{
  uint32_t pressed = backup_read(BACKUP_PRESS_MS);
  backup_write(BACKUP_PRESS_MS, 0);
  if (pressed && reason == BOOT_PIN_RESET) {
    uint32_t held = rtc_time_ms32() - pressed;
    log_printf("The button was held %lu.%03lu s before this reset (includes the bootloader start)",
               (unsigned long)(held / 1000U), (unsigned long)(held % 1000U));
  }
}

int main(void)
{
  HAL_Init();
  board_isr_stack_paint();
  SystemClock_Config();
  board_init();
  audio_init();
  rtc_time_init();

  if (backup_read(BACKUP_STANDBY) == BACKUP_STANDBY_REQUEST) {
    backup_write(BACKUP_STANDBY, 0);
    if (!vbus_present()) {
      power_enter_standby(); /* requested before the reset (power.c) */
    }
  }

  system_boot_t boot = {power_boot_reason(), 0, true};
  if (boot.reason == BOOT_BUTTON) {
    early_wake_press(&boot);
  }
  watchdog_start();

  fmt_init();
  log_init();
  storage_init();
  log_printf("%s %s (%s %s), start: %s", FW_NAME, FW_VERSION, __DATE__, __TIME__,
             power_boot_reason_name(boot.reason));
  power_log_boot();
  log_press_before_reset(boot.reason);
  faults_log_last();

  indicator_init();
  sound_init();
  sensors_init();
  rawlog_init();
  gps_init();
  console_init();
  ble_init();
  system_start(&boot);
  health_start(HEALTH_SYSTEM | HEALTH_SOUND | HEALTH_GPS | HEALTH_CONSOLE | HEALTH_BLE | HEALTH_SENSORS);
  vTaskStartScheduler();
  for (;;) {
  }
}
