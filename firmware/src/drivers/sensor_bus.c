#include "sensor_bus.h"

#include "board.h"

#define BARO_CS_PORT GPIOC
#define BARO_CS_PIN  GPIO_PIN_10
#define IMU_CS_PORT  GPIOC
#define IMU_CS_PIN   GPIO_PIN_11

/* A byte takes 1 us at 8 MHz; this many status polls are far more. */
#define POLL_LIMIT 10000U

static bool spi_xchg(uint8_t out, uint8_t *in)
{
  uint32_t n = POLL_LIMIT;
  while (!(SPI1->SR & SPI_SR_TXE)) {
    if (--n == 0U) {
      return false;
    }
  }
  *(volatile uint8_t *)&SPI1->DR = out;
  n = POLL_LIMIT;
  while (!(SPI1->SR & SPI_SR_RXNE)) {
    if (--n == 0U) {
      return false;
    }
  }
  uint8_t v = *(volatile uint8_t *)&SPI1->DR;
  if (in) {
    *in = v;
  }
  return true;
}

static void select(sensor_dev_t dev, bool on)
{
  GPIO_PinState s = on ? GPIO_PIN_RESET : GPIO_PIN_SET;
  if (dev == SENSOR_BARO) {
    HAL_GPIO_WritePin(BARO_CS_PORT, BARO_CS_PIN, s);
  } else {
    HAL_GPIO_WritePin(IMU_CS_PORT, IMU_CS_PIN, s);
  }
}

void sensor_bus_init(void)
{
  BOARD_ATOMIC(__HAL_RCC_SPI1_CLK_ENABLE());
  BOARD_ATOMIC(__HAL_RCC_GPIOA_CLK_ENABLE());
  BOARD_ATOMIC(__HAL_RCC_GPIOB_CLK_ENABLE());
  BOARD_ATOMIC(__HAL_RCC_GPIOC_CLK_ENABLE());

  HAL_GPIO_WritePin(BARO_CS_PORT, BARO_CS_PIN, GPIO_PIN_SET);
  HAL_GPIO_WritePin(IMU_CS_PORT, IMU_CS_PIN, GPIO_PIN_SET);
  GPIO_InitTypeDef init = {0};
  init.Mode = GPIO_MODE_OUTPUT_PP;
  init.Speed = GPIO_SPEED_FREQ_LOW;
  init.Pin = BARO_CS_PIN | IMU_CS_PIN; /* both on GPIOC */
  board_gpio_init(BARO_CS_PORT, &init);

  init.Mode = GPIO_MODE_AF_PP;
  init.Pull = GPIO_PULLUP;
  init.Speed = GPIO_SPEED_FREQ_HIGH;
  init.Alternate = GPIO_AF5_SPI1;
  init.Pin = GPIO_PIN_5;
  board_gpio_init(GPIOA, &init);
  init.Pin = GPIO_PIN_4 | GPIO_PIN_5;
  board_gpio_init(GPIOB, &init);

  SPI1->CR1 = 0;
  SPI1->CR2 = (7U << SPI_CR2_DS_Pos) | SPI_CR2_FRXTH;
  SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | SPI_CR1_CPOL | SPI_CR1_CPHA |
              (2U << SPI_CR1_BR_Pos); /* 64 MHz / 8 = 8 MHz */
  SPI1->CR1 |= SPI_CR1_SPE;
}

void sensor_bus_release(void)
{
  SPI1->CR1 = 0;
  board_gpio_deinit(BARO_CS_PORT, BARO_CS_PIN | IMU_CS_PIN);
  board_gpio_deinit(GPIOA, GPIO_PIN_5);
  board_gpio_deinit(GPIOB, GPIO_PIN_4 | GPIO_PIN_5);
}

bool sensor_bus_read(sensor_dev_t dev, uint8_t reg, uint8_t *buf, size_t n)
{
  select(dev, true);
  bool ok = spi_xchg(reg | 0x80U, NULL);
  for (size_t i = 0; ok && i < n; i++) {
    ok = spi_xchg(0x00, &buf[i]);
  }
  select(dev, false);
  return ok;
}

bool sensor_bus_write(sensor_dev_t dev, uint8_t reg, uint8_t value)
{
  select(dev, true);
  bool ok = spi_xchg(reg & 0x7FU, NULL) && spi_xchg(value, NULL);
  select(dev, false);
  return ok;
}

uint8_t sensor_bus_read1(sensor_dev_t dev, uint8_t reg)
{
  uint8_t v = 0;
  sensor_bus_read(dev, reg, &v, 1);
  return v;
}
