#include "uart_console.h"

#include "FreeRTOS.h"
#include "task.h"

#include "board.h"

#define TX_SIZE 1024U /* power of two; about 10 ms of output at 1 Mbaud */
#define RX_SIZE 64U   /* power of two */

static volatile uint8_t tx_ring[TX_SIZE];
static volatile uint32_t tx_head; /* written by the tasks */
static volatile uint32_t tx_tail; /* advanced by the interrupt */
static volatile uint8_t rx_ring[RX_SIZE];
static volatile uint32_t rx_head; /* advanced by the interrupt */
static volatile uint32_t rx_tail;
static volatile bool enabled;

static void pins(bool on)
{
  if (!on) {
    board_gpio_deinit(GPIOC, GPIO_PIN_0 | GPIO_PIN_1);
    return;
  }
  BOARD_ATOMIC(__HAL_RCC_GPIOC_CLK_ENABLE());
  GPIO_InitTypeDef init = {0};
  init.Pin = GPIO_PIN_0 | GPIO_PIN_1;
  init.Mode = GPIO_MODE_AF_PP;
  init.Pull = GPIO_PULLUP; /* an open RX line reads idle */
  init.Speed = GPIO_SPEED_FREQ_HIGH;
  init.Alternate = GPIO_AF8_LPUART1;
  board_gpio_init(GPIOC, &init);
}

void uart_console_enable(bool on)
{
  if (on == enabled) {
    return;
  }
  if (on) {
    BOARD_ATOMIC(__HAL_RCC_LPUART1_CLK_ENABLE());
    pins(true);
    LPUART1->CR1 = 0;
    LPUART1->BRR = (uint32_t)(((uint64_t)HAL_RCC_GetPCLK1Freq() * 256U + UART_CONSOLE_BAUD / 2U) / UART_CONSOLE_BAUD);
    LPUART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE_RXFNEIE;
    LPUART1->CR1 |= USART_CR1_UE;
    tx_head = tx_tail = 0;
    rx_head = rx_tail = 0;
    HAL_NVIC_SetPriority(LPUART1_IRQn, 6, 0); /* below configMAX_SYSCALL: no FreeRTOS calls in it anyway */
    HAL_NVIC_EnableIRQ(LPUART1_IRQn);
    enabled = true;
  } else {
    enabled = false;
    HAL_NVIC_DisableIRQ(LPUART1_IRQn);
    LPUART1->CR1 = 0;
    pins(false);
    BOARD_ATOMIC(__HAL_RCC_LPUART1_CLK_DISABLE());
  }
}

bool uart_console_enabled(void)
{
  return enabled;
}

void uart_console_write(const char *data, size_t len)
{
  if (!enabled) {
    return;
  }
  uint32_t head = tx_head;
  for (size_t i = 0; i < len; i++) {
    if (head - tx_tail >= TX_SIZE) {
      break; /* full: the rest is dropped */
    }
    tx_ring[head % TX_SIZE] = (uint8_t)data[i];
    head++;
  }
  tx_head = head;
  BOARD_ATOMIC(LPUART1->CR1 |= USART_CR1_TXEIE_TXFNFIE);
}

int uart_console_getc(void)
{
  if (!enabled || rx_tail == rx_head) {
    return -1;
  }
  int c = rx_ring[rx_tail % RX_SIZE];
  rx_tail++;
  return c;
}

void LPUART1_IRQHandler(void)
{
  uint32_t isr = LPUART1->ISR;
  if (isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE)) {
    LPUART1->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF;
  }
  if (isr & USART_ISR_RXNE_RXFNE) {
    uint8_t c = (uint8_t)LPUART1->RDR;
    if (rx_head - rx_tail < RX_SIZE) {
      rx_ring[rx_head % RX_SIZE] = c;
      rx_head++;
    }
  }
  if ((LPUART1->CR1 & USART_CR1_TXEIE_TXFNFIE) && (isr & USART_ISR_TXE_TXFNF)) {
    if (tx_tail != tx_head) {
      LPUART1->TDR = tx_ring[tx_tail % TX_SIZE];
      tx_tail++;
    } else {
      LPUART1->CR1 &= ~USART_CR1_TXEIE_TXFNFIE;
    }
  }
}
