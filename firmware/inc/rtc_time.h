#ifndef RTC_TIME_H
#define RTC_TIME_H

#include <stdbool.h>
#include <stdint.h>

/*
 * UTC time in the RTC (no 32 kHz crystal on the board). An RTC the stock firmware set up keeps its
 * clock source; otherwise it runs on LSI, also in standby, and drifts. Set from GPS; used to give the
 * GNSS module the time for a warm start - only when the RTC keeps running while the device is off
 * and the rate error measured at the last GPS sync is plausible (RTC_TRUST_PPM). The stock's HSE/32
 * stands still in standby: on the flights of 2026-10-12 both RTCs still showed the day they were
 * last switched off, and the module given such a time needed 377 s for its fix against 90 s
 * without a time (with the same signal strengths).
 */
#define RTC_TRUST_PPM 20000 /* 2 %: an internal RC oscillator; beyond it the RTC stopped or ran off */
#define RTC_DRIFT_MIN_SPAN_S 600U /* shorter spans give no usable rate (1 s resolution) */
void rtc_time_init(void);
bool rtc_time_valid(void);
uint32_t rtc_time_unix(void);
/* Sets the RTC and remembers the moment as last GPS synchronisation. */
void rtc_time_sync(uint32_t unix_time);
uint32_t rtc_time_last_sync(void); /* 0 = never */
/* Milliseconds (wrapping); for intervals that span a reset. */
uint32_t rtc_time_ms32(void);

/* Clock source as text ("LSI", "LSE", "HSE/32 (stops in standby)", "none") and the prescalers. */
const char *rtc_time_source_name(void);
/* The RTC keeps counting while the device is off (standby): LSE or LSI, not HSE/32. */
bool rtc_time_runs_while_off(void);
void rtc_time_prescalers(uint32_t *async, uint32_t *sync);
/* Rate error measured at the last GPS sync (ppm, RTC minus GPS); false if none was measured. */
bool rtc_time_drift(int32_t *ppm);
void rtc_time_set_drift(int32_t ppm);
/*
 * The current UTC time and the seconds since the last sync, corrected by the measured rate error;
 * false when the RTC cannot be trusted (never synced, no rate measured, rate beyond RTC_TRUST_PPM).
 */
bool rtc_time_estimate(uint32_t *unix_time, uint32_t *age_s);
/* The arithmetic of rtc_time_estimate, without hardware (unit-tested). */
bool rtc_drift_estimate(uint32_t rtc_now, uint32_t last_sync, int32_t ppm, uint32_t *unix_time, uint32_t *age_s);

uint32_t unix_from_civil(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t min, uint8_t sec);
void civil_from_unix(uint32_t t, uint16_t *year, uint8_t *month, uint8_t *day, uint8_t *hour, uint8_t *min,
                     uint8_t *sec);

#endif
