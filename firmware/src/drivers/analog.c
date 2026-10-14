/*
 * Analog inputs, same scaling as the stock firmware:
 *   PA2 / ADC1_IN7  battery, divider enabled by PC4 low, VBAT = V(PA2) * 95 / 68
 *   PA4 / ADC1_IN9  solar cell, same scaling
 *   PA3 / ADC1_IN8  USB-C audio adapter detect, biased by PB14 high,
 *                   < 100 mV = adapter plugged
 * The stock firmware maps 3640 mV .. 4250 mV linearly to 0 .. 100 %.
 */
#include "analog.h"
#include "board.h"

static ADC_HandleTypeDef hadc1;
static bool adc_ready;

void HAL_ADC_MspInit(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance != ADC1) {
    return;
  }
  BOARD_ATOMIC(__HAL_RCC_ADC_CLK_ENABLE());
  BOARD_ATOMIC(__HAL_RCC_GPIOA_CLK_ENABLE());
  GPIO_InitTypeDef init = {0};
  init.Pin = GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4;
  init.Mode = GPIO_MODE_ANALOG;
  init.Pull = GPIO_NOPULL;
  board_gpio_init(GPIOA, &init);
}

bool analog_init(void)
{
  if (adc_ready) {
    return true;
  }
  BOARD_ATOMIC(__HAL_RCC_GPIOB_CLK_ENABLE());
  BOARD_ATOMIC(__HAL_RCC_GPIOC_CLK_ENABLE());
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
  hadc1.Init.OversamplingMode = DISABLE;
  if (HAL_ADC_Init(&hadc1) != HAL_OK) {
    return false;
  }
  adc_ready = HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) == HAL_OK;
  return adc_ready;
}

uint32_t analog_read_raw(uint32_t channel)
{
  if (!analog_init()) {
    return 0;
  }
  ADC_ChannelConfTypeDef conf = {0};
  conf.Channel = channel;
  conf.Rank = ADC_REGULAR_RANK_1;
  conf.SamplingTime = ADC_SAMPLETIME_640CYCLES_5;
  conf.SingleDiff = ADC_SINGLE_ENDED;
  conf.OffsetNumber = ADC_OFFSET_NONE;
  if (HAL_ADC_ConfigChannel(&hadc1, &conf) != HAL_OK) {
    return 0;
  }
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) {
    HAL_ADC_Start(&hadc1);
    if (HAL_ADC_PollForConversion(&hadc1, 10) == HAL_OK) {
      sum += HAL_ADC_GetValue(&hadc1);
    }
  }
  HAL_ADC_Stop(&hadc1);
  return sum / 16;
}

uint32_t analog_vdda_mv(void)
{
  uint32_t raw = analog_read_raw(ADC_CHANNEL_VREFINT);
  return raw ? __HAL_ADC_CALC_VREFANALOG_VOLTAGE(raw, ADC_RESOLUTION_12B) : 0;
}

uint32_t analog_pin_mv(uint32_t channel, uint32_t vdda_mv)
{
  return analog_read_raw(channel) * vdda_mv / 4095U;
}

int32_t analog_mcu_temp_c(uint32_t vdda_mv)
{
  uint32_t raw = analog_read_raw(ADC_CHANNEL_TEMPSENSOR);
  return __HAL_ADC_CALC_TEMPERATURE(vdda_mv, raw, ADC_RESOLUTION_12B);
}

/* state: 0 = high-Z (analog: no input buffer on the floating divider level), 1 = output low, 2 = output high */
void analog_set_pin(GPIO_TypeDef *port, uint16_t pin, int state)
{
  GPIO_InitTypeDef init = {0};
  init.Pin = pin;
  init.Pull = GPIO_NOPULL;
  init.Speed = GPIO_SPEED_FREQ_LOW;
  if (state == 0) {
    init.Mode = GPIO_MODE_ANALOG;
  } else {
    HAL_GPIO_WritePin(port, pin, state == 2 ? GPIO_PIN_SET : GPIO_PIN_RESET);
    init.Mode = GPIO_MODE_OUTPUT_PP;
  }
  board_gpio_init(port, &init);
}

bool analog_measure(analog_values_t *v)
{
  if (!analog_init()) {
    return false;
  }
  v->vdda_mv = analog_vdda_mv();
  analog_set_pin(GPIOC, GPIO_PIN_4, 1);  /* VBAT divider on */
  analog_set_pin(GPIOB, GPIO_PIN_14, 2); /* audio detect bias on */
  board_delay_ms(2);
  v->vbat_pin_mv = analog_pin_mv(ADC_CHANNEL_7, v->vdda_mv);
  v->audio_detect_mv = analog_pin_mv(ADC_CHANNEL_8, v->vdda_mv);
  v->solar_pin_mv = analog_pin_mv(ADC_CHANNEL_9, v->vdda_mv);
  analog_set_pin(GPIOC, GPIO_PIN_4, 0);
  analog_set_pin(GPIOB, GPIO_PIN_14, 0);
  v->vbat_mv = v->vbat_pin_mv * 95U / 68U;
  v->solar_mv = v->solar_pin_mv * 95U / 68U;
  v->battery_percent = analog_battery_percent(v->vbat_mv);
  return v->vdda_mv != 0;
}

uint32_t analog_audio_detect_mv(void)
{
  if (!analog_init()) {
    return UINT32_MAX;
  }
  uint32_t vdda = analog_vdda_mv();
  analog_set_pin(GPIOB, GPIO_PIN_14, 2); /* bias on */
  board_delay_ms(2);
  uint32_t mv = analog_pin_mv(ADC_CHANNEL_8, vdda);
  analog_set_pin(GPIOB, GPIO_PIN_14, 0);
  return vdda ? mv : UINT32_MAX;
}

/* Stock mapping: linear 3640 .. 4250 mV, rounded down to 10 % steps. */
unsigned analog_battery_percent(uint32_t vbat_mv)
{
  if (vbat_mv >= 4250U) {
    return 100;
  }
  if (vbat_mv <= 3640U) {
    return 0;
  }
  unsigned pct = (unsigned)((vbat_mv - 3640U) * 100U / (4250U - 3640U));
  return pct / 10U * 10U;
}
