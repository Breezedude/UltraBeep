#include "board.h"
#include "faults.h"

/*
 * Same clock tree as the stock firmware:
 * HSE 32 MHz -> PLL (/2 x8 /2) -> SYSCLK 64 MHz, HCLK2 (CPU2) 32 MHz,
 * voltage range 1, 3 flash wait states, HSI + LSI1/LSI2 on, no LSE.
 */
/*
 * HSE crystal settings of the stock firmware: sense amplifier
 * high, current limit 3 and load capacitor tuning 0x19 on "UltraBip v1.0"
 * hardware, 0 on v1.1. The stock firmware takes the hardware revision from the
 * bootloader version in RTC BKP14 bits 16..23 (BL_v3 or older = v1.0).
 * The radio needs the 32 MHz to be accurate (BLE: +-50 ppm).
 */
static void hse_config_like_stock(void)
{
  BOARD_ATOMIC(__HAL_RCC_RTCAPB_CLK_ENABLE());
  uint32_t bl_version = (RTC->BKP14R >> 16) & 0xFFU;
  uint32_t tune = bl_version <= 3U ? 0x19U : 0U;
  const uint32_t key = 0xCAFECAFEU; /* every HSECR write needs the key first */
  RCC->HSECR = key;
  RCC->HSECR |= RCC_HSECR_HSES;
  RCC->HSECR = key;
  RCC->HSECR = (RCC->HSECR & ~RCC_HSECR_HSEGMC) | (3U << RCC_HSECR_HSEGMC_Pos);
  RCC->HSECR = key;
  RCC->HSECR = (RCC->HSECR & ~RCC_HSECR_HSETUNE) | (tune << RCC_HSECR_HSETUNE_Pos);
}

uint32_t clock_hse_tune(void)
{
  return (RCC->HSECR & RCC_HSECR_HSETUNE) >> RCC_HSECR_HSETUNE_Pos;
}

void SystemClock_Config(void)
{
  RCC_OscInitTypeDef osc = {0};
  RCC_ClkInitTypeDef clk = {0};

  /* The bootloader may leave the PLL running as system clock. */
  HAL_RCC_DeInit();
  hse_config_like_stock();

  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  osc.OscillatorType = RCC_OSCILLATORTYPE_HSE | RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_LSI1;
  osc.HSEState = RCC_HSE_ON;
  osc.HSIState = RCC_HSI_ON;
  osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  osc.LSIState = RCC_LSI_ON;
  osc.PLL.PLLState = RCC_PLL_ON;
  osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  osc.PLL.PLLM = RCC_PLLM_DIV2;
  osc.PLL.PLLN = 8;
  osc.PLL.PLLP = RCC_PLLP_DIV2;
  osc.PLL.PLLQ = RCC_PLLQ_DIV2;
  osc.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
    fault_assert(__FILE__, __LINE__);
  }

  clk.ClockType = RCC_CLOCKTYPE_HCLK4 | RCC_CLOCKTYPE_HCLK2 | RCC_CLOCKTYPE_HCLK |
                  RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clk.APB1CLKDivider = RCC_HCLK_DIV1;
  clk.APB2CLKDivider = RCC_HCLK_DIV1;
  clk.AHBCLK2Divider = RCC_SYSCLK_DIV2;
  clk.AHBCLK4Divider = RCC_SYSCLK_DIV1;
  if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_3) != HAL_OK) {
    fault_assert(__FILE__, __LINE__);
  }
}
