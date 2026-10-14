#include "icm42605.h"

#include <stdint.h>

#include "board.h"
#include "sensor_bus.h"

#define REG_DEVICE_CONFIG  0x11
#define REG_INT_CONFIG     0x14
#define REG_INTF_CONFIG0   0x4C
#define REG_ACCEL_DATA_X1  0x1F
#define REG_INT_STATUS     0x2D
#define REG_PWR_MGMT0      0x4E
#define REG_GYRO_CONFIG0   0x4F
#define REG_INT_CONFIG1    0x64
#define REG_INT_SOURCE0    0x65
#define REG_ACCEL_CONFIG0  0x50
#define REG_WHO_AM_I       0x75
#define REG_BANK_SEL       0x76
#define REG_INTF_CONFIG4   0x7A /* bank 1: bit 1 = 4-wire SPI (SDO driven), reset value 0x03 */

#define WHO_AM_I_VALUE 0x42
#define DATA_RDY       0x08

static bool running;
static unsigned char who_am_i;

unsigned char icm42605_id(void)
{
  return who_am_i;
}

void icm42605_quiet(void)
{
  sensor_bus_init();
  sensor_bus_write(SENSOR_IMU, REG_BANK_SEL, 0);
  sensor_bus_write(SENSOR_IMU, REG_INTF_CONFIG0, 0x33); /* I2C off: SPI only */
}

bool icm42605_start(void)
{
  running = false;
  sensor_bus_init();
  /*
   * A chip that received stray traffic can sit in another register bank or in 3-wire SPI mode,
   * where it no longer drives SDO and every read gives 0xFF. Writes still reach it: back to bank
   * 0, a soft reset, and the 4-wire setting of bank 1, all blind, before the first probe. (The
   * 0xFF of 2026-10-09 was the sensor supply, SENS_PWR_PIN in board.h, not this.)
   */
  sensor_bus_write(SENSOR_IMU, REG_BANK_SEL, 0);
  sensor_bus_write(SENSOR_IMU, REG_DEVICE_CONFIG, 0x01);
  board_delay_ms(3);
  sensor_bus_write(SENSOR_IMU, REG_BANK_SEL, 1);
  sensor_bus_write(SENSOR_IMU, REG_INTF_CONFIG4, 0x03);
  sensor_bus_write(SENSOR_IMU, REG_BANK_SEL, 0);
  /* As with the barometer, the first transfer after power-up may be lost. */
  for (int i = 0; i < 3; i++) {
    sensor_bus_write(SENSOR_IMU, REG_BANK_SEL, 0);
    who_am_i = sensor_bus_read1(SENSOR_IMU, REG_WHO_AM_I);
    if (who_am_i == WHO_AM_I_VALUE) {
      break;
    }
    board_delay_ms(2);
  }
  if (who_am_i != WHO_AM_I_VALUE) {
    return false;
  }
  sensor_bus_write(SENSOR_IMU, REG_DEVICE_CONFIG, 0x01); /* soft reset */
  board_delay_ms(5);
  (void)sensor_bus_read1(SENSOR_IMU, REG_WHO_AM_I); /* in case the reset restored I2C */
  sensor_bus_write(SENSOR_IMU, REG_BANK_SEL, 0);
  sensor_bus_write(SENSOR_IMU, REG_INTF_CONFIG0, 0x33); /* I2C off (SPI only), data big endian, default counts */
  /* Data-ready flag as in the hardware check (polled in INT_STATUS; INT1 = PC12 unused). */
  sensor_bus_write(SENSOR_IMU, REG_INT_CONFIG, 0x02);  /* INT1 push-pull, active low, pulsed */
  sensor_bus_write(SENSOR_IMU, REG_INT_CONFIG1, 0x00); /* INT_ASYNC_RESET = 0 (required) */
  sensor_bus_write(SENSOR_IMU, REG_INT_SOURCE0, 0x08); /* data ready */
  sensor_bus_write(SENSOR_IMU, REG_GYRO_CONFIG0, (0U << 5) | 0x08);  /* +-2000 dps, 100 Hz */
  sensor_bus_write(SENSOR_IMU, REG_ACCEL_CONFIG0, (1U << 5) | 0x08); /* +-8 g, 100 Hz */
  running = sensor_bus_write(SENSOR_IMU, REG_PWR_MGMT0, 0x0F);       /* gyro + accel low-noise */
  board_delay_ms(50);                                                /* gyro start-up 45 ms */
  return running;
}

bool icm42605_read(int16_t raw[6])
{
  uint8_t d[12];
  if (!running || !(sensor_bus_read1(SENSOR_IMU, REG_INT_STATUS) & DATA_RDY) ||
      !sensor_bus_read(SENSOR_IMU, REG_ACCEL_DATA_X1, d, sizeof(d))) {
    return false;
  }
  for (int i = 0; i < 6; i++) {
    raw[i] = (int16_t)((d[2 * i] << 8) | d[2 * i + 1]);
  }
  return true;
}

void icm42605_sleep(void)
{
  sensor_bus_write(SENSOR_IMU, REG_BANK_SEL, 0);
  sensor_bus_write(SENSOR_IMU, REG_PWR_MGMT0, 0x00);
  running = false;
}
