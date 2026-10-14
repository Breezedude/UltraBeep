#ifndef ICM42605_H
#define ICM42605_H

#include <stdbool.h>
#include <stdint.h>

/*
 * IMU ICM-42605 on the sensor bus: accelerometer +-8 g and gyro +-2000 dps,
 * both 100 Hz in low-noise mode. Polled: a sample is read once its
 * data-ready flag is set.
 */
#define ICM42605_RATE_HZ       100
#define ICM42605_ACCEL_LSB_G   4096.0f /* +-8 g */
#define ICM42605_GYRO_LSB_DPS  16.4f   /* +-2000 dps */

/* Reset and start. False if the chip does not answer. */
/* Disables the chip's I2C engine (it listens on the shared SCK/MOSI lines after power-up and can
 * take the barometer's SPI traffic for I2C commands). Call before any other device on the bus. */
void icm42605_quiet(void);
bool icm42605_start(void);
/* WHO_AM_I read by the last start (0x42 = ICM-42605), for diagnostics. */
unsigned char icm42605_id(void);
/* True when a new sample was read: raw[0..2] accel x/y/z, raw[3..5] gyro x/y/z. */
bool icm42605_read(int16_t raw[6]);
/* Sleep mode; also right before the MCU standby. */
void icm42605_sleep(void);

#endif
