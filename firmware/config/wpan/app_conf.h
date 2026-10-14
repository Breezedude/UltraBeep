/* Configuration for the STM32_WPAN transport layer used by hwcheck. */
#ifndef APP_CONF_H
#define APP_CONF_H

#include "stm32wbxx.h"
#include "stm32wbxx_ll_bus.h"
#include "stm32wbxx_ll_exti.h"
#include "stm32wbxx_ll_ipcc.h"
#include "stm32wbxx_ll_pwr.h"
#include "stm32wbxx_ll_rcc.h"

/* Number of BLE events the mailbox can hold before CPU1 has processed them: a burst of
 * writes without response within one connection event (about 270 bytes each). */
#define CFG_TLBLE_EVT_QUEUE_LENGTH        8
#define CFG_TLBLE_MOST_EVENT_PAYLOAD_SIZE 255
#define TL_BLE_EVENT_FRAME_SIZE           (TL_EVT_HDR_SIZE + CFG_TLBLE_MOST_EVENT_PAYLOAD_SIZE)

/* HW_IPCC_* prototypes for hw_ipcc.c */
#include "hw.h"

#endif
