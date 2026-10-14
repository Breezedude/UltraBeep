#ifndef SENSOR_BUS_H
#define SENSOR_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * SPI1 sensor bus (PA5 SCK, PB4 MISO, PB5 MOSI, mode 3, 8 MHz as in the
 * stock firmware): barometer DPS310 (CS PC10) and IMU ICM-42605 (CS PC11).
 * Both sensors have no power switch; they stay powered in standby and must
 * be put to sleep over the bus. No RTOS calls: usable before the scheduler.
 */
typedef enum { SENSOR_BARO, SENSOR_IMU } sensor_dev_t;

void sensor_bus_init(void);
/* SPI1 off, its pins and both chip selects released: nothing feeds the unpowered sensors. */
void sensor_bus_release(void);
/* Register access; false if the bus did not respond in time. */
bool sensor_bus_read(sensor_dev_t dev, uint8_t reg, uint8_t *buf, size_t n);
bool sensor_bus_write(sensor_dev_t dev, uint8_t reg, uint8_t value);
uint8_t sensor_bus_read1(sensor_dev_t dev, uint8_t reg);

#endif
