#ifndef DPS310_H
#define DPS310_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Barometer DPS310 on the sensor bus, three sampling modes (dps310_start):
 *   DPS310_MODE_100  100 Hz, 4x oversampling (8.4 ms per measurement). The MCU starts each
 *                    measurement on a 10 ms grid (command mode, as the stock firmware): exact
 *                    timing, values at most about 10 ms old.
 *   DPS310_MODE_64   64 Hz, 8x (14.8 ms), continuous on the chip's own clock (runs ~3 % slow).
 *   DPS310_MODE_32   32 Hz, 16x (27.6 ms), continuous (the mode until 0.20).
 * Datasheet noise per sample: 0.5 / 0.4 / 0.35 Pa. Per second that is about 20 % less noise at
 * 100 and 64 Hz than at 32 Hz: oversampling beyond 4x to 8x pays off less than more samples.
 * Temperature once per second (low-pass filtered before it enters the pressure compensation).
 */
typedef enum { DPS310_MODE_32 = 32, DPS310_MODE_64 = 64, DPS310_MODE_100 = 100 } dps310_mode_t;

#define DPS310_COEFS 9 /* c0, c1, c00, c10, c01, c11, c20, c21, c30 */

typedef struct {
  float pressure_pa;
  float temp_c;
  int32_t p_raw; /* raw counts, for the raw log */
  int32_t t_raw;
} dps310_sample_t;

/* Reset, coefficients, measurements in the given mode. False if the chip does not answer. */
bool dps310_start(dps310_mode_t mode);
/* Nominal pressure rate of the running mode [Hz]. */
unsigned dps310_rate_hz(void);
/* Scale factors of the running mode's oversampling, and those of a rate (raw log header). */
float dps310_kp(void);
float dps310_kt(void);
void dps310_scale_factors(unsigned rate_hz, float *kp, float *kt);
/* Product id read by the last start (0x10 = DPS310), for diagnostics. */
unsigned char dps310_id(void);
/* True when a new pressure sample was read. Call at least every 5 ms (the 100 Hz mode starts the next measurement here). */
bool dps310_read(dps310_sample_t *s);
/* Calibration coefficients of the last start (raw log header). */
void dps310_coefficients(int32_t out[DPS310_COEFS]);
/* Standby (no measurements); also right before the MCU standby. */
void dps310_sleep(void);

#endif
