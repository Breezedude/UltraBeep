#include "rtc_time.h"

#include "backup.h"
#include "board.h"

#define LSI_ASYNC_PREDIV 127U /* 32 kHz / 128 / 250 = 1 Hz */
#define LSI_SYNC_PREDIV  249U

static uint8_t to_bcd(uint8_t v)
{
  return (uint8_t)(((v / 10U) << 4) | (v % 10U));
}

static uint8_t from_bcd(uint32_t v)
{
  return (uint8_t)(((v >> 4) & 0x0FU) * 10U + (v & 0x0FU));
}

void rtc_time_init(void)
{
  BOARD_ATOMIC(__HAL_RCC_RTCAPB_CLK_ENABLE());
  backup_enable_write();
  /* Keep a running RTC (the stock firmware set it up); otherwise clock it from LSI. */
  if ((RCC->BDCR & RCC_BDCR_RTCSEL) == 0U) {
    RCC->BDCR |= RCC_BDCR_RTCSEL_1; /* LSI */
  }
  RCC->BDCR |= RCC_BDCR_RTCEN;
}

bool rtc_time_valid(void)
{
  return backup_get_flag(BACKUP_FLAG_TIME_VALID);
}

uint32_t rtc_time_unix(void)
{
  uint32_t tr = RTC->TR;
  uint32_t dr = RTC->DR;
  if (tr != RTC->TR) { /* read across a second boundary */
    tr = RTC->TR;
    dr = RTC->DR;
  }
  uint8_t sec = from_bcd(tr & 0x7FU);
  uint8_t min = from_bcd((tr >> 8) & 0x7FU);
  uint8_t hour = from_bcd((tr >> 16) & 0x3FU);
  uint8_t day = from_bcd(dr & 0x3FU);
  uint8_t month = from_bcd((dr >> 8) & 0x1FU);
  uint16_t year = (uint16_t)(2000U + from_bcd((dr >> 16) & 0xFFU));
  if (month == 0 || day == 0) {
    return 0;
  }
  return unix_from_civil(year, month, day, hour, min, sec);
}

void rtc_time_sync(uint32_t unix_time)
{
  uint16_t year;
  uint8_t month, day, hour, min, sec;
  civil_from_unix(unix_time, &year, &month, &day, &hour, &min, &sec);
  backup_enable_write();
  RTC->WPR = 0xCAU;
  RTC->WPR = 0x53U;
  RTC->ISR |= RTC_ISR_INIT;
  uint32_t start = HAL_GetTick();
  while (!(RTC->ISR & RTC_ISR_INITF) && HAL_GetTick() - start < 100U) {
  }
  if ((RCC->BDCR & RCC_BDCR_RTCSEL) == RCC_BDCR_RTCSEL_1) {
    RTC->PRER = (LSI_ASYNC_PREDIV << 16) | LSI_SYNC_PREDIV;
  }
  RTC->CR &= ~RTC_CR_FMT; /* 24 h */
  RTC->TR = ((uint32_t)to_bcd(hour) << 16) | ((uint32_t)to_bcd(min) << 8) | to_bcd(sec);
  RTC->DR = ((uint32_t)to_bcd((uint8_t)(year - 2000U)) << 16) | ((uint32_t)to_bcd(month) << 8) | to_bcd(day) |
            (1U << 13); /* weekday must not be 0 */
  RTC->ISR &= ~RTC_ISR_INIT;
  RTC->WPR = 0xFFU;
  backup_write(BACKUP_LAST_SYNC, unix_time);
  backup_set_flag(BACKUP_FLAG_TIME_VALID, true);
}

uint32_t rtc_time_ms32(void)
{
  uint32_t ssr = RTC->SSR;
  uint32_t sec = rtc_time_unix();
  uint32_t prediv_s = RTC->PRER & RTC_PRER_PREDIV_S;
  uint32_t ms = (prediv_s - (ssr & 0xFFFFU)) * 1000U / (prediv_s + 1U);
  return sec * 1000U + ms;
}

const char *rtc_time_source_name(void)
{
  switch (RCC->BDCR & RCC_BDCR_RTCSEL) {
  case RCC_BDCR_RTCSEL_0:
    return "LSE";
  case RCC_BDCR_RTCSEL_1:
    return "LSI";
  case RCC_BDCR_RTCSEL:
    return "HSE/32 (stops in standby)";
  default:
    return "none";
  }
}

bool rtc_time_runs_while_off(void)
{
  uint32_t sel = RCC->BDCR & RCC_BDCR_RTCSEL;
  return sel == RCC_BDCR_RTCSEL_0 || sel == RCC_BDCR_RTCSEL_1;
}

void rtc_time_prescalers(uint32_t *async, uint32_t *sync)
{
  *async = (RTC->PRER & RTC_PRER_PREDIV_A) >> RTC_PRER_PREDIV_A_Pos;
  *sync = RTC->PRER & RTC_PRER_PREDIV_S;
}

bool rtc_time_drift(int32_t *ppm)
{
  if (!backup_get_flag(BACKUP_FLAG_DRIFT_KNOWN)) {
    return false;
  }
  *ppm = (int32_t)backup_read(BACKUP_RTC_DRIFT);
  return true;
}

void rtc_time_set_drift(int32_t ppm)
{
  backup_write(BACKUP_RTC_DRIFT, (uint32_t)ppm);
  backup_set_flag(BACKUP_FLAG_DRIFT_KNOWN, true);
}

bool rtc_time_estimate(uint32_t *unix_time, uint32_t *age_s)
{
  int32_t ppm;
  if (!rtc_time_valid() || !rtc_time_drift(&ppm)) {
    return false;
  }
  return rtc_drift_estimate(rtc_time_unix(), rtc_time_last_sync(), ppm, unix_time, age_s);
}

uint32_t rtc_time_last_sync(void)
{
  return rtc_time_valid() ? backup_read(BACKUP_LAST_SYNC) : 0U;
}
