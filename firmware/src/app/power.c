#include "power.h"
#include "rtc_time.h"

#include "backup.h"
#include "board.h"
#include "log.h"
#include "sensors.h"
#include "stm32wbxx_ll_pwr.h"

/* Bootloader report bits (BKP14) */
#define BL_BUTTON  (1U << 0)
#define BL_VBUS    (1U << 1)
#define BL_RAN     (1U << 2)
#define BL_UPDATE  (1U << 3)
#define BL_FAIL    (1U << 4)
#define BL_SUCCESS (1U << 5)
#define BL_RECOVER (1U << 6)
#define BL_ERROR   (1U << 8)

boot_reason_t power_boot_reason(void)
{
  uint32_t report = backup_report_valid() ? backup_bootloader_report() : 0U;
  uint32_t wake = backup_wakeup_flags();
  uint32_t csr = backup_reset_flags();
  bool running = backup_read(BACKUP_RUN_STATE) == BACKUP_RUNNING;

  /* Update attempted with a result; "update" alone means: no file (e.g. leaving recovery). */
  if ((report & BL_UPDATE) && (report & (BL_SUCCESS | BL_FAIL))) {
    return BOOT_UPDATE;
  }
  if (report & BL_RECOVER) {
    return BOOT_RECOVERY;
  }
  if (wake & PWR_SR1_WUF2) {
    return BOOT_BUTTON;
  }
  if (wake & PWR_SR1_WUF5) {
    return BOOT_USB;
  }
  if (csr & RCC_CSR_BORRSTF) {
    return BOOT_POWER_ON;
  }
  /* Only the reset pin: the board resets itself when the button is held for some seconds. */
  if ((csr & 0xFE000000U) == RCC_CSR_PINRSTF) {
    return BOOT_PIN_RESET;
  }
  if (running) {
    return BOOT_RESUME;
  }
  return BOOT_OTHER;
}

bool power_update_succeeded(void)
{
  return (backup_bootloader_report() & BL_SUCCESS) != 0U;
}

const char *power_boot_reason_name(boot_reason_t reason)
{
  switch (reason) {
  case BOOT_POWER_ON: return "power-on reset";
  case BOOT_BUTTON: return "button";
  case BOOT_USB: return "USB power";
  case BOOT_UPDATE: return "after update";
  case BOOT_RESUME: return "resume after reset";
  case BOOT_RECOVERY: return "back from bootloader recovery";
  case BOOT_PIN_RESET: return "reset by long button press";
  default: return "other";
  }
}

void power_log_boot(void)
{
  uint32_t report = backup_bootloader_report();
  uint32_t csr = backup_reset_flags();
  log_printf("Bootloader BL_v%lu report 0x%08lX%s: %s%s%s%s%s%s%s", (unsigned long)((report >> 16) & 0xFFU),
             (unsigned long)report, backup_report_valid() ? "" : " (invalid)", report & BL_BUTTON ? "button " : "",
             report & BL_VBUS ? "ext-power " : "", report & BL_UPDATE ? "update " : "",
             report & BL_SUCCESS ? "SUCCESS " : "", report & BL_FAIL ? "FAIL " : "",
             report & BL_RECOVER ? "recovery " : "", report & BL_ERROR ? "error " : "");
  uint32_t pa, ps;
  rtc_time_prescalers(&pa, &ps);
  int32_t ppm = 0;
  bool drift = rtc_time_drift(&ppm);
  log_printf("RTC: clock %s, prescalers %lu/%lu, rate error %s%ld ppm", rtc_time_source_name(), (unsigned long)pa,
             (unsigned long)ps, drift ? "" : "not measured ", (long)(drift ? ppm : 0));
  log_printf("Reset flags 0x%08lX:%s%s%s%s%s%s, wake-up flags 0x%02lX", (unsigned long)csr,
             csr & RCC_CSR_BORRSTF ? " BOR" : "", csr & RCC_CSR_PINRSTF ? " PIN" : "",
             csr & RCC_CSR_SFTRSTF ? " SW" : "", csr & RCC_CSR_IWDGRSTF ? " IWDG" : "",
             csr & RCC_CSR_WWDGRSTF ? " WWDG" : "", csr & RCC_CSR_OBLRSTF ? " OBL" : "",
             (unsigned long)backup_wakeup_flags());
}

void power_set_running(bool running)
{
  backup_write(BACKUP_RUN_STATE, running ? BACKUP_RUNNING : 0U);
}

uint32_t power_reset_count_increment(void)
{
  uint32_t n = backup_read(BACKUP_RESETS) + 1U;
  backup_write(BACKUP_RESETS, n);
  return n;
}

void power_reset_count_clear(void)
{
  backup_write(BACKUP_RESETS, 0);
}

/*
 * Standby is always entered from a fresh reset: power_standby() leaves a
 * request in a backup register and resets, main() calls power_enter_standby()
 * before anything else is started. So no peripheral, DMA transfer or the radio
 * core (CPU2) is still running; C2BOOT is cleared, so the radio core stays off
 * after the reset and after the next wake-up.
 */
void power_standby(void)
{
  while (button_pressed()) {
    watchdog_feed(); /* a held button would wake the device at once */
  }
  __disable_irq();
  led_force(LED_OFF);
  power_set_running(false);
  LL_PWR_DisableBootC2();
  backup_write(BACKUP_STANDBY, BACKUP_STANDBY_REQUEST);
  NVIC_SystemReset();
}

void power_enter_standby(void)
{
  /* Frozen in standby (option byte IWDG_STDBY = 0); resets the device if standby is refused. */
  watchdog_start();
  sensors_sleep_hw(); /* no power switch: barometer and IMU would keep measuring */
  while (button_pressed()) {
    watchdog_feed();
  }
  __disable_irq();
  led_force(LED_OFF);
  LL_PWR_DisableBootC2();
  LL_C2_PWR_SetPowerMode(LL_PWR_MODE_SHUTDOWN);

  /* WFI returns at once while any interrupt is pending, even with interrupts
   * disabled: stop the tick sources and clear everything pending. */
  TIM17->DIER = 0;
  TIM17->CR1 = 0;
  TIM17->SR = 0;
  SysTick->CTRL = 0;
  for (uint32_t i = 0; i < sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0]); i++) {
    NVIC->ICER[i] = 0xFFFFFFFFU;
    NVIC->ICPR[i] = 0xFFFFFFFFU;
  }
  SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;

  /*
   * Pins while the GPIOs are unpowered. No pull-ups: they pull to VDD (about
   * 3.3 V), not to the battery, and the active-low load switches (GNSS PC6,
   * SD card PC14, blue LED PB1) are held off by external pull-ups. A gate
   * pulled to 3.3 V while the battery is above about 3.7 V starts to conduct:
   * the standby current rose from 3.5 uA at 3.6 V to 2.4 mA at 4.2 V. Like the
   * stock firmware, these pins and the button (PC13) float. Pull-downs only on
   * the loads that a high level switches on, and on VBUS and the SD card select.
   */
  HAL_GPIO_WritePin(SENS_PWR_PORT, SENS_PWR_PIN, GPIO_PIN_RESET); /* sensors off, as the stock does before standby */
  HAL_PWREx_EnableGPIOPullDown(PWR_GPIO_C, PWR_GPIO_BIT_5 | PWR_GPIO_BIT_15);
  HAL_PWREx_EnableGPIOPullDown(PWR_GPIO_B, PWR_GPIO_BIT_0 | PWR_GPIO_BIT_2 | PWR_GPIO_BIT_12);
  HAL_PWREx_EnableGPIOPullDown(PWR_GPIO_D, PWR_GPIO_BIT_0); /* SD card select: no current into the unpowered card */
  HAL_PWREx_EnablePullUpPullDownConfig();

  HAL_PWR_DisableWakeUpPin(PWR_WAKEUP_PIN2);
  HAL_PWR_DisableWakeUpPin(PWR_WAKEUP_PIN5);
  HAL_PWR_EnableWakeUpPin(PWR_WAKEUP_PIN2_LOW);
  HAL_PWR_EnableWakeUpPin(PWR_WAKEUP_PIN5_HIGH);
  for (;;) {
    PWR->SCR = PWR_SCR_CWUF;
    HAL_PWR_EnterSTANDBYMode(); /* returns only if standby was refused; the watchdog ends this */
  }
}
