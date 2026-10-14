#include "rtc_time.h"

/* Howard Hinnant's days_from_civil / civil_from_days. */
uint32_t unix_from_civil(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t min, uint8_t sec)
{
  int32_t y = (int32_t)year - (month <= 2 ? 1 : 0);
  int32_t era = (y >= 0 ? y : y - 399) / 400;
  uint32_t yoe = (uint32_t)(y - era * 400);
  uint32_t doy = (153U * (month + (month > 2 ? -3 : 9)) + 2U) / 5U + day - 1U;
  uint32_t doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
  int32_t days = era * 146097 + (int32_t)doe - 719468;
  return (uint32_t)days * 86400U + hour * 3600U + min * 60U + sec;
}

void civil_from_unix(uint32_t t, uint16_t *year, uint8_t *month, uint8_t *day, uint8_t *hour, uint8_t *min,
                     uint8_t *sec)
{
  uint32_t days = t / 86400U;
  uint32_t rem = t % 86400U;
  *hour = (uint8_t)(rem / 3600U);
  *min = (uint8_t)((rem % 3600U) / 60U);
  *sec = (uint8_t)(rem % 60U);
  int32_t z = (int32_t)days + 719468;
  int32_t era = z / 146097;
  uint32_t doe = (uint32_t)(z - era * 146097);
  uint32_t yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
  int32_t y = (int32_t)yoe + era * 400;
  uint32_t doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
  uint32_t mp = (5U * doy + 2U) / 153U;
  *day = (uint8_t)(doy - (153U * mp + 2U) / 5U + 1U);
  *month = (uint8_t)(mp < 10U ? mp + 3U : mp - 9U);
  *year = (uint16_t)(y + (*month <= 2 ? 1 : 0));
}

bool rtc_drift_estimate(uint32_t rtc_now, uint32_t last_sync, int32_t ppm, uint32_t *unix_time, uint32_t *age_s)
{
  if (ppm > RTC_TRUST_PPM || ppm < -RTC_TRUST_PPM || rtc_now < last_sync) {
    return false;
  }
  uint32_t age = rtc_now - last_sync;
  /* the RTC counted age seconds at (1 + ppm / 1e6) times the real rate */
  int64_t corr = (int64_t)age * ppm / 1000000;
  *unix_time = (uint32_t)((int64_t)rtc_now - corr);
  *age_s = (uint32_t)((int64_t)age - corr);
  return true;
}
