#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "stm32wbxx_hal.h"

/* ---- pin map ---- */
#define BTN_PORT     GPIOC
#define BTN_PIN      GPIO_PIN_13 /* active low, wake-up pin WKUP2 */
#define VBUS_PORT    GPIOC
#define VBUS_PIN     GPIO_PIN_5  /* high while USB power is present, wake-up pin WKUP5 */
#define CHG_PORT     GPIOE
#define CHG_PIN      GPIO_PIN_4  /* charger status, low while USB power is present */

/* Red and green are active high, blue is active low (stock firmware: blue on = PB1 low). */
#define LED_R_PORT   GPIOB
#define LED_R_PIN    GPIO_PIN_2
#define LED_G_PORT   GPIOB
#define LED_G_PIN    GPIO_PIN_0
#define LED_B_PORT   GPIOB
#define LED_B_PIN    GPIO_PIN_1

#define SD_PWR_PORT  GPIOC
#define SD_PWR_PIN   GPIO_PIN_14 /* active low */
#define SD_CS_PORT   GPIOD
#define SD_CS_PIN    GPIO_PIN_0
#define SD_SCK_PORT  GPIOD
#define SD_SCK_PIN   GPIO_PIN_1  /* SPI2_SCK, AF5 */
#define SD_MISO_PORT GPIOC
#define SD_MISO_PIN  GPIO_PIN_2  /* SPI2_MISO, AF5 */
#define SD_MOSI_PORT GPIOC
#define SD_MOSI_PIN  GPIO_PIN_3  /* SPI2_MOSI, AF5 */

#define GPS_PWR_PORT GPIOC
#define GPS_PWR_PIN  GPIO_PIN_6  /* GNSS load switch, active low */
#define GPS_TX_PORT  GPIOA
#define GPS_TX_PIN   GPIO_PIN_9  /* USART1_TX, AF7 */
#define GPS_RX_PORT  GPIOB
#define GPS_RX_PIN   GPIO_PIN_7  /* USART1_RX, AF7 */

#define AMP_PORT     GPIOB
#define AMP_PIN      GPIO_PIN_12 /* speaker amplifier enable, high = on */
/* Sensor supply enable, high = on: the stock firmware drives it high before it talks to the IMU and
 * the barometer (10 ms settle) and low before standby. Unconnected on board v1.0 (bootloader BL_v3);
 * on v1.1 (BL_v4) the IMU answers only after this supply cycle at boot. */
#define SENS_PWR_PORT GPIOC
#define SENS_PWR_PIN  GPIO_PIN_15

/* ICM-42605 INT1 (the stock firmware: EXTI falling edge with pull-up, data ready). Not used: the
 * sensor task reads the IMU every 5 ms, which serves the 100 Hz output data rate as well. */
#define IMU_INT_PORT  GPIOC
#define IMU_INT_PIN   GPIO_PIN_12

/* Mezzanine (GNSS + speaker board) detect: the stock reads it once with pull-up and pull-down while
 * the amplifier is on (driven low = UltraBip with mezzanine, open = BlueBip) and releases it. */
#define MEZZ_PORT     GPIOB
#define MEZZ_PIN      GPIO_PIN_9

/* Audio output select (FSA221 switch; the pin doubles as BOOT0, which the option bytes ignore):
 * high = the USB-C lines carry the headphone audio, low or released = USB data and the speaker. */
#define AUDIO_SEL_PORT GPIOH
#define AUDIO_SEL_PIN  GPIO_PIN_3

typedef enum {
  LED_OFF = 0,
  LED_RED = 1,
  LED_GREEN = 2,
  LED_YELLOW = 3,
  LED_BLUE = 4,
  LED_MAGENTA = 5,
  LED_CYAN = 6,
  LED_WHITE = 7,
} led_color_t;

/*
 * Port and clock registers are read-modify-write in the HAL: two tasks configuring pins of the
 * same port at the same time (the GPS task PC6, the sensor task PC10/PC11, the SD card PC14) can
 * undo each other's work - a chip-select left floating lets the IMU listen to the barometer's
 * traffic. So every pin and clock initialisation runs with interrupts off (a few microseconds).
 */
static inline uint32_t board_irq_lock(void)
{
  uint32_t pm = __get_PRIMASK();
  __disable_irq();
  return pm;
}
static inline void board_irq_unlock(uint32_t pm)
{
  __set_PRIMASK(pm);
}
#define BOARD_ATOMIC(block)            \
  do {                                 \
    uint32_t pm_ = board_irq_lock();   \
    block;                             \
    board_irq_unlock(pm_);             \
  } while (0)
void board_gpio_init(GPIO_TypeDef *port, const GPIO_InitTypeDef *init);
void board_gpio_deinit(GPIO_TypeDef *port, uint32_t pins);

/*
 * The device's serial number as the stock firmware forms it: the 64-bit unique device id
 * (UID64: company id 0x0080E1, device id, unique number) as 16 hex digits, high word first,
 * e.g. 0080E12600A74360. The stock writes it to system_serialnumber_string, reports it over
 * Bluetooth and names its IGC files after the last four digits. out: at least 17 bytes.
 */
void board_serial_number(char *out, size_t size);

void board_init(void);
/* Sensor supply (PC15): on with 10 ms settling, or off; the caller handles the bus lines. */
void board_sensor_power(bool on);
/* Audio output: headphones on the USB-C lines (AUDIO_SEL high) or the speaker (low). */
void board_audio_headphones(bool on);
void SystemClock_Config(void);
/* HSE load capacitor tuning in use (set like the stock firmware). */
uint32_t clock_hse_tune(void);

/* LEDs are dimmed by software PWM; brightness in 1/32 steps (full brightness dazzles). */
#define LED_BRIGHTNESS_DEFAULT 2U
void led_set(led_color_t color);
void led_set_brightness(uint8_t duty_of_32);
/* Bypasses the dimming (fault handlers, early boot without interrupts). */
void led_force(led_color_t color);

bool button_pressed(void);
bool vbus_present(void);
/* Charger status line PE4: true while it is low. */
bool charger_line_low(void);

void board_gps_power(bool on);
void board_amp_enable(bool on);

/* Delay: vTaskDelay once the scheduler runs, busy wait (watchdog fed) before. */
void board_delay_ms(uint32_t ms);

void watchdog_start(void);
void watchdog_feed(void);

#endif
