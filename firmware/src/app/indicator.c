#include "indicator.h"

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"

#define TICK_MS      25U
#define BLINK_ON_MS  100U
#define BLINK_GAP    200U
#define COUNT_PAUSE  1000U
#define QUICK_ON_MS  50U  /* indicator_blinks: short flashes, easy to count */
#define QUICK_GAP_MS 150U
#define BREATHE_MAX  5U   /* peak brightness of the breathing, in 1/32 (the default is 2) */
#define HEARTBEAT_ON_MS     50U
#define HEARTBEAT_PERIOD_MS 4000U

typedef enum {
  MODE_OFF, MODE_SOLID, MODE_COUNT, MODE_PULSE, MODE_BLINKS, MODE_BREATHE, MODE_DIM, MODE_VARIO, MODE_POWER_ON
} ind_mode_t;

static volatile ind_mode_t mode;
static volatile led_color_t color;
static volatile uint8_t count;
static volatile uint16_t pulse_on;
static volatile uint16_t pulse_period;
static volatile uint8_t dim_duty;
static volatile led_color_t vario_color;
static uint32_t phase_ms;

static void set_mode(ind_mode_t m, led_color_t c)
{
  taskENTER_CRITICAL();
  mode = m;
  color = c;
  phase_ms = 0;
  taskEXIT_CRITICAL();
  if (m != MODE_BREATHE && m != MODE_DIM) {
    led_set_brightness(LED_BRIGHTNESS_DEFAULT);
  }
}

static led_color_t count_pattern(uint32_t t)
{
  if (count == 0) {
    return (t % 2000U) < 60U ? color : LED_OFF;
  }
  uint32_t blink_period = BLINK_ON_MS + BLINK_GAP;
  uint32_t cycle = count * blink_period + COUNT_PAUSE;
  uint32_t pos = t % cycle;
  if (pos >= count * blink_period) {
    return LED_OFF;
  }
  return (pos % blink_period) < BLINK_ON_MS ? color : LED_OFF;
}

/* count quick flashes at the start of every pulse_period. */
static led_color_t blinks_pattern(uint32_t t)
{
  uint32_t blink_period = QUICK_ON_MS + QUICK_GAP_MS;
  uint32_t pos = t % pulse_period;
  if (pos >= count * blink_period) {
    return LED_OFF;
  }
  return (pos % blink_period) < QUICK_ON_MS ? color : LED_OFF;
}

/* Triangle: 0 at the start and the end of the period, BREATHE_MAX in the middle. */
static uint8_t breathe_duty(uint32_t t)
{
  uint32_t half = pulse_period / 2U;
  uint32_t pos = t % pulse_period;
  uint32_t rise = pos < half ? pos : pulse_period - pos;
  return (uint8_t)((rise * BREATHE_MAX + half / 2U) / half);
}

static void tick(TimerHandle_t t)
{
  (void)t;
  phase_ms += TICK_MS;
  switch (mode) {
  case MODE_OFF:
    led_set(LED_OFF);
    break;
  case MODE_SOLID:
    led_set(color);
    break;
  case MODE_COUNT:
    led_set(count_pattern(phase_ms));
    break;
  case MODE_PULSE:
    led_set((phase_ms % pulse_period) < pulse_on ? color : LED_OFF);
    break;
  case MODE_BLINKS:
    led_set(blinks_pattern(phase_ms));
    break;
  case MODE_BREATHE:
    led_set_brightness(breathe_duty(phase_ms));
    led_set(color);
    break;
  case MODE_DIM:
    led_set_brightness(dim_duty);
    led_set(color);
    break;
  case MODE_VARIO:
    led_set(vario_color);
    break;
  case MODE_POWER_ON: {
    static const led_color_t seq[] = {LED_RED, LED_GREEN, LED_BLUE};
    uint32_t i = phase_ms / 150U;
    if (i < 3U) {
      led_set(seq[i]);
    } else {
      mode = MODE_OFF;
      led_set(LED_OFF);
    }
    break;
  }
  }
}

void indicator_init(void)
{
  TimerHandle_t t = xTimerCreate("led", pdMS_TO_TICKS(TICK_MS), pdTRUE, NULL, tick);
  xTimerStart(t, 0);
}

void indicator_off(void)
{
  if (mode == MODE_OFF) {
    return;
  }
  set_mode(MODE_OFF, LED_OFF);
}

void indicator_solid(led_color_t c)
{
  if (mode == MODE_SOLID && color == c) {
    return;
  }
  set_mode(MODE_SOLID, c);
}

void indicator_count(led_color_t c, uint8_t n)
{
  if (mode == MODE_COUNT && color == c && count == n) {
    return; /* keep the running cycle */
  }
  count = n;
  set_mode(MODE_COUNT, c);
}

void indicator_pulse(led_color_t c, uint16_t on_ms, uint16_t period_ms)
{
  if (mode == MODE_PULSE && color == c && pulse_on == on_ms && pulse_period == period_ms) {
    return; /* keep the running cycle */
  }
  pulse_on = on_ms;
  pulse_period = period_ms ? period_ms : 1U;
  set_mode(MODE_PULSE, c);
}

void indicator_heartbeat(led_color_t c)
{
  indicator_pulse(c, HEARTBEAT_ON_MS, HEARTBEAT_PERIOD_MS);
}

void indicator_blinks(led_color_t c, uint8_t n, uint16_t period_ms)
{
  if (mode == MODE_BLINKS && color == c && count == n && pulse_period == period_ms) {
    return;
  }
  count = n;
  pulse_period = period_ms ? period_ms : 1U;
  set_mode(MODE_BLINKS, c);
}

void indicator_breathe(led_color_t c, uint16_t period_ms)
{
  if (mode == MODE_BREATHE && color == c && pulse_period == period_ms) {
    return;
  }
  pulse_period = period_ms ? period_ms : 1U;
  set_mode(MODE_BREATHE, c);
}

void indicator_dim(led_color_t c, uint8_t duty_of_32)
{
  uint8_t d = duty_of_32 == 0U ? 1U : duty_of_32 > LED_BRIGHTNESS_DEFAULT ? LED_BRIGHTNESS_DEFAULT : duty_of_32;
  if (mode == MODE_DIM && color == c && dim_duty == d) {
    return;
  }
  dim_duty = d;
  set_mode(MODE_DIM, c);
}

void indicator_vario(void)
{
  if (mode == MODE_VARIO) {
    return;
  }
  set_mode(MODE_VARIO, LED_OFF);
}

void indicator_vario_feed(led_color_t c)
{
  vario_color = c;
}

void indicator_power_on(void)
{
  set_mode(MODE_POWER_ON, LED_OFF);
}
