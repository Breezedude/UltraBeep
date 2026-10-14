#include "dps310.h"

#include <stdint.h>

#include "board.h"
#include "sensor_bus.h"

#define REG_PSR_B2   0x00
#define REG_TMP_B2   0x03
#define REG_PRS_CFG  0x06
#define REG_TMP_CFG  0x07
#define REG_MEAS_CFG 0x08
#define REG_CFG_REG  0x09
#define REG_RESET    0x0C
#define REG_PROD_ID  0x0D
#define REG_COEF     0x10
#define REG_COEF_SRC 0x28

#define MEAS_PRS_RDY  0x10
#define MEAS_TMP_RDY  0x20
#define MEAS_SENS_RDY 0x40
#define MEAS_COEF_RDY 0x80

#define T_ALPHA 0.2f  /* temperature low-pass at 1 Hz: time constant 5 s */
#define CMD_PRESSURE    0x01
#define CMD_TEMPERATURE 0x02
#define SLOT_MS         10U  /* 100 Hz mode: one measurement per 10 ms */
#define TEMP_EVERY      100U /* ... a temperature measurement instead of every 100th pressure one */

/* compensation scale factors per oversampling rate (datasheet) */
#define K_1X  524288.0f
#define K_4X  3670016.0f
#define K_8X  7864320.0f
#define K_16X 253952.0f

typedef struct {
  int32_t c0, c1, c00, c10, c01, c11, c20, c21, c30;
} coef_t;

static coef_t k;
static float t_scaled; /* low-pass filtered: its noise would enter the pressure as 1 Hz steps */
static int32_t t_raw;
static bool have_t;
static bool running;
static dps310_mode_t mode = DPS310_MODE_100;
static float kp = K_4X, kt = K_1X;
/* command mode (100 Hz) */
static uint8_t cmd;          /* measurement in progress: CMD_*, 0 = none */
static uint32_t slot_start;  /* tick of the current 10 ms slot */
static unsigned since_temp;  /* pressure measurements since the last temperature one */

static int32_t sign_extend(uint32_t v, int bits)
{
  uint32_t m = 1U << (bits - 1);
  return (int32_t)((v ^ m) - m);
}

static bool raw24(uint8_t reg, int32_t *out)
{
  uint8_t b[3];
  if (!sensor_bus_read(SENSOR_BARO, reg, b, 3)) {
    return false;
  }
  *out = sign_extend(((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2], 24);
  return true;
}

static uint8_t prod_id;

unsigned char dps310_id(void)
{
  return prod_id;
}

unsigned dps310_rate_hz(void)
{
  return (unsigned)mode;
}

float dps310_kp(void)
{
  return kp;
}

float dps310_kt(void)
{
  return kt;
}

void dps310_scale_factors(unsigned rate_hz, float *p, float *t)
{
  *p = rate_hz == 32U ? K_16X : rate_hz == 64U ? K_8X : K_4X;
  *t = rate_hz == 32U ? K_8X : K_1X;
}

bool dps310_start(dps310_mode_t m)
{
  running = false;
  have_t = false;
  mode = m == DPS310_MODE_32 || m == DPS310_MODE_64 ? m : DPS310_MODE_100;
  sensor_bus_init();
  /* After power-up the chip listens on I2C; the first chip select switches it
   * to SPI and that transfer is lost. */
  for (int i = 0; i < 3; i++) {
    prod_id = sensor_bus_read1(SENSOR_BARO, REG_PROD_ID);
    if (prod_id == 0x10) {
      break;
    }
    board_delay_ms(2);
  }
  if (prod_id != 0x10) {
    return false;
  }
  sensor_bus_write(SENSOR_BARO, REG_RESET, 0x09); /* soft reset */
  board_delay_ms(50);
  uint32_t start = HAL_GetTick();
  while ((sensor_bus_read1(SENSOR_BARO, REG_MEAS_CFG) & (MEAS_COEF_RDY | MEAS_SENS_RDY)) !=
         (MEAS_COEF_RDY | MEAS_SENS_RDY)) {
    if (HAL_GetTick() - start > 500U) {
      return false;
    }
    board_delay_ms(5);
  }

  uint8_t c[18];
  if (!sensor_bus_read(SENSOR_BARO, REG_COEF, c, sizeof(c))) {
    return false;
  }
  k.c0 = sign_extend(((uint32_t)c[0] << 4) | (c[1] >> 4), 12);
  k.c1 = sign_extend(((uint32_t)(c[1] & 0x0F) << 8) | c[2], 12);
  k.c00 = sign_extend(((uint32_t)c[3] << 12) | ((uint32_t)c[4] << 4) | (c[5] >> 4), 20);
  k.c10 = sign_extend(((uint32_t)(c[5] & 0x0F) << 16) | ((uint32_t)c[6] << 8) | c[7], 20);
  k.c01 = sign_extend(((uint32_t)c[8] << 8) | c[9], 16);
  k.c11 = sign_extend(((uint32_t)c[10] << 8) | c[11], 16);
  k.c20 = sign_extend(((uint32_t)c[12] << 8) | c[13], 16);
  k.c21 = sign_extend(((uint32_t)c[14] << 8) | c[15], 16);
  k.c30 = sign_extend(((uint32_t)c[16] << 8) | c[17], 16);
  uint8_t tmp_src = sensor_bus_read1(SENSOR_BARO, REG_COEF_SRC) & 0x80U;

  /* Temperature sensor fix from the Infineon reference driver. */
  sensor_bus_write(SENSOR_BARO, 0x0E, 0xA5);
  sensor_bus_write(SENSOR_BARO, 0x0F, 0x96);
  sensor_bus_write(SENSOR_BARO, 0x62, 0x02);
  sensor_bus_write(SENSOR_BARO, 0x0E, 0x00);
  sensor_bus_write(SENSOR_BARO, 0x0F, 0x00);

  if (mode == DPS310_MODE_32) {
    sensor_bus_write(SENSOR_BARO, REG_PRS_CFG, 0x54);          /* pressure: 32 Hz, 16x (27.6 ms each) */
    sensor_bus_write(SENSOR_BARO, REG_TMP_CFG, tmp_src | 0x03); /* temperature: 1 Hz, 8x (14.8 ms) */
    sensor_bus_write(SENSOR_BARO, REG_CFG_REG, 0x04);           /* P_SHIFT (oversampling > 8) */
    kp = K_16X;
    kt = K_8X;
    running = sensor_bus_write(SENSOR_BARO, REG_MEAS_CFG, 0x07); /* continuous P + T */
  } else if (mode == DPS310_MODE_64) {
    sensor_bus_write(SENSOR_BARO, REG_PRS_CFG, 0x63);          /* pressure: 64 Hz, 8x (14.8 ms): 947 ms/s */
    sensor_bus_write(SENSOR_BARO, REG_TMP_CFG, tmp_src | 0x00); /* temperature: 1 Hz, single (3.6 ms) */
    sensor_bus_write(SENSOR_BARO, REG_CFG_REG, 0x00);
    kp = K_8X;
    kt = K_1X;
    running = sensor_bus_write(SENSOR_BARO, REG_MEAS_CFG, 0x07);
  } else {
    sensor_bus_write(SENSOR_BARO, REG_PRS_CFG, 0x02);          /* pressure: 4x (8.4 ms), started by us */
    sensor_bus_write(SENSOR_BARO, REG_TMP_CFG, tmp_src | 0x00); /* temperature: single (3.6 ms) */
    sensor_bus_write(SENSOR_BARO, REG_CFG_REG, 0x00);
    kp = K_4X;
    kt = K_1X;
    /* a temperature first: the pressure compensation needs it */
    running = sensor_bus_write(SENSOR_BARO, REG_MEAS_CFG, CMD_TEMPERATURE);
    cmd = CMD_TEMPERATURE;
    slot_start = HAL_GetTick();
    since_temp = 0;
  }
  return running;
}

static void take_temperature(int32_t raw)
{
  float t = (float)raw / kt;
  t_scaled = have_t ? t_scaled + T_ALPHA * (t - t_scaled) : t;
  t_raw = raw;
  have_t = true;
}

static void compensate(int32_t raw, dps310_sample_t *s)
{
  float p = (float)raw / kp;
  float t = t_scaled;
  s->pressure_pa = (float)k.c00 + p * ((float)k.c10 + p * ((float)k.c20 + p * (float)k.c30)) + t * (float)k.c01 +
                   t * p * ((float)k.c11 + p * (float)k.c21);
  s->temp_c = (float)k.c0 * 0.5f + (float)k.c1 * t;
  s->p_raw = raw;
  s->t_raw = t_raw;
}

/*
 * 100 Hz: the result of the measurement started in the last slot, then the next one at the start
 * of the next 10 ms slot (on the slot grid, so the rate does not drift with the polling).
 */
static bool read_command_mode(dps310_sample_t *s)
{
  uint32_t now = HAL_GetTick();
  uint8_t meas = sensor_bus_read1(SENSOR_BARO, REG_MEAS_CFG);
  int32_t raw;
  bool got = false;
  if (cmd == CMD_TEMPERATURE && (meas & MEAS_TMP_RDY)) {
    if (raw24(REG_TMP_B2, &raw)) {
      take_temperature(raw);
    }
    cmd = 0;
  } else if (cmd == CMD_PRESSURE && (meas & MEAS_PRS_RDY)) {
    if (have_t && raw24(REG_PSR_B2, &raw)) {
      compensate(raw, s);
      got = true;
    }
    cmd = 0;
  } else if (cmd && now - slot_start > 5U * SLOT_MS) {
    cmd = 0; /* no result: start again */
  }
  if (!cmd && now - slot_start >= SLOT_MS) {
    slot_start = now - slot_start < 2U * SLOT_MS ? slot_start + SLOT_MS : now;
    if (!have_t || since_temp >= TEMP_EVERY - 1U) {
      cmd = CMD_TEMPERATURE;
      since_temp = 0;
    } else {
      cmd = CMD_PRESSURE;
      since_temp++;
    }
    sensor_bus_write(SENSOR_BARO, REG_MEAS_CFG, cmd);
  }
  return got;
}

bool dps310_read(dps310_sample_t *s)
{
  if (!running) {
    return false;
  }
  if (mode == DPS310_MODE_100) {
    return read_command_mode(s);
  }
  uint8_t meas = sensor_bus_read1(SENSOR_BARO, REG_MEAS_CFG);
  int32_t raw;
  if ((meas & MEAS_TMP_RDY) && raw24(REG_TMP_B2, &raw)) {
    take_temperature(raw);
  }
  if (!(meas & MEAS_PRS_RDY) || !have_t || !raw24(REG_PSR_B2, &raw)) {
    return false;
  }
  compensate(raw, s);
  return true;
}

void dps310_coefficients(int32_t out[DPS310_COEFS])
{
  const int32_t c[DPS310_COEFS] = {k.c0, k.c1, k.c00, k.c10, k.c01, k.c11, k.c20, k.c21, k.c30};
  for (int i = 0; i < DPS310_COEFS; i++) {
    out[i] = c[i];
  }
}

void dps310_sleep(void)
{
  sensor_bus_write(SENSOR_BARO, REG_MEAS_CFG, 0x00); /* standby */
  running = false;
}
