/*
 * USB composite device: CDC ACM (debug console) + MSC (SD card).
 *
 * The MSC callbacks run in the USB interrupt and access the SD card
 * directly; the SD card is only offered as drive while the firmware has the
 * file system unmounted (usb_msc_set_ready()).
 */
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "diskio.h"
#include "usb_device.h"
#include "usbd_cdc.h"
#include "usbd_composite_builder.h"
#include "usbd_core.h"
#include "usbd_ctlreq.h"
#include "usbd_msc.h"

#define USB_VID 0x1209 /* pid.codes */
#define USB_PID 0x0001 /* pid.codes test PID, private testing only */

static USBD_HandleTypeDef husb;
static bool started;
static uint8_t cdc_class_id;
static volatile bool cdc_dtr;
static volatile bool cdc_tx_busy;
static uint8_t cdc_rx_buf[CDC_DATA_FS_MAX_PACKET_SIZE];
static uint8_t cdc_tx_buf[256];
static volatile bool msc_ready;
static volatile uint8_t rx_ring[64];
static volatile uint8_t rx_head;
static volatile uint8_t rx_tail;
static volatile uint32_t msc_activity_tick;

/* ---- descriptors ---- */

static uint8_t desc_buf[USBD_MAX_STR_DESC_SIZ];

static uint8_t device_desc[USB_LEN_DEV_DESC] = {
  0x12, USB_DESC_TYPE_DEVICE, 0x00, 0x02,
  0xEF, 0x02, 0x01,          /* misc class / IAD: composite */
  USB_MAX_EP0_SIZE,
  LOBYTE(USB_VID), HIBYTE(USB_VID), LOBYTE(USB_PID), HIBYTE(USB_PID),
  0x05, 0x00,                /* bcdDevice 0.05 (new value forces Windows to re-read the IAD layout) */
  USBD_IDX_MFC_STR, USBD_IDX_PRODUCT_STR, USBD_IDX_SERIAL_STR,
  USBD_MAX_NUM_CONFIGURATION,
};

static uint8_t langid_desc[USB_LEN_LANGID_STR_DESC] = {
  USB_LEN_LANGID_STR_DESC, USB_DESC_TYPE_STRING, 0x09, 0x04,
};

static uint8_t *get_device_desc(USBD_SpeedTypeDef speed, uint16_t *length)
{
  (void)speed;
  *length = sizeof(device_desc);
  return device_desc;
}

static uint8_t *get_langid_desc(USBD_SpeedTypeDef speed, uint16_t *length)
{
  (void)speed;
  *length = sizeof(langid_desc);
  return langid_desc;
}

static uint8_t *string_desc(const char *s, uint16_t *length)
{
  USBD_GetString((uint8_t *)s, desc_buf, length);
  return desc_buf;
}

static uint8_t *get_mfc_desc(USBD_SpeedTypeDef speed, uint16_t *length)
{
  (void)speed;
  return string_desc("UltraBeep", length);
}

static uint8_t *get_product_desc(USBD_SpeedTypeDef speed, uint16_t *length)
{
  (void)speed;
  return string_desc("UltraBeep", length);
}

static uint8_t *get_serial_desc(USBD_SpeedTypeDef speed, uint16_t *length)
{
  (void)speed;
  char serial[25];
  board_serial_number(serial, sizeof(serial));
  return string_desc(serial, length);
}

static uint8_t *get_config_desc(USBD_SpeedTypeDef speed, uint16_t *length)
{
  (void)speed;
  return string_desc("CDC+MSC", length);
}

static uint8_t *get_interface_desc(USBD_SpeedTypeDef speed, uint16_t *length)
{
  (void)speed;
  return string_desc("UltraBeep", length);
}

static USBD_DescriptorsTypeDef fs_desc = {
  get_device_desc, get_langid_desc, get_mfc_desc, get_product_desc,
  get_serial_desc, get_config_desc, get_interface_desc,
};

/* ---- CDC ---- */

static int8_t cdc_init(void)
{
  USBD_CDC_SetTxBuffer(&husb, cdc_tx_buf, 0, cdc_class_id);
  USBD_CDC_SetRxBuffer(&husb, cdc_rx_buf);
  cdc_tx_busy = false;
  return USBD_OK;
}

static int8_t cdc_deinit(void)
{
  cdc_dtr = false;
  return USBD_OK;
}

static int8_t cdc_control(uint8_t cmd, uint8_t *pbuf, uint16_t length)
{
  static uint8_t line_coding[7] = {0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08}; /* 115200 8N1 */
  (void)length;
  switch (cmd) {
  case CDC_SET_LINE_CODING:
    memcpy(line_coding, pbuf, sizeof(line_coding));
    break;
  case CDC_GET_LINE_CODING:
    memcpy(pbuf, line_coding, sizeof(line_coding));
    break;
  case CDC_SET_CONTROL_LINE_STATE: {
    USBD_SetupReqTypedef *req = (USBD_SetupReqTypedef *)pbuf;
    cdc_dtr = (req->wValue & 0x0001U) != 0;
    break;
  }
  default:
    break;
  }
  return USBD_OK;
}

static int8_t cdc_receive(uint8_t *buf, uint32_t *len)
{
  for (uint32_t i = 0; i < *len; i++) {
    uint8_t next = (uint8_t)((rx_head + 1U) % sizeof(rx_ring));
    if (next != rx_tail) {
      rx_ring[rx_head] = buf[i];
      rx_head = next;
    }
  }
  USBD_CDC_SetRxBuffer(&husb, cdc_rx_buf);
  USBD_CDC_ReceivePacket(&husb);
  return USBD_OK;
}

static int8_t cdc_transmit_cplt(uint8_t *buf, uint32_t *len, uint8_t epnum)
{
  (void)buf;
  (void)len;
  (void)epnum;
  cdc_tx_busy = false;
  return USBD_OK;
}

static USBD_CDC_ItfTypeDef cdc_fops = {
  cdc_init, cdc_deinit, cdc_control, cdc_receive, cdc_transmit_cplt,
};

int usb_cdc_getc(void)
{
  if (rx_head == rx_tail) {
    return -1;
  }
  int c = rx_ring[rx_tail];
  rx_tail = (uint8_t)((rx_tail + 1U) % sizeof(rx_ring));
  return c;
}

bool usb_cdc_connected(void)
{
  return started && cdc_dtr && husb.dev_state == USBD_STATE_CONFIGURED;
}

void usb_cdc_write(const char *data, size_t len)
{
  while (len > 0 && usb_cdc_connected()) {
    uint32_t start = HAL_GetTick();
    while (cdc_tx_busy) {
      if (HAL_GetTick() - start > 50) {
        return; /* host not reading: drop */
      }
    }
    size_t chunk = len < sizeof(cdc_tx_buf) ? len : sizeof(cdc_tx_buf);
    memcpy(cdc_tx_buf, data, chunk);
    cdc_tx_busy = true;
    USBD_CDC_SetTxBuffer(&husb, cdc_tx_buf, chunk, cdc_class_id);
    if (USBD_CDC_TransmitPacket(&husb, cdc_class_id) != USBD_OK) {
      cdc_tx_busy = false;
      return;
    }
    data += chunk;
    len -= chunk;
  }
}

/* ---- MSC (SD card) ---- */

static int8_t inquiry_data[] = {
  0x00, 0x80, 0x02, 0x02, 36 - 5, 0x00, 0x00, 0x00,
  'U', 'B', 'R', 'e', 'l', 'o', 'a', 'd',                          /* vendor (8) */
  'U', 'l', 't', 'r', 'a', 'B', 'i', 'p', ' ', 'S', 'D', ' ', 'c', 'a', 'r', 'd', /* product (16) */
  '0', '.', '1', '0',                                              /* revision (4) */
};

static int8_t msc_init(uint8_t lun)
{
  (void)lun;
  return 0;
}

static int8_t msc_capacity(uint8_t lun, uint32_t *block_num, uint16_t *block_size)
{
  (void)lun;
  DWORD sectors = 0;
  if (!msc_ready || disk_ioctl(0, GET_SECTOR_COUNT, &sectors) != RES_OK) {
    return -1;
  }
  *block_num = sectors;
  *block_size = 512;
  return 0;
}

static int8_t msc_is_ready(uint8_t lun)
{
  (void)lun;
  return msc_ready ? 0 : -1;
}

static int8_t msc_is_write_protected(uint8_t lun)
{
  (void)lun;
  return 0;
}

static int8_t msc_read(uint8_t lun, uint8_t *buf, uint32_t blk_addr, uint16_t blk_len)
{
  (void)lun;
  msc_activity_tick = HAL_GetTick();
  return disk_read(0, buf, blk_addr, blk_len) == RES_OK ? 0 : -1;
}

static int8_t msc_write(uint8_t lun, uint8_t *buf, uint32_t blk_addr, uint16_t blk_len)
{
  (void)lun;
  msc_activity_tick = HAL_GetTick();
  return disk_write(0, buf, blk_addr, blk_len) == RES_OK ? 0 : -1;
}

static int8_t msc_max_lun(void)
{
  return 0;
}

static USBD_StorageTypeDef msc_fops = {
  msc_init, msc_capacity, msc_is_ready, msc_is_write_protected,
  msc_read, msc_write, msc_max_lun, inquiry_data,
};

/* ---- device control ---- */

static void usb_clock_init(void)
{
  RCC_OscInitTypeDef osc = {0};
  osc.OscillatorType = RCC_OSCILLATORTYPE_HSI48;
  osc.HSI48State = RCC_HSI48_ON;
  osc.PLL.PLLState = RCC_PLL_NONE;
  HAL_RCC_OscConfig(&osc);

  RCC_PeriphCLKInitTypeDef pclk = {0};
  pclk.PeriphClockSelection = RCC_PERIPHCLK_USB;
  pclk.UsbClockSelection = RCC_USBCLKSOURCE_HSI48;
  HAL_RCCEx_PeriphCLKConfig(&pclk);

  /* Trim HSI48 to the USB start-of-frame. */
  BOARD_ATOMIC(__HAL_RCC_CRS_CLK_ENABLE());
  RCC_CRSInitTypeDef crs = {0};
  crs.Prescaler = RCC_CRS_SYNC_DIV1;
  crs.Source = RCC_CRS_SYNC_SOURCE_USB;
  crs.Polarity = RCC_CRS_SYNC_POLARITY_RISING;
  crs.ReloadValue = __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000, 1000);
  crs.ErrorLimitValue = RCC_CRS_ERRORLIMIT_DEFAULT;
  crs.HSI48CalibrationValue = RCC_CRS_HSI48CALIBRATION_DEFAULT;
  HAL_RCCEx_CRSConfig(&crs);

  HAL_PWREx_EnableVddUSB();
}

bool usb_start(bool with_msc)
{
  static uint8_t cdc_ep[] = {CDC_IN_EP, CDC_OUT_EP, CDC_CMD_EP};
  static uint8_t msc_ep[] = {MSC_EPIN_ADDR, MSC_EPOUT_ADDR};

  if (started) {
    return true;
  }
  msc_ready = with_msc && disk_initialize(0) == 0;
  usb_clock_init();

  if (USBD_Init(&husb, &fs_desc, DEVICE_FS) != USBD_OK) {
    return false;
  }
  if (USBD_RegisterClassComposite(&husb, USBD_CDC_CLASS, CLASS_TYPE_CDC, cdc_ep) != USBD_OK ||
      USBD_RegisterClassComposite(&husb, USBD_MSC_CLASS, CLASS_TYPE_MSC, msc_ep) != USBD_OK) {
    return false;
  }
  cdc_class_id = (uint8_t)USBD_CMPSIT_GetClassID(&husb, CLASS_TYPE_CDC, 0);
  if (USBD_CMPSIT_SetClassID(&husb, CLASS_TYPE_CDC, 0) != 0xFFU) {
    USBD_CDC_RegisterInterface(&husb, &cdc_fops);
  }
  if (USBD_CMPSIT_SetClassID(&husb, CLASS_TYPE_MSC, 0) != 0xFFU) {
    USBD_MSC_RegisterStorage(&husb, &msc_fops);
  }
  started = USBD_Start(&husb) == USBD_OK;
  return started;
}

void usb_stop(void)
{
  if (!started) {
    return;
  }
  msc_ready = false;
  USBD_Stop(&husb);
  USBD_DeInit(&husb);
  /* The composite builder keeps the configuration descriptor in static
   * storage; without clearing it a second session would append to it. */
  USBD_CMPST_ClearConfDesc(&husb);
  started = false;
  cdc_dtr = false;
}

void usb_msc_set_ready(bool ready)
{
  if (!started) {
    return;
  }
  msc_ready = ready && disk_initialize(0) == 0;
}

bool usb_is_started(void)
{
  return started;
}

/* Starts USB (COM port, SD card hidden) when the cable is plugged, stops it
 * when unplugged. Non-blocking; called regularly by the console task. */
void usb_follow_cable(void)
{
  bool vbus = HAL_GPIO_ReadPin(VBUS_PORT, VBUS_PIN) == GPIO_PIN_SET;
  if (vbus && !started) {
    usb_start(false);
  } else if (!vbus && started) {
    usb_stop();
  }
}

bool usb_is_configured(void)
{
  return started && husb.dev_state == USBD_STATE_CONFIGURED;
}

uint32_t usb_msc_idle_ms(void)
{
  return HAL_GetTick() - msc_activity_tick;
}
