#ifndef USBD_CONF_H
#define USBD_CONF_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "stm32wbxx_hal.h"

/* Composite device: CDC (log/console) + MSC (SD card). */
#define USE_USBD_COMPOSITE
#define USBD_CMPSIT_ACTIVATE_CDC        1U
#define USBD_CMPSIT_ACTIVATE_MSC        1U
#define USBD_MAX_SUPPORTED_CLASS        2U
#define USBD_MAX_CLASS_ENDPOINTS        5U
#define USBD_MAX_CLASS_INTERFACES       5U
/* Interface Association Descriptor: groups the two CDC interfaces into one
 * function. Without it Windows creates the COM port but cannot open it. */
#define USBD_COMPOSITE_USE_IAD          1U

#define USBD_MAX_NUM_INTERFACES         3U
#define USBD_MAX_NUM_CONFIGURATION      1U
#define USBD_MAX_STR_DESC_SIZ           0x100U
#define USBD_SELF_POWERED               1U
#define USBD_DEBUG_LEVEL                0U
#define USBD_SUPPORT_USER_STRING_DESC   0U
#define USBD_CLASS_USER_STRING_DESC     0U
#define USBD_CLASS_BOS_ENABLED          0U
#define USBD_LPM_ENABLED                0U

/* Endpoints: CDC on EP1 (+ EP2 IN for notifications), MSC on EP3. */
#define CDC_IN_EP                       0x81U
#define CDC_OUT_EP                      0x01U
#define CDC_CMD_EP                      0x82U
#define MSC_EPIN_ADDR                   0x83U
#define MSC_EPOUT_ADDR                  0x03U

#define MSC_MEDIA_PACKET                2048U /* 4 sectors per transfer: multi-block SD commands over USB (RAM is tight) */
#define MSC_BOT_MAX_LUN                 1U
#define USBD_CDC_INTERVAL               2000U

#define DEVICE_FS                       0

#define USBD_malloc   (void *)USBD_static_malloc
#define USBD_free     USBD_static_free
#define USBD_memset   memset
#define USBD_memcpy   memcpy
#define USBD_Delay    HAL_Delay

#define USBD_UsrLog(...) do {} while (0)
#define USBD_ErrLog(...) do {} while (0)
#define USBD_DbgLog(...) do {} while (0)

void *USBD_static_malloc(uint32_t size);
void USBD_static_free(void *p);

#endif
