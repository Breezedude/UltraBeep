#ifndef ANALOG_H
#define ANALOG_H

#include <stdbool.h>
#include <stdint.h>

#include "stm32wbxx_hal.h"

typedef struct {
  uint32_t vdda_mv;
  uint32_t vbat_pin_mv;
  uint32_t vbat_mv;
  uint32_t solar_pin_mv;
  uint32_t solar_mv;
  uint32_t audio_detect_mv; /* PA3, < 100 mV = USB-C audio adapter plugged */
  unsigned battery_percent;
} analog_values_t;

bool analog_init(void);
uint32_t analog_read_raw(uint32_t channel);
uint32_t analog_vdda_mv(void);
uint32_t analog_pin_mv(uint32_t channel, uint32_t vdda_mv);
int32_t analog_mcu_temp_c(uint32_t vdda_mv);
void analog_set_pin(GPIO_TypeDef *port, uint16_t pin, int state);
/* Battery, solar cell and audio adapter detect (switches the enables itself). */
bool analog_measure(analog_values_t *v);
unsigned analog_battery_percent(uint32_t vbat_mv);
/* PA3 with its bias (PB14) on: below ANALOG_AUDIO_ADAPTER_MV a USB-C audio adapter is plugged. */
#define ANALOG_AUDIO_ADAPTER_MV 100U
uint32_t analog_audio_detect_mv(void);

#endif
