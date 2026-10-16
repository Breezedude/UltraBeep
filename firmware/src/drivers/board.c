/*
 * Board support: GPIO set-up, LED dimming, button / USB / charger inputs,
 * load switches, watchdog and the HAL time base.
 *
 * HAL time base: TIM17 increments the HAL tick. While an LED is lit it runs at
 * 20 kHz for the LED software PWM (tick every 20th interrupt), otherwise at
 * 1 kHz: 19000 fewer interrupts per second most of the time. The rate changes
 * only at a millisecond boundary, so the tick stays exact. It runs at the
 * highest priority so that HAL timeouts also work inside the USB interrupt
 * (mass storage reads the SD card there). FreeRTOS uses SysTick on its own.
 */
#include "board.h"

#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#define LED_PWM_STEPS 32U
#define TIM17_RATE_HZ 20000U

static IWDG_HandleTypeDef hiwdg;
static bool watchdog_running;
static volatile uint8_t led_color_target;
static volatile uint8_t led_duty = LED_BRIGHTNESS_DEFAULT; /* ~6 % */
static uint16_t arr_fast;             /* TIM17 reload for 20 kHz */
static uint16_t arr_slow;             /* TIM17 reload for 1 kHz */
static bool fast;                     /* TIM17 runs at 20 kHz */

void board_gpio_init(GPIO_TypeDef *port, const GPIO_InitTypeDef *init)
{
  uint32_t pm = board_irq_lock();
  HAL_GPIO_Init(port, (GPIO_InitTypeDef *)init);
  board_irq_unlock(pm);
}

void board_gpio_deinit(GPIO_TypeDef *port, uint32_t pins)
{
  uint32_t pm = board_irq_lock();
  HAL_GPIO_DeInit(port, pins);
  board_irq_unlock(pm);
}

static void gpio_output(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState level)
{
  HAL_GPIO_WritePin(port, pin, level);
  GPIO_InitTypeDef init = {0};
  init.Pin = pin;
  init.Mode = GPIO_MODE_OUTPUT_PP;
  init.Pull = GPIO_NOPULL;
  init.Speed = GPIO_SPEED_FREQ_LOW;
  board_gpio_init(port, &init);
}

static void gpio_input(GPIO_TypeDef *port, uint16_t pin, uint32_t pull)
{
  GPIO_InitTypeDef init = {0};
  init.Pin = pin;
  init.Mode = GPIO_MODE_INPUT;
  init.Pull = pull;
  board_gpio_init(port, &init);
}

static void led_pins(uint8_t color)
{
  uint32_t set = 0;
  uint32_t reset = 0;
  if (color & LED_RED) {
    set |= LED_R_PIN;
  } else {
    reset |= LED_R_PIN;
  }
  if (color & LED_GREEN) {
    set |= LED_G_PIN;
  } else {
    reset |= LED_G_PIN;
  }
  if (color & LED_BLUE) {
    reset |= LED_B_PIN; /* active low */
  } else {
    set |= LED_B_PIN;
  }
  GPIOB->BSRR = set | (reset << 16); /* all three LEDs are on port B */
}

/* ---- HAL time base on TIM17 ---- */

HAL_StatusTypeDef HAL_InitTick(uint32_t TickPriority)
{
  BOARD_ATOMIC(__HAL_RCC_TIM17_CLK_ENABLE());
  uint32_t clk = HAL_RCC_GetPCLK2Freq(); /* APB2 prescaler is 1: timer clock = PCLK2 */
  arr_fast = (uint16_t)(clk / TIM17_RATE_HZ - 1U);
  arr_slow = (uint16_t)(clk / 1000U - 1U); /* 63999 at 64 MHz: fits the 16-bit timer */
  fast = false;
  TIM17->CR1 = 0;
  TIM17->PSC = 0;
  TIM17->ARR = arr_slow;
  TIM17->EGR = TIM_EGR_UG;
  TIM17->SR = 0;
  TIM17->DIER = TIM_DIER_UIE;
  TIM17->CR1 = TIM_CR1_CEN;
  if (TickPriority < (1UL << __NVIC_PRIO_BITS)) {
    HAL_NVIC_SetPriority(TIM1_TRG_COM_TIM17_IRQn, TickPriority, 0);
    uwTickPrio = TickPriority;
  }
  HAL_NVIC_EnableIRQ(TIM1_TRG_COM_TIM17_IRQn);
  return HAL_OK;
}

void HAL_SuspendTick(void)
{
  TIM17->DIER &= ~TIM_DIER_UIE;
}

void HAL_ResumeTick(void)
{
  TIM17->DIER |= TIM_DIER_UIE;
}

void TIM1_TRG_COM_TIM17_IRQHandler(void)
{
  static uint8_t phase;
  static uint8_t sub;
  TIM17->SR = ~TIM_SR_UIF;
  if (fast) {
    phase = (uint8_t)((phase + 1U) % LED_PWM_STEPS);
    led_pins(phase < led_duty ? led_color_target : LED_OFF);
    if (++sub < TIM17_RATE_HZ / 1000U) {
      return;
    }
    sub = 0;
  }
  HAL_IncTick();
  /* Millisecond boundary: 20 kHz only while an LED is lit. The counter has just
   * restarted, so the new reload value applies to the period now running. */
  bool want = led_color_target != LED_OFF && led_duty > 0U;
  if (want != fast) {
    fast = want;
    TIM17->ARR = want ? arr_fast : arr_slow;
    if (TIM17->CNT > TIM17->ARR) {
      TIM17->CNT = 0; /* only after an unusually long interrupt delay */
    }
    if (!want) {
      led_pins(LED_OFF);
    }
  }
}

/* ---- init ---- */

void board_serial_number(char *out, size_t size)
{
  const uint32_t *uid = (const uint32_t *)UID64_BASE;
  snprintf(out, size, "%08lX%08lX", (unsigned long)uid[1], (unsigned long)uid[0]);
}

void board_init(void)
{
  BOARD_ATOMIC(__HAL_RCC_GPIOA_CLK_ENABLE());
  BOARD_ATOMIC(__HAL_RCC_GPIOB_CLK_ENABLE());
  BOARD_ATOMIC(__HAL_RCC_GPIOC_CLK_ENABLE());
  BOARD_ATOMIC(__HAL_RCC_GPIOD_CLK_ENABLE());
  BOARD_ATOMIC(__HAL_RCC_GPIOE_CLK_ENABLE());
  BOARD_ATOMIC(__HAL_RCC_GPIOH_CLK_ENABLE());

  /* Loads off: SD card (active low), GNSS (active low, see board_gps_power), speaker amplifier. */
  gpio_output(SD_PWR_PORT, SD_PWR_PIN, GPIO_PIN_SET);
  gpio_output(SD_CS_PORT, SD_CS_PIN, GPIO_PIN_RESET); /* high would feed the unpowered card */
  board_gps_power(false);
  /* Sensor supply off, then on: the chips start from their reset state at every boot, as under the
   * stock firmware (which switches them off before standby). The IMU otherwise stays powered
   * through a soft restart and keeps whatever state it was left in. */
  gpio_output(SENS_PWR_PORT, SENS_PWR_PIN, GPIO_PIN_RESET);
  board_delay_ms(100);
  HAL_GPIO_WritePin(SENS_PWR_PORT, SENS_PWR_PIN, GPIO_PIN_SET);
  board_delay_ms(10);
  gpio_output(AMP_PORT, AMP_PIN, GPIO_PIN_RESET);
  /* PH3 released: speaker and USB data path as in the stock firmware (board_audio_headphones). */
  gpio_input(AUDIO_SEL_PORT, AUDIO_SEL_PIN, GPIO_NOPULL);
  /* Mezzanine detect: released, as the stock leaves it after its one reading. */
  GPIO_InitTypeDef mezz = {0};
  mezz.Pin = MEZZ_PIN;
  mezz.Mode = GPIO_MODE_ANALOG;
  board_gpio_init(MEZZ_PORT, &mezz);

  gpio_output(LED_R_PORT, LED_R_PIN, GPIO_PIN_RESET);
  gpio_output(LED_G_PORT, LED_G_PIN, GPIO_PIN_RESET);
  gpio_output(LED_B_PORT, LED_B_PIN, GPIO_PIN_SET); /* active low */

  gpio_input(BTN_PORT, BTN_PIN, GPIO_PULLUP);
  gpio_input(VBUS_PORT, VBUS_PIN, GPIO_PULLDOWN);
  gpio_input(CHG_PORT, CHG_PIN, GPIO_PULLUP);
  gpio_input(GPIOB, GPIO_PIN_13, GPIO_PULLDOWN);

  /* The standby pull configuration (power.c) stays active after the wake-up until released here. */
  HAL_PWREx_DisablePullUpPullDownConfig();
}

/* ---- LEDs ---- */

void led_set_brightness(uint8_t duty_of_32)
{
  led_duty = duty_of_32 > LED_PWM_STEPS ? LED_PWM_STEPS : duty_of_32;
}

void led_force(led_color_t color)
{
  led_pins((uint8_t)color);
}

void led_set(led_color_t color)
{
  led_color_target = (uint8_t)color;
}

/* ---- inputs and load switches ---- */

bool button_pressed(void)
{
  return HAL_GPIO_ReadPin(BTN_PORT, BTN_PIN) == GPIO_PIN_RESET;
}

bool vbus_present(void)
{
  return HAL_GPIO_ReadPin(VBUS_PORT, VBUS_PIN) == GPIO_PIN_SET;
}

bool charger_line_low(void)
{
  return HAL_GPIO_ReadPin(CHG_PORT, CHG_PIN) == GPIO_PIN_RESET;
}

/*
 * On: PC6 low. Off: PC6 high-Z like the stock firmware, the external pull-up
 * switches the module off. Driven high it would sit at 3.3 V, and with a full
 * battery the switch would conduct partly.
 */
void board_sensor_power(bool on)
{
  HAL_GPIO_WritePin(SENS_PWR_PORT, SENS_PWR_PIN, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
  board_delay_ms(10);
}

void board_audio_headphones(bool on)
{
  gpio_output(AUDIO_SEL_PORT, AUDIO_SEL_PIN, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void board_gps_power(bool on)
{
  if (on) {
    gpio_output(GPS_PWR_PORT, GPS_PWR_PIN, GPIO_PIN_RESET);
  } else {
    GPIO_InitTypeDef init = {0};
    init.Pin = GPS_PWR_PIN;
    init.Mode = GPIO_MODE_ANALOG;
    init.Pull = GPIO_NOPULL;
    board_gpio_init(GPS_PWR_PORT, &init);
  }
}

void board_amp_enable(bool on)
{
  HAL_GPIO_WritePin(AMP_PORT, AMP_PIN, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* ---- timing and watchdog ---- */

void board_delay_ms(uint32_t ms)
{
  if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
    vTaskDelay(pdMS_TO_TICKS(ms));
    return;
  }
  uint32_t start = HAL_GetTick();
  while ((HAL_GetTick() - start) < ms) {
    watchdog_feed();
  }
}

void watchdog_start(void)
{
  /* LSI ~32 kHz / 64 * 4095 -> ~8 s. Frozen in standby (option byte IWDG_STDBY = 0). */
  hiwdg.Instance = IWDG;
  hiwdg.Init.Prescaler = IWDG_PRESCALER_64;
  hiwdg.Init.Window = IWDG_WINDOW_DISABLE;
  hiwdg.Init.Reload = 4095;
  watchdog_running = HAL_IWDG_Init(&hiwdg) == HAL_OK;
}

void watchdog_feed(void)
{
  if (watchdog_running) {
    HAL_IWDG_Refresh(&hiwdg);
  }
}

/* ---- main stack ---- */

/*
 * The main stack (MSP) below the top of RAM: main() before the scheduler, every interrupt afterwards
 * (the USB mass storage path is the deepest). Its reservation is _Min_Stack_Size in the linker script;
 * newlib's heap ends there. Painted with a pattern from its bottom up to just below what is in use
 * at start; the console's `tasks` reports what never got overwritten.
 */
#define STACK_PATTERN 0xA5A5A5A5U
extern uint32_t _estack;
extern uint32_t _Min_Stack_Size;

static uint32_t *stack_bottom(void)
{
  return (uint32_t *)((uint32_t)&_estack - (uint32_t)&_Min_Stack_Size);
}

void board_isr_stack_paint(void)
{
  uint32_t *sp = (uint32_t *)(__get_MSP() - 64U); /* below the frames in use now */
  for (uint32_t *p = stack_bottom(); p < sp; p++) {
    *p = STACK_PATTERN;
  }
}

uint32_t board_isr_stack_reserve(void)
{
  const uint32_t *p = stack_bottom();
  while (p < &_estack && *p == STACK_PATTERN) {
    p++;
  }
  return (uint32_t)((const uint8_t *)p - (const uint8_t *)stack_bottom());
}
