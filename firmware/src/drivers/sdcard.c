/*
 * microSD card over SPI2 (PD1 SCK, PC2 MISO, PC3 MOSI, PD0 CS, PC14 power,
 * active low) and the FatFs disk I/O layer on top of it.
 *
 * The SPI data path uses the registers directly (8-bit frames, RX FIFO
 * threshold 1/4) so every transferred byte is accounted for; the protocol
 * follows the well-known MMC/SDC SPI sequence (CMD0, CMD8, ACMD41, CMD58).
 * The 512-byte data blocks go through DMA1 channels 6 (RX) and 7 (TX);
 * commands, tokens and the 16-byte registers stay on the byte loops.
 */
#include <string.h>

#include "disk.h"
#include "board.h"
#include "diskio.h"

#define CMD0   0   /* GO_IDLE_STATE */
#define CMD1   1   /* SEND_OP_COND (MMC) */
#define ACMD41 (0x80 + 41)
#define CMD8   8   /* SEND_IF_COND */
#define CMD9   9   /* SEND_CSD */
#define CMD10  10  /* SEND_CID */
#define CMD12  12  /* STOP_TRANSMISSION */
#define CMD16  16  /* SET_BLOCKLEN */
#define CMD17  17  /* READ_SINGLE_BLOCK */
#define CMD18  18  /* READ_MULTIPLE_BLOCK */
#define ACMD23 (0x80 + 23)
#define CMD24  24  /* WRITE_BLOCK */
#define CMD25  25  /* WRITE_MULTIPLE_BLOCK */
#define CMD55  55  /* APP_CMD */
#define CMD58  58  /* READ_OCR */

#define SPI_BR_250K (7U << SPI_CR1_BR_Pos) /* 64 MHz / 256 */
#define SPI_BR_8M   (2U << SPI_CR1_BR_Pos) /* 64 MHz / 8 */
#define SPI_BR_16M  (1U << SPI_CR1_BR_Pos) /* 64 MHz / 4: SD cards take 25 MHz in SPI mode */

/* A byte takes 32 us at 250 kHz; this many status polls are far more (~10 ms). */
#define SPI_POLL_LIMIT 100000U

/* DMA for the data blocks: DMA1 channel 6 (SPI2 RX) and 7 (SPI2 TX) via DMAMUX channels 5 and 6. */
#define SD_USE_DMA   1
#define SD_DMA       DMA1
#define SD_DMA_RX    DMA1_Channel6
#define SD_DMA_TX    DMA1_Channel7
#define SD_DMAMUX_RX DMAMUX1_Channel5
#define SD_DMAMUX_TX DMAMUX1_Channel6
#define SD_DMA_RX_TC DMA_ISR_TCIF6
#define SD_DMA_CLEAR (DMA_IFCR_CGIF6 | DMA_IFCR_CGIF7)

static volatile DSTATUS disk_stat = STA_NOINIT;
static uint8_t card_type;
static bool spi_running; /* SPI2 clocked and enabled: only then may a byte be exchanged */

static void spi_setup(uint32_t br)
{
  SPI2->CR1 = 0;
  SPI2->CR2 = (7U << SPI_CR2_DS_Pos) | SPI_CR2_FRXTH;
  SPI2->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | br;
  SPI2->CR1 |= SPI_CR1_SPE;
  spi_running = true;
}

/* Bounded: a stopped SPI never sets TXE/RXNE, which once hung the power-off path. */
static uint8_t spi_xchg(uint8_t out)
{
  if (!spi_running) {
    return 0xFF;
  }
  uint32_t n = SPI_POLL_LIMIT;
  while (!(SPI2->SR & SPI_SR_TXE)) {
    if (--n == 0U) {
      return 0xFF;
    }
  }
  *(volatile uint8_t *)&SPI2->DR = out;
  n = SPI_POLL_LIMIT;
  while (!(SPI2->SR & SPI_SR_RXNE)) {
    if (--n == 0U) {
      return 0xFF;
    }
  }
  return *(volatile uint8_t *)&SPI2->DR;
}

static void cs_high(void) { HAL_GPIO_WritePin(SD_CS_PORT, SD_CS_PIN, GPIO_PIN_SET); }
static void cs_low(void)  { HAL_GPIO_WritePin(SD_CS_PORT, SD_CS_PIN, GPIO_PIN_RESET); }

static bool wait_ready(uint32_t timeout_ms)
{
  uint32_t start = HAL_GetTick();
  uint8_t d;
  do {
    d = spi_xchg(0xFF);
  } while (d != 0xFF && (HAL_GetTick() - start) < timeout_ms);
  return d == 0xFF;
}

static void deselect(void)
{
  cs_high();
  spi_xchg(0xFF); /* one extra clock to release MISO */
}

static bool select_card(void)
{
  cs_low();
  spi_xchg(0xFF);
  if (wait_ready(500)) {
    return true;
  }
  deselect();
  return false;
}

static uint8_t send_cmd(uint8_t cmd, uint32_t arg)
{
  uint8_t res;
  if (cmd & 0x80) {
    cmd &= 0x7F;
    res = send_cmd(CMD55, 0);
    if (res > 1) {
      return res;
    }
  }
  if (cmd != CMD12) {
    deselect();
    if (!select_card()) {
      return 0xFF;
    }
  }
  spi_xchg(0x40 | cmd);
  spi_xchg((uint8_t)(arg >> 24));
  spi_xchg((uint8_t)(arg >> 16));
  spi_xchg((uint8_t)(arg >> 8));
  spi_xchg((uint8_t)arg);
  uint8_t crc = 0x01;
  if (cmd == CMD0) {
    crc = 0x95;
  } else if (cmd == CMD8) {
    crc = 0x87;
  }
  spi_xchg(crc);
  if (cmd == CMD12) {
    spi_xchg(0xFF); /* skip stuff byte */
  }
  uint8_t n = 10;
  do {
    res = spi_xchg(0xFF);
  } while ((res & 0x80) && --n);
  return res;
}

/* Leftover bytes in the receive FIFO after an aborted exchange would shift every answer. */
static void spi_flush(void)
{
  uint32_t n = 8;
  while ((SPI2->SR & SPI_SR_RXNE) && n--) {
    (void)*(volatile uint8_t *)&SPI2->DR;
  }
}

/*
 * Block transfers keep one byte in flight: the next byte is written before the answer to the
 * current one is read, so the bus does not idle while the loop runs. Bounded like spi_xchg.
 */
static bool rcvr_bytes(uint8_t *buf, uint32_t n)
{
  if (!spi_running || n == 0U) {
    return false;
  }
  uint32_t guard;
  *(volatile uint8_t *)&SPI2->DR = 0xFF;
  for (uint32_t i = 0; i < n; i++) {
    if (i + 1U < n) {
      guard = SPI_POLL_LIMIT;
      while (!(SPI2->SR & SPI_SR_TXE)) {
        if (!--guard) {
          spi_flush();
          return false;
        }
      }
      *(volatile uint8_t *)&SPI2->DR = 0xFF;
    }
    guard = SPI_POLL_LIMIT;
    while (!(SPI2->SR & SPI_SR_RXNE)) {
      if (!--guard) {
        spi_flush();
        return false;
      }
    }
    buf[i] = *(volatile uint8_t *)&SPI2->DR;
  }
  return true;
}

static bool xmit_bytes(const uint8_t *buf, uint32_t n)
{
  if (!spi_running || n == 0U) {
    return false;
  }
  uint32_t guard;
  *(volatile uint8_t *)&SPI2->DR = buf[0];
  for (uint32_t i = 0; i < n; i++) {
    if (i + 1U < n) {
      guard = SPI_POLL_LIMIT;
      while (!(SPI2->SR & SPI_SR_TXE)) {
        if (!--guard) {
          spi_flush();
          return false;
        }
      }
      *(volatile uint8_t *)&SPI2->DR = buf[i + 1U];
    }
    guard = SPI_POLL_LIMIT;
    while (!(SPI2->SR & SPI_SR_RXNE)) {
      if (!--guard) {
        spi_flush();
        return false;
      }
    }
    (void)*(volatile uint8_t *)&SPI2->DR;
  }
  return true;
}

/*
 * One data block by DMA: the TX channel clocks n bytes out (buf, or 0xFF while
 * reading), the RX channel takes the answers (buf, or a sink while writing).
 * Waits for the RX transfer, bounded like the byte loops; no interrupts, so it
 * works from any context the byte loops work from.
 */
static bool dma_xfer(uint8_t *rx, const uint8_t *tx, uint32_t n)
{
  static const uint8_t ones = 0xFF;
  static uint8_t sink;
  if (!spi_running || n == 0U) {
    return false;
  }
  spi_flush();
  SD_DMA_RX->CCR = 0;
  SD_DMA_TX->CCR = 0;
  SD_DMA->IFCR = SD_DMA_CLEAR;
  SD_DMA_RX->CPAR = (uint32_t)&SPI2->DR;
  SD_DMA_RX->CMAR = (uint32_t)(rx ? rx : &sink);
  SD_DMA_RX->CNDTR = n;
  SD_DMA_TX->CPAR = (uint32_t)&SPI2->DR;
  SD_DMA_TX->CMAR = (uint32_t)(tx ? tx : &ones);
  SD_DMA_TX->CNDTR = n;
  /* 8-bit both sides, high priority (the audio DMA on channel 5 still wins), RX armed before TX starts */
  SD_DMA_RX->CCR = (rx ? DMA_CCR_MINC : 0U) | DMA_CCR_PL_1 | DMA_CCR_EN;
  SPI2->CR2 |= SPI_CR2_RXDMAEN;
  SD_DMA_TX->CCR = (tx ? DMA_CCR_MINC : 0U) | DMA_CCR_DIR | DMA_CCR_PL_1 | DMA_CCR_EN;
  SPI2->CR2 |= SPI_CR2_TXDMAEN;
  uint32_t guard = SPI_POLL_LIMIT * 4U; /* 512 bytes take 0.26 ms at 16 MHz */
  while (!(SD_DMA->ISR & SD_DMA_RX_TC) && --guard) {
  }
  bool ok = guard != 0U;
  SPI2->CR2 &= ~(SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN);
  SD_DMA_RX->CCR = 0;
  SD_DMA_TX->CCR = 0;
  SD_DMA->IFCR = SD_DMA_CLEAR;
  if (!ok) {
    spi_flush();
  }
  return ok;
}

static bool block_in(uint8_t *buf, uint32_t n)
{
  return (SD_USE_DMA && n >= 512U) ? dma_xfer(buf, NULL, n) : rcvr_bytes(buf, n);
}

static bool block_out(const uint8_t *buf, uint32_t n)
{
  return (SD_USE_DMA && n >= 512U) ? dma_xfer(NULL, buf, n) : xmit_bytes(buf, n);
}

static bool rcvr_datablock(uint8_t *buf, uint32_t len)
{
  uint32_t start = HAL_GetTick();
  uint8_t token;
  do {
    token = spi_xchg(0xFF);
  } while (token == 0xFF && (HAL_GetTick() - start) < 200);
  if (token != 0xFE) {
    return false;
  }
  if (!block_in(buf, len)) {
    return false;
  }
  spi_xchg(0xFF); /* CRC */
  spi_xchg(0xFF);
  return true;
}

static bool xmit_datablock(const uint8_t *buf, uint8_t token)
{
  if (!wait_ready(500)) {
    return false;
  }
  spi_xchg(token);
  if (token != 0xFD) {
    if (!block_out(buf, 512)) {
      return false;
    }
    spi_xchg(0xFF); /* dummy CRC */
    spi_xchg(0xFF);
    if ((spi_xchg(0xFF) & 0x1F) != 0x05) {
      return false;
    }
  }
  return true;
}

static void sd_hw_init(void)
{
  BOARD_ATOMIC(__HAL_RCC_SPI2_CLK_ENABLE());
  BOARD_ATOMIC(__HAL_RCC_GPIOC_CLK_ENABLE());
  BOARD_ATOMIC(__HAL_RCC_GPIOD_CLK_ENABLE());
  BOARD_ATOMIC(__HAL_RCC_DMAMUX1_CLK_ENABLE());
  BOARD_ATOMIC(__HAL_RCC_DMA1_CLK_ENABLE());
  SD_DMAMUX_RX->CCR = DMA_REQUEST_SPI2_RX;
  SD_DMAMUX_TX->CCR = DMA_REQUEST_SPI2_TX;

  GPIO_InitTypeDef init = {0};
  init.Mode = GPIO_MODE_AF_PP;
  init.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  init.Alternate = GPIO_AF5_SPI2;
  init.Pull = GPIO_NOPULL;
  init.Pin = SD_SCK_PIN;
  board_gpio_init(SD_SCK_PORT, &init);
  init.Pull = GPIO_PULLUP;
  init.Pin = SD_MISO_PIN | SD_MOSI_PIN; /* both on GPIOC */
  board_gpio_init(SD_MISO_PORT, &init);
}

/* The stock waits 80 ms after switching the card off: it must be fully discharged to start
 * from reset at the next power on. */
#define SD_OFF_MIN_MS 80U
static uint32_t off_tick;
static bool off_tick_valid;

void sd_power_off(void)
{
  if (spi_running) {
    deselect();
    SPI2->CR1 = 0;
    spi_running = false;
  }
  HAL_GPIO_WritePin(SD_PWR_PORT, SD_PWR_PIN, GPIO_PIN_SET);
  /* No pin may feed the unpowered card through its protection diodes: it
   * would not start cleanly at the next power on. */
  cs_low();
  board_gpio_deinit(SD_SCK_PORT, SD_SCK_PIN);
  board_gpio_deinit(SD_MISO_PORT, SD_MISO_PIN | SD_MOSI_PIN);
  disk_stat = STA_NOINIT;
  off_tick = HAL_GetTick();
  off_tick_valid = true;
}

DSTATUS sd_disk_initialize(BYTE pdrv)
{
  uint8_t ocr[4];
  if (pdrv != 0) {
    return STA_NOINIT;
  }

  if (off_tick_valid) {
    uint32_t off_ms = HAL_GetTick() - off_tick;
    if (off_ms < SD_OFF_MIN_MS) {
      board_delay_ms(SD_OFF_MIN_MS - off_ms);
    }
  }
  sd_hw_init();
  HAL_GPIO_WritePin(SD_PWR_PORT, SD_PWR_PIN, GPIO_PIN_RESET); /* power on */
  board_delay_ms(20);
  spi_setup(SPI_BR_250K);
  cs_high();
  for (int i = 0; i < 10; i++) {
    spi_xchg(0xFF); /* >= 74 clocks with CS high */
  }

  uint8_t type = 0;
  if (send_cmd(CMD0, 0) == 1) {
    uint32_t start = HAL_GetTick();
    if (send_cmd(CMD8, 0x1AA) == 1) {
      for (int i = 0; i < 4; i++) {
        ocr[i] = spi_xchg(0xFF);
      }
      if (ocr[2] == 0x01 && ocr[3] == 0xAA) {
        while ((HAL_GetTick() - start) < 1000 && send_cmd(ACMD41, 1UL << 30) != 0) {
        }
        if ((HAL_GetTick() - start) < 1000 && send_cmd(CMD58, 0) == 0) {
          for (int i = 0; i < 4; i++) {
            ocr[i] = spi_xchg(0xFF);
          }
          type = (ocr[0] & 0x40) ? (CT_SD2 | CT_BLOCK) : CT_SD2;
        }
      }
    } else {
      uint8_t cmd;
      if (send_cmd(ACMD41, 0) <= 1) {
        type = CT_SD1;
        cmd = ACMD41;
      } else {
        type = CT_MMC;
        cmd = CMD1;
      }
      while ((HAL_GetTick() - start) < 1000 && send_cmd(cmd, 0) != 0) {
      }
      if ((HAL_GetTick() - start) >= 1000 || send_cmd(CMD16, 512) != 0) {
        type = 0;
      }
    }
  }
  card_type = type;
  deselect();

  if (type) {
    spi_setup(SPI_BR_16M);
    disk_stat &= ~STA_NOINIT;
  } else {
    sd_power_off();
  }
  return disk_stat;
}

DSTATUS sd_disk_status(BYTE pdrv)
{
  return pdrv ? STA_NOINIT : disk_stat;
}

DRESULT sd_disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
  if (pdrv || !count) {
    return RES_PARERR;
  }
  if (disk_stat & STA_NOINIT) {
    return RES_NOTRDY;
  }
  if (!(card_type & CT_BLOCK)) {
    sector *= 512;
  }
  if (count == 1) {
    if (send_cmd(CMD17, sector) == 0 && rcvr_datablock(buff, 512)) {
      count = 0;
    }
  } else if (send_cmd(CMD18, sector) == 0) {
    do {
      if (!rcvr_datablock(buff, 512)) {
        break;
      }
      buff += 512;
    } while (--count);
    send_cmd(CMD12, 0);
  }
  deselect();
  return count ? RES_ERROR : RES_OK;
}

DRESULT sd_disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
  if (pdrv || !count) {
    return RES_PARERR;
  }
  if (disk_stat & STA_NOINIT) {
    return RES_NOTRDY;
  }
  if (!(card_type & CT_BLOCK)) {
    sector *= 512;
  }
  if (count == 1) {
    if (send_cmd(CMD24, sector) == 0 && xmit_datablock(buff, 0xFE)) {
      count = 0;
    }
  } else {
    if (card_type & (CT_SD1 | CT_SD2)) {
      send_cmd(ACMD23, count);
    }
    if (send_cmd(CMD25, sector) == 0) {
      do {
        if (!xmit_datablock(buff, 0xFC)) {
          break;
        }
        buff += 512;
      } while (--count);
      if (!xmit_datablock(NULL, 0xFD)) {
        count = 1;
      }
    }
  }
  deselect();
  return count ? RES_ERROR : RES_OK;
}

static bool read_register(uint8_t cmd, uint8_t out[16])
{
  bool ok = send_cmd(cmd, 0) == 0 && rcvr_datablock(out, 16);
  deselect();
  return ok;
}

DRESULT sd_disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
  if (pdrv) {
    return RES_PARERR;
  }
  if (disk_stat & STA_NOINIT) {
    return RES_NOTRDY;
  }
  DRESULT res = RES_ERROR;
  uint8_t csd[16];
  switch (cmd) {
  case CTRL_SYNC:
    if (select_card()) {
      res = RES_OK;
    }
    deselect();
    break;
  case GET_SECTOR_COUNT:
    if (read_register(CMD9, csd)) {
      DWORD sectors;
      if ((csd[0] >> 6) == 1) { /* CSD v2 */
        DWORD csize = csd[9] + ((DWORD)csd[8] << 8) + ((DWORD)(csd[7] & 63) << 16) + 1;
        sectors = csize << 10;
      } else {                  /* CSD v1 */
        uint8_t n = (csd[5] & 15) + ((csd[10] & 128) >> 7) + ((csd[9] & 3) << 1) + 2;
        DWORD csize = (csd[8] >> 6) + ((DWORD)csd[7] << 2) + ((DWORD)(csd[6] & 3) << 10) + 1;
        sectors = csize << (n - 9);
      }
      *(DWORD *)buff = sectors;
      res = RES_OK;
    }
    break;
  case GET_SECTOR_SIZE:
    *(WORD *)buff = 512;
    res = RES_OK;
    break;
  case GET_BLOCK_SIZE:
    *(DWORD *)buff = 128;
    res = RES_OK;
    break;
  default:
    res = RES_PARERR;
    break;
  }
  return res;
}

bool sd_get_info(sd_info_t *info)
{
  memset(info, 0, sizeof(*info));
  if (disk_stat & STA_NOINIT) {
    return false;
  }
  info->type = card_type;
  DWORD sectors = 0;
  if (sd_disk_ioctl(0, GET_SECTOR_COUNT, &sectors) == RES_OK) {
    info->sectors = sectors;
  }
  bool ok = read_register(CMD9, info->csd);
  ok = read_register(CMD10, info->cid) && ok;
  return ok;
}
