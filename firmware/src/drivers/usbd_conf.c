/*
 * Glue between the ST USB device library and the STM32WB HAL PCD driver.
 */
#include <stdbool.h>

#include "stm32wbxx_hal.h"
#include "board.h"
#include "usbd_cdc.h"
#include "usbd_core.h"
#include "usbd_msc.h"

PCD_HandleTypeDef hpcd_USB_FS;

/* ---- static allocation for the two class handles ---- */

#define POOL_WORDS (((sizeof(USBD_MSC_BOT_HandleTypeDef) > sizeof(USBD_CDC_HandleTypeDef)) \
                       ? sizeof(USBD_MSC_BOT_HandleTypeDef) : sizeof(USBD_CDC_HandleTypeDef)) / 4U + 1U)
static uint32_t pool[USBD_MAX_SUPPORTED_CLASS][POOL_WORDS];
static bool pool_used[USBD_MAX_SUPPORTED_CLASS];

void *USBD_static_malloc(uint32_t size)
{
  if (size > sizeof(pool[0])) {
    return NULL;
  }
  for (unsigned i = 0; i < USBD_MAX_SUPPORTED_CLASS; i++) {
    if (!pool_used[i]) {
      pool_used[i] = true;
      return pool[i];
    }
  }
  return NULL;
}

void USBD_static_free(void *p)
{
  for (unsigned i = 0; i < USBD_MAX_SUPPORTED_CLASS; i++) {
    if (p == pool[i]) {
      pool_used[i] = false;
    }
  }
}

/* ---- MSP ---- */

void HAL_PCD_MspInit(PCD_HandleTypeDef *hpcd)
{
  if (hpcd->Instance != USB) {
    return;
  }
  BOARD_ATOMIC(__HAL_RCC_GPIOA_CLK_ENABLE());
  GPIO_InitTypeDef init = {0};
  init.Pin = GPIO_PIN_11 | GPIO_PIN_12;
  init.Mode = GPIO_MODE_AF_PP;
  init.Pull = GPIO_NOPULL;
  init.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  init.Alternate = GPIO_AF10_USB;
  board_gpio_init(GPIOA, &init);

  BOARD_ATOMIC(__HAL_RCC_USB_CLK_ENABLE());
  HAL_NVIC_SetPriority(USB_LP_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(USB_LP_IRQn);
}

void HAL_PCD_MspDeInit(PCD_HandleTypeDef *hpcd)
{
  if (hpcd->Instance != USB) {
    return;
  }
  HAL_NVIC_DisableIRQ(USB_LP_IRQn);
  __HAL_RCC_USB_CLK_DISABLE();
  board_gpio_deinit(GPIOA, GPIO_PIN_11 | GPIO_PIN_12);
}

void USB_LP_IRQHandler(void)
{
  HAL_PCD_IRQHandler(&hpcd_USB_FS);
}

/* ---- HAL PCD callbacks -> USB device library ---- */

void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef *hpcd)
{
  USBD_LL_SetupStage((USBD_HandleTypeDef *)hpcd->pData, (uint8_t *)hpcd->Setup);
}

void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
  USBD_LL_DataOutStage((USBD_HandleTypeDef *)hpcd->pData, epnum, hpcd->OUT_ep[epnum].xfer_buff);
}

void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
  USBD_LL_DataInStage((USBD_HandleTypeDef *)hpcd->pData, epnum, hpcd->IN_ep[epnum].xfer_buff);
}

void HAL_PCD_SOFCallback(PCD_HandleTypeDef *hpcd)
{
  USBD_LL_SOF((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_ResetCallback(PCD_HandleTypeDef *hpcd)
{
  USBD_LL_SetSpeed((USBD_HandleTypeDef *)hpcd->pData, USBD_SPEED_FULL);
  USBD_LL_Reset((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_SuspendCallback(PCD_HandleTypeDef *hpcd)
{
  USBD_LL_Suspend((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_ResumeCallback(PCD_HandleTypeDef *hpcd)
{
  USBD_LL_Resume((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_ISOOUTIncompleteCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
  USBD_LL_IsoOUTIncomplete((USBD_HandleTypeDef *)hpcd->pData, epnum);
}

void HAL_PCD_ISOINIncompleteCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
  USBD_LL_IsoINIncomplete((USBD_HandleTypeDef *)hpcd->pData, epnum);
}

void HAL_PCD_ConnectCallback(PCD_HandleTypeDef *hpcd)
{
  USBD_LL_DevConnected((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_DisconnectCallback(PCD_HandleTypeDef *hpcd)
{
  USBD_LL_DevDisconnected((USBD_HandleTypeDef *)hpcd->pData);
}

/* ---- low level driver interface ---- */

static USBD_StatusTypeDef status(HAL_StatusTypeDef s)
{
  switch (s) {
  case HAL_OK:   return USBD_OK;
  case HAL_BUSY: return USBD_BUSY;
  default:       return USBD_FAIL;
  }
}

USBD_StatusTypeDef USBD_LL_Init(USBD_HandleTypeDef *pdev)
{
  hpcd_USB_FS.pData = pdev;
  pdev->pData = &hpcd_USB_FS;
  hpcd_USB_FS.Instance = USB;
  hpcd_USB_FS.Init.dev_endpoints = 8;
  hpcd_USB_FS.Init.speed = PCD_SPEED_FULL;
  hpcd_USB_FS.Init.phy_itface = PCD_PHY_EMBEDDED;
  hpcd_USB_FS.Init.Sof_enable = DISABLE;
  hpcd_USB_FS.Init.low_power_enable = DISABLE;
  hpcd_USB_FS.Init.lpm_enable = DISABLE;
  hpcd_USB_FS.Init.battery_charging_enable = DISABLE;
  if (HAL_PCD_Init(&hpcd_USB_FS) != HAL_OK) {
    return USBD_FAIL;
  }
  /* Packet memory: buffer descriptor table at 0x00, then 64-byte buffers. */
  PCD_HandleTypeDef *h = &hpcd_USB_FS;
  HAL_PCDEx_PMAConfig(h, 0x00, PCD_SNG_BUF, 0x040);
  HAL_PCDEx_PMAConfig(h, 0x80, PCD_SNG_BUF, 0x080);
  HAL_PCDEx_PMAConfig(h, CDC_IN_EP, PCD_SNG_BUF, 0x0C0);
  HAL_PCDEx_PMAConfig(h, CDC_OUT_EP, PCD_SNG_BUF, 0x100);
  HAL_PCDEx_PMAConfig(h, CDC_CMD_EP, PCD_SNG_BUF, 0x140);
  HAL_PCDEx_PMAConfig(h, MSC_EPIN_ADDR, PCD_SNG_BUF, 0x180);
  HAL_PCDEx_PMAConfig(h, MSC_EPOUT_ADDR, PCD_SNG_BUF, 0x1C0);
  return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_DeInit(USBD_HandleTypeDef *pdev)
{
  return status(HAL_PCD_DeInit(pdev->pData));
}

USBD_StatusTypeDef USBD_LL_Start(USBD_HandleTypeDef *pdev)
{
  return status(HAL_PCD_Start(pdev->pData));
}

USBD_StatusTypeDef USBD_LL_Stop(USBD_HandleTypeDef *pdev)
{
  return status(HAL_PCD_Stop(pdev->pData));
}

USBD_StatusTypeDef USBD_LL_OpenEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr, uint8_t ep_type, uint16_t ep_mps)
{
  return status(HAL_PCD_EP_Open(pdev->pData, ep_addr, ep_mps, ep_type));
}

USBD_StatusTypeDef USBD_LL_CloseEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
  return status(HAL_PCD_EP_Close(pdev->pData, ep_addr));
}

USBD_StatusTypeDef USBD_LL_FlushEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
  return status(HAL_PCD_EP_Flush(pdev->pData, ep_addr));
}

USBD_StatusTypeDef USBD_LL_StallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
  return status(HAL_PCD_EP_SetStall(pdev->pData, ep_addr));
}

USBD_StatusTypeDef USBD_LL_ClearStallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
  return status(HAL_PCD_EP_ClrStall(pdev->pData, ep_addr));
}

uint8_t USBD_LL_IsStallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
  PCD_HandleTypeDef *h = pdev->pData;
  if (ep_addr & 0x80U) {
    return h->IN_ep[ep_addr & 0x7FU].is_stall;
  }
  return h->OUT_ep[ep_addr & 0x7FU].is_stall;
}

USBD_StatusTypeDef USBD_LL_SetUSBAddress(USBD_HandleTypeDef *pdev, uint8_t dev_addr)
{
  return status(HAL_PCD_SetAddress(pdev->pData, dev_addr));
}

USBD_StatusTypeDef USBD_LL_Transmit(USBD_HandleTypeDef *pdev, uint8_t ep_addr, uint8_t *pbuf, uint32_t size)
{
  return status(HAL_PCD_EP_Transmit(pdev->pData, ep_addr, pbuf, size));
}

USBD_StatusTypeDef USBD_LL_PrepareReceive(USBD_HandleTypeDef *pdev, uint8_t ep_addr, uint8_t *pbuf, uint32_t size)
{
  return status(HAL_PCD_EP_Receive(pdev->pData, ep_addr, pbuf, size));
}

uint32_t USBD_LL_GetRxDataSize(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
  return HAL_PCD_EP_GetRxCount(pdev->pData, ep_addr);
}

void USBD_LL_Delay(uint32_t delay)
{
  HAL_Delay(delay);
}
