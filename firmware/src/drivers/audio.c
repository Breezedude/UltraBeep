/*
 * Audio hardware, driven the same way as by the stock firmware:
 *
 * Piezo: H-bridge driven by TIM2 (centre-aligned, f = 64 MHz / (2 * ARR)):
 *   left leg  PA0 (CH1, high side, active low) + PB11 (CH4, low side)
 *   right leg PA1 (CH2, high side, active low) + PB10 (CH3, low side)
 *   A high side is on for 2p ticks per period (pulse); full pulse: p = 0.45 ARR.
 *   Classic drive (stock): CCR1 = CCR3 = p, CCR2 = CCR4 = ARR - p, p <= ARR / 2
 *   (never both transistors of one leg on). Between the pulses all four are off
 *   and the piezo, a capacitor, keeps its voltage: it sees the full square wave
 *   once a pulse is longer than a few us. A shorter pulse charges it only
 *   partly, which is quieter; otherwise only the charge pump changes the
 *   loudness.
 *   Pulse width drive (experimental): both low sides short the piezo between the
 *   pulses (+V, 0, -V, 0). Far below its resonance the piezo sounds mainly from
 *   the edges, so this is not louder or more continuous either (test 0.11).
 * Charge pump (bridge supply): TIM1 complementary PWM with 27-tick dead time,
 *   centre-aligned, 50 %. Leg A = PA8 (CH1) / PA7 (CH1N), leg B = PA10 (CH3) /
 *   PB15 (CH3N). Mode 1 = leg B only, 2 = leg A only, 3 = both, 0 = off.
 *   Stock volume table (piezo): {500 kHz, mode 2}, {10 kHz, off},
 *   {500 kHz, mode 2}, {7 kHz, mode 3}, {500 kHz, mode 3}.
 * Voice: TIM16 PWM at 88.2 kHz (22.05 kHz x 4), DMA (TIM16_CH1 request) into
 *   CCR1; speaker path PB8 (CH1) + PB6 (CH1N) differential, alternative
 *   PA6 (CH1) presumably towards the headphone switch.
 */
#include <math.h>
#include <string.h>

#include "audio.h"

#include "FreeRTOS.h"
#include "task.h"

#include "board.h"
#include "ff.h"
#include "qoa.h"
#include "storage.h"

#define TIM_CLK_HZ 64000000U

static TIM_HandleTypeDef htim1;
static TIM_HandleTypeDef htim2;
static TIM_HandleTypeDef htim16;
static DMA_HandleTypeDef hdma_tim16;

/* ---------------------------------------------------------------- helpers */

static void pin_out(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState level)
{
  HAL_GPIO_WritePin(port, pin, level);
  GPIO_InitTypeDef init = {0};
  init.Pin = pin;
  init.Mode = GPIO_MODE_OUTPUT_PP;
  init.Pull = GPIO_NOPULL;
  init.Speed = GPIO_SPEED_FREQ_LOW;
  board_gpio_init(port, &init);
}

static void pin_af(GPIO_TypeDef *port, uint16_t pin, uint8_t af)
{
  GPIO_InitTypeDef init = {0};
  init.Pin = pin;
  init.Mode = GPIO_MODE_AF_PP;
  init.Pull = GPIO_NOPULL;
  init.Speed = GPIO_SPEED_FREQ_LOW;
  init.Alternate = af;
  board_gpio_init(port, &init);
}

static void pin_analog(GPIO_TypeDef *port, uint16_t pin)
{
  GPIO_InitTypeDef init = {0};
  init.Pin = pin;
  init.Mode = GPIO_MODE_ANALOG;
  init.Pull = GPIO_NOPULL;
  board_gpio_init(port, &init);
}

void audio_init(void)
{
  BOARD_ATOMIC(__HAL_RCC_GPIOA_CLK_ENABLE());
  BOARD_ATOMIC(__HAL_RCC_GPIOB_CLK_ENABLE());
  BOARD_ATOMIC(__HAL_RCC_GPIOH_CLK_ENABLE());
  piezo_stop();
  charge_pump_stop();
  voice_stop();
}

/* ------------------------------------------------------------ charge pump */

void charge_pump_stop(void)
{
  if (htim1.Instance) {
    HAL_TIM_PWM_DeInit(&htim1);
    htim1.Instance = NULL;
  }
  /* Stock idle state: high sides (PA10, PA7) high, low sides (PB15, PA8) low. */
  pin_out(GPIOA, GPIO_PIN_10, GPIO_PIN_SET);
  pin_out(GPIOB, GPIO_PIN_15, GPIO_PIN_RESET);
  pin_out(GPIOA, GPIO_PIN_7, GPIO_PIN_SET);
  pin_out(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);
}

bool charge_pump_start(uint32_t freq_hz, int mode)
{
  charge_pump_stop();
  if (mode == 0 || freq_hz == 0) {
    return true;
  }
  BOARD_ATOMIC(__HAL_RCC_TIM1_CLK_ENABLE());
  htim1.Instance = TIM1;
  htim1.Init.Prescaler = 0;
  htim1.Init.CounterMode = TIM_COUNTERMODE_CENTERALIGNED2;
  htim1.Init.Period = TIM_CLK_HZ / (2U * freq_hz);
  htim1.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim1.Init.RepetitionCounter = 0;
  htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  if (HAL_TIM_PWM_Init(&htim1) != HAL_OK) {
    return false;
  }
  TIM_BreakDeadTimeConfigTypeDef bdt = {0};
  bdt.OffStateRunMode = TIM_OSSR_DISABLE;
  bdt.OffStateIDLEMode = TIM_OSSI_DISABLE;
  bdt.LockLevel = TIM_LOCKLEVEL_OFF;
  bdt.DeadTime = 27;
  bdt.BreakState = TIM_BREAK_DISABLE;
  bdt.BreakPolarity = TIM_BREAKPOLARITY_HIGH;
  bdt.AutomaticOutput = TIM_AUTOMATICOUTPUT_DISABLE;
  if (HAL_TIMEx_ConfigBreakDeadTime(&htim1, &bdt) != HAL_OK) {
    return false;
  }
  TIM_OC_InitTypeDef oc = {0};
  oc.OCMode = TIM_OCMODE_PWM1;
  oc.Pulse = 0;
  oc.OCPolarity = TIM_OCPOLARITY_HIGH;
  oc.OCNPolarity = TIM_OCNPOLARITY_LOW;
  if (HAL_TIM_PWM_ConfigChannel(&htim1, &oc, TIM_CHANNEL_1) != HAL_OK) {
    return false;
  }
  oc.OCPolarity = TIM_OCPOLARITY_LOW;
  oc.OCNPolarity = TIM_OCNPOLARITY_HIGH;
  if (HAL_TIM_PWM_ConfigChannel(&htim1, &oc, TIM_CHANNEL_3) != HAL_OK) {
    return false;
  }

  /* Pins: switching legs on the timer, the other leg static like stock. */
  if (mode & 1) {
    pin_af(GPIOA, GPIO_PIN_10, GPIO_AF1_TIM1);
    pin_af(GPIOB, GPIO_PIN_15, GPIO_AF1_TIM1);
  }
  if (mode & 2) {
    pin_af(GPIOA, GPIO_PIN_7, GPIO_AF1_TIM1);
    pin_af(GPIOA, GPIO_PIN_8, GPIO_AF1_TIM1);
  }
  if (mode == 1) {
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_SET);
  } else if (mode == 2) {
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_15, GPIO_PIN_SET);
  }

  TIM_TypeDef *t = TIM1;
  t->CCR1 = htim1.Init.Period / 2;
  t->CCR3 = htim1.Init.Period / 2;
  t->CCER = 0x070DU; /* CC1E, CC1NE, CC1NP, CC3E, CC3P, CC3NE (stock value) */
  t->CNT = htim1.Init.Period / 2;
  t->BDTR |= TIM_BDTR_MOE;
  t->CR1 |= TIM_CR1_CEN;
  return true;
}

/* ------------------------------------------------------------------ piezo */

void piezo_stop(void)
{
  if (htim2.Instance) {
    HAL_TIM_PWM_DeInit(&htim2);
    htim2.Instance = NULL;
  }
  /* Stock idle state: high sides (PA0, PA1) high = off, low sides low = off. */
  pin_out(GPIOA, GPIO_PIN_1, GPIO_PIN_SET);
  pin_out(GPIOA, GPIO_PIN_0, GPIO_PIN_SET);
  pin_out(GPIOB, GPIO_PIN_10, GPIO_PIN_RESET);
  pin_out(GPIOB, GPIO_PIN_11, GPIO_PIN_RESET);
  pin_analog(GPIOA, GPIO_PIN_15);
}

static bool tim2_setup(uint32_t freq_hz)
{
  BOARD_ATOMIC(__HAL_RCC_TIM2_CLK_ENABLE());
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 0;
  htim2.Init.CounterMode = TIM_COUNTERMODE_CENTERALIGNED1;
  htim2.Init.Period = TIM_CLK_HZ / (2U * freq_hz);
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  /* Preloaded: a smaller ARR written while the counter is above it would let
   * the 32-bit counter run on to its overflow (up to 67 s without a tone). */
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  if (HAL_TIM_PWM_Init(&htim2) != HAL_OK) {
    return false;
  }
  TIM_OC_InitTypeDef oc = {0};
  oc.OCMode = TIM_OCMODE_PWM1;
  oc.OCPolarity = TIM_OCPOLARITY_HIGH;
  HAL_TIM_PWM_ConfigChannel(&htim2, &oc, TIM_CHANNEL_2);
  HAL_TIM_PWM_ConfigChannel(&htim2, &oc, TIM_CHANNEL_3);
  oc.OCPolarity = TIM_OCPOLARITY_LOW;
  HAL_TIM_PWM_ConfigChannel(&htim2, &oc, TIM_CHANNEL_4);
  HAL_TIM_PWM_ConfigChannel(&htim2, &oc, TIM_CHANNEL_1);
  return true;
}

/*
 * Time between switching off one transistor of a leg and switching on the other
 * one, in the pulse width drive: 20 us. The classic drive leaves at least
 * ARR - 2p = 0.1 * ARR (amplitude <= 0.9), that is 19 us at the highest melody
 * note (2640 Hz), and has run like that without trouble.
 */
#define PIEZO_DEAD_TICKS 1280U

static volatile piezo_drive_t drive = PIEZO_DRIVE_CLASSIC;

void piezo_set_drive(piezo_drive_t d)
{
  drive = d;
}

piezo_drive_t piezo_get_drive(void)
{
  return drive;
}

/*
 * Loads the period and the compare values together: with the update event
 * disabled meanwhile, no period can run with a mix of old and new values (a new
 * CCR1 with an old CCR4 could switch on both transistors of a leg).
 * Left leg: high side on while CNT < CCR1, low side on while CNT >= CCR4.
 * Right leg: high side on while CNT >= CCR2, low side on while CNT < CCR3.
 */
static void tim2_load(uint32_t arr, uint32_t pulse_ns)
{
  uint32_t half = arr / 2U;
  bool pwm = drive == PIEZO_DRIVE_PWM;
  uint32_t max = pwm ? (half > PIEZO_DEAD_TICKS ? half - PIEZO_DEAD_TICKS : 0U) : half;
  uint32_t full = arr * 9U / 20U;
  uint32_t p = pulse_ns == PIEZO_PULSE_FULL ? full : pulse_ns * (TIM_CLK_HZ / 1000000U) / 2000U;
  if (p > full) {
    p = full;
  }
  if (p > max) {
    p = max;
  }
  TIM2->CR1 |= TIM_CR1_UDIS;
  TIM2->ARR = arr;
  TIM2->CCR1 = p;
  TIM2->CCR2 = arr - p;
  if (pwm) {
    TIM2->CCR3 = arr - p - PIEZO_DEAD_TICKS; /* both low sides on in between: piezo shorted */
    TIM2->CCR4 = p + PIEZO_DEAD_TICKS;
  } else {
    TIM2->CCR3 = p; /* all off in between: piezo floating */
    TIM2->CCR4 = arr - p;
  }
  TIM2->CR1 &= ~TIM_CR1_UDIS;
}

bool piezo_start(uint32_t freq_hz, uint32_t pulse_ns, piezo_output_t output)
{
  if (freq_hz < 10 || freq_hz > 5000) {
    return false;
  }
  piezo_stop();
  if (!tim2_setup(freq_hz)) {
    return false;
  }
  tim2_load(TIM2->ARR, pulse_ns);
  if (output == PIEZO_OUT_BRIDGE) {
    pin_af(GPIOA, GPIO_PIN_0, GPIO_AF1_TIM2);
    pin_af(GPIOA, GPIO_PIN_1, GPIO_AF1_TIM2);
    pin_af(GPIOB, GPIO_PIN_10, GPIO_AF1_TIM2);
    pin_af(GPIOB, GPIO_PIN_11, GPIO_AF1_TIM2);
  } else {
    pin_af(GPIOA, GPIO_PIN_15, GPIO_AF1_TIM2);
  }
  TIM2->CCER = 0x3113U; /* CC1E|CC1P, CC2E, CC3E, CC4E|CC4P (stock value) */
  TIM2->CNT = TIM2->ARR / 2;
  TIM2->CR1 |= TIM_CR1_CEN;
  return true;
}

void piezo_set_frequency(uint32_t freq_hz, uint32_t pulse_ns)
{
  if (!htim2.Instance || freq_hz < 10 || freq_hz > 5000) { /* <= 5 kHz: half period > 2 dead times */
    return;
  }
  tim2_load(TIM_CLK_HZ / (2U * freq_hz), pulse_ns);
}

/* ------------------------------------------------------------------ voice */

#define VOICE_RATE      22050U
#define VOICE_OVERSAMPLE 4U
#define VOICE_ARR       ((TIM_CLK_HZ / (VOICE_RATE * VOICE_OVERSAMPLE)) - 1U) /* 724 -> 88.27 kHz */
#define BLOCK_IN        256U                                                /* input samples per half */
#define BLOCK_OUT       (BLOCK_IN * VOICE_OVERSAMPLE)

static uint16_t pwm_buf[2 * BLOCK_OUT];
static volatile uint32_t half_ready_mask; /* bit0: first half free, bit1: second half free */
static TaskHandle_t voice_task;           /* woken by the DMA half/complete interrupts */
static volatile bool voice_abort_flag;
static int16_t last_sample;
static float voice_gain = 0.5f;
static bool voice_running;
static unsigned voice_late;    /* blocks filled after the DMA had played both halves (stale audio) */
static unsigned voice_starved; /* silence blocks fed because the clip data was not read yet */
static bool voice_last_audible; /* the block fed before was not silence (reset per session) */

void DMA1_Channel5_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&hdma_tim16);
}

static void wake_voice_task(void)
{
  if (voice_task) {
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(voice_task, &woken);
    portYIELD_FROM_ISR(woken);
  }
}

void HAL_TIM_PWM_PulseFinishedHalfCpltCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM16) {
    half_ready_mask |= 1U;
    wake_voice_task();
  }
}

void HAL_TIM_PWM_PulseFinishedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM16) {
    half_ready_mask |= 2U;
    wake_voice_task();
  }
}

static uint16_t to_ccr(int32_t s)
{
  int32_t mid = (int32_t)(VOICE_ARR + 1) / 2;
  int32_t v = mid + (int32_t)((float)s * voice_gain * (float)mid / 32768.0f);
  if (v < 0) {
    v = 0;
  } else if (v > (int32_t)VOICE_ARR) {
    v = VOICE_ARR;
  }
  return (uint16_t)v;
}

/* Linear 4x interpolation like the stock firmware. */
static void fill_block(uint16_t *out, const int16_t *in, uint32_t n)
{
  for (uint32_t i = 0; i < BLOCK_IN; i++) {
    int32_t s = i < n ? in[i] : 0;
    for (uint32_t k = 1; k <= VOICE_OVERSAMPLE; k++) {
      int32_t v = last_sample + ((s - last_sample) * (int32_t)k) / (int32_t)VOICE_OVERSAMPLE;
      *out++ = to_ccr(v);
    }
    last_sample = (int16_t)s;
  }
}

void voice_stop(void)
{
  if (voice_running) {
    HAL_TIM_PWM_Stop_DMA(&htim16, TIM_CHANNEL_1);
    HAL_TIMEx_PWMN_Stop(&htim16, TIM_CHANNEL_1);
    HAL_TIM_PWM_DeInit(&htim16);
    HAL_DMA_DeInit(&hdma_tim16);
    HAL_NVIC_DisableIRQ(DMA1_Channel5_IRQn);
    voice_running = false;
  }
  /* High-Z towards the amplifier input filter; analog: no input buffer on a floating level. */
  pin_analog(GPIOB, GPIO_PIN_8);
  pin_analog(GPIOB, GPIO_PIN_6);
  pin_analog(GPIOA, GPIO_PIN_6);
}

bool voice_start(voice_output_t output, float gain)
{
  voice_stop();
  voice_gain = gain;
  last_sample = 0;
  voice_late = 0;
  voice_starved = 0;
  voice_last_audible = false; /* a late first block only delays the start: nothing audible repeats */
  for (uint32_t i = 0; i < 2 * BLOCK_OUT; i++) {
    pwm_buf[i] = (uint16_t)((VOICE_ARR + 1) / 2);
  }

  BOARD_ATOMIC(__HAL_RCC_TIM16_CLK_ENABLE());
  BOARD_ATOMIC(__HAL_RCC_DMAMUX1_CLK_ENABLE());
  BOARD_ATOMIC(__HAL_RCC_DMA1_CLK_ENABLE());

  hdma_tim16.Instance = DMA1_Channel5;
  hdma_tim16.Init.Request = DMA_REQUEST_TIM16_CH1;
  hdma_tim16.Init.Direction = DMA_MEMORY_TO_PERIPH;
  hdma_tim16.Init.PeriphInc = DMA_PINC_DISABLE;
  hdma_tim16.Init.MemInc = DMA_MINC_ENABLE;
  hdma_tim16.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
  hdma_tim16.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
  hdma_tim16.Init.Mode = DMA_CIRCULAR;
  hdma_tim16.Init.Priority = DMA_PRIORITY_HIGH;
  if (HAL_DMA_Init(&hdma_tim16) != HAL_OK) {
    return false;
  }
  HAL_NVIC_SetPriority(DMA1_Channel5_IRQn, 5, 0); /* calls FreeRTOS: >= configMAX_SYSCALL */
  HAL_NVIC_EnableIRQ(DMA1_Channel5_IRQn);

  htim16.Instance = TIM16;
  htim16.Init.Prescaler = 0;
  htim16.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim16.Init.Period = VOICE_ARR;
  htim16.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim16.Init.RepetitionCounter = 0;
  htim16.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  if (HAL_TIM_PWM_Init(&htim16) != HAL_OK) {
    return false;
  }
  __HAL_LINKDMA(&htim16, hdma[TIM_DMA_ID_CC1], hdma_tim16);

  TIM_OC_InitTypeDef oc = {0};
  oc.OCMode = TIM_OCMODE_PWM1;
  oc.Pulse = (VOICE_ARR + 1) / 2;
  oc.OCPolarity = TIM_OCPOLARITY_HIGH;
  oc.OCNPolarity = TIM_OCNPOLARITY_HIGH;
  oc.OCIdleState = TIM_OCIDLESTATE_RESET;
  oc.OCNIdleState = TIM_OCNIDLESTATE_RESET;
  if (HAL_TIM_PWM_ConfigChannel(&htim16, &oc, TIM_CHANNEL_1) != HAL_OK) {
    return false;
  }

  if (output == VOICE_OUT_SPEAKER) {
    pin_af(GPIOB, GPIO_PIN_8, GPIO_AF14_TIM16);
    pin_af(GPIOB, GPIO_PIN_6, GPIO_AF14_TIM16);
  } else {
    pin_af(GPIOA, GPIO_PIN_6, GPIO_AF14_TIM16);
  }

  half_ready_mask = 0;
  voice_abort_flag = false;
  voice_task = xTaskGetSchedulerState() == taskSCHEDULER_RUNNING ? xTaskGetCurrentTaskHandle() : NULL;
  if (HAL_TIM_PWM_Start_DMA(&htim16, TIM_CHANNEL_1, (uint32_t *)pwm_buf, 2 * BLOCK_OUT) != HAL_OK) {
    return false;
  }
  if (output == VOICE_OUT_SPEAKER) {
    HAL_TIMEx_PWMN_Start(&htim16, TIM_CHANNEL_1);
  }
  voice_running = true;
  return true;
}

void voice_stats(unsigned *late, unsigned *starved)
{
  *late = voice_late;
  *starved = voice_starved;
}

void voice_abort(void)
{
  voice_abort_flag = true;
}

/* Feeds the next block of 22.05 kHz samples; waits for a free half buffer. */
static bool voice_feed(const int16_t *samples, uint32_t n)
{
  uint32_t start = HAL_GetTick();
  if (half_ready_mask == 3U && voice_last_audible) {
    voice_late++; /* both halves already played: audio was repeated */
  }
  voice_last_audible = false;
  for (uint32_t i = 0; i < n && !voice_last_audible; i++) {
    voice_last_audible = samples[i] != 0;
  }
  while (!half_ready_mask) {
    if (HAL_GetTick() - start > 100) {
      return false; /* DMA not running */
    }
    if (voice_task) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    }
  }
  uint32_t half = (half_ready_mask & 1U) ? 0 : 1;
  __disable_irq();
  half_ready_mask &= ~(1U << half);
  __enable_irq();
  fill_block(&pwm_buf[half * BLOCK_OUT], samples, n);
  return true;
}

bool voice_play_tone(uint32_t freq_hz, uint32_t duration_ms, float amplitude)
{
  int16_t block[BLOCK_IN];
  float phase = 0.0f;
  float step = 2.0f * 3.14159265f * (float)freq_hz / (float)VOICE_RATE;
  uint32_t total = VOICE_RATE * duration_ms / 1000U;
  for (uint32_t done = 0; done < total; done += BLOCK_IN) {
    for (uint32_t i = 0; i < BLOCK_IN; i++) {
      block[i] = (int16_t)(32767.0f * amplitude * sinf(phase));
      phase += step;
      if (phase > 6.2831853f) {
        phase -= 6.2831853f;
      }
    }
    if (!voice_feed(block, BLOCK_IN)) {
      return false;
    }
  }
  int16_t silence[BLOCK_IN] = {0};
  voice_feed(silence, BLOCK_IN);
  voice_feed(silence, BLOCK_IN);
  return true;
}

/*
 * Clip data (QOA, about 8.9 kB/s) runs through a read-ahead ring. The file
 * system is shared: the raw log and the recorder write to the card, and a card
 * write can hold the storage lock for hundreds of milliseconds, far longer than
 * the 2 x 11.6 ms of the DMA buffer. So the ring is refilled only while the lock
 * is free at once, about 0.9 s ahead. If it runs dry anyway, silence is fed:
 * without new data the DMA would repeat the last buffer halves (a crackle).
 */
#define CLIP_RING   8192U /* power of two */
#define CLIP_FIRST  4096U /* read before the clip starts */
#define CLIP_REFILL 1024U /* per output block at most: one sector of the voice pack */

static FIL clip_file;
static uint8_t clip_ring[CLIP_RING];
static uint32_t clip_in;  /* bytes put into the ring */
static uint32_t clip_out; /* bytes taken out */
static bool clip_eof;

/* wait = false: only if the storage lock is free now. */
static void clip_fill(bool wait, uint32_t max)
{
  uint32_t space = CLIP_RING - (clip_in - clip_out);
  if (clip_eof || space < 512U) {
    return;
  }
  if (wait) {
    storage_lock();
  } else if (!storage_try_lock()) {
    return;
  }
  if (max > space) {
    max = space;
  }
  max &= ~511U; /* whole sectors: FatFs reads them straight into the ring */
  while (max > 0U && !clip_eof) {
    uint32_t pos = clip_in % CLIP_RING;
    uint32_t n = CLIP_RING - pos < max ? CLIP_RING - pos : max;
    UINT br = 0;
    if (f_read(&clip_file, &clip_ring[pos], n, &br) != FR_OK || br < n) {
      clip_eof = true; /* end of the file (or a read error) */
    }
    clip_in += br;
    max -= br; /* br <= n <= max */
  }
  storage_unlock();
}

static uint32_t clip_source(void *ctx, uint8_t *buf, uint32_t len)
{
  (void)ctx;
  uint32_t avail = clip_in - clip_out;
  if (len > avail) {
    len = avail;
  }
  for (uint32_t i = 0; i < len; i++) {
    buf[i] = clip_ring[(clip_out + i) % CLIP_RING];
  }
  clip_out += len;
  return len;
}

/* A new frame is due and its bytes are not in the ring yet. */
static bool clip_starved(const qoa_stream_t *qs)
{
  return qoa_buffered(qs) < BLOCK_IN && !clip_eof && clip_in - clip_out < QOA_FRAME_MAX;
}

uint32_t voice_play_qoa(const char *path)
{
  static qoa_stream_t qs;
  static int16_t block[BLOCK_IN];
  storage_lock();
  bool opened = f_open(&clip_file, path, FA_READ) == FR_OK;
  storage_unlock();
  uint32_t played = 0;
  if (opened) {
    clip_in = clip_out = 0;
    clip_eof = false;
    clip_fill(true, CLIP_FIRST);
    if (qoa_open(&qs, clip_source, NULL)) {
      unsigned silent = 0; /* silence blocks fed in a row */
      while (!voice_abort_flag) {
        if (clip_starved(&qs)) {
          /* Once both halves hold silence, waiting for the card is inaudible. */
          clip_fill(silent >= 2U, CLIP_RING);
          if (clip_starved(&qs)) {
            memset(block, 0, sizeof(block));
            if (!voice_feed(block, BLOCK_IN)) {
              break;
            }
            silent++;
            voice_starved++;
            continue;
          }
        }
        silent = 0;
        uint32_t n = qoa_read(&qs, block, BLOCK_IN);
        if (n == 0 || !voice_feed(block, n)) {
          break;
        }
        played += n;
        if (n < BLOCK_IN) {
          break;
        }
        clip_fill(false, CLIP_REFILL);
      }
    }
    memset(block, 0, sizeof(block));
    voice_feed(block, BLOCK_IN);
    voice_feed(block, BLOCK_IN);
    storage_lock();
    f_close(&clip_file);
    storage_unlock();
  }
  return played;
}
