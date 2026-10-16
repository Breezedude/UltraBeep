#include "sensors.h"

#include <math.h>

#include "FreeRTOS.h"
#include "task.h"

#include "board.h"
#include "dps310.h"
#include "fusion.h"
#include "health.h"
#include "icm42605.h"
#include "log.h"
#include "rawlog.h"
#include "sensor_bus.h"
#include "tap.h"
#include "vario.h"
#include "vario_audio.h"

#define TICK_MS       5U     /* polls the barometer (up to 100 Hz) and the 100 Hz IMU without missing samples */
#define SETTLE_S      2U     /* barometer samples of this long before the values count */
#define CAL_BLOCK     100U   /* IMU samples per calibration mean (1 s) */
#define CAL_ACC_SD    0.05f  /* still: accelerometer standard deviation per axis below [m/s²] */
#define CAL_GYRO_SPAN 3.0f   /* ... and gyro span per axis below [°/s] */
#define ACCEL_ALPHA   0.02f  /* 100 Hz, time constant 0.5 s */
#define FAST_ALPHA    0.1f   /* 100 Hz, time constant 0.1 s (manoeuvre detection) */
#define YAW_ALPHA   (1.0f / ICM42605_RATE_HZ) /* 1 s mean of the yaw rate for the thermal detector */
#define DIR_ALPHA   0.02f /* direction of the specific force, 0.5 s mean (the analyzer's yawRate) */
#define TURN_ALPHA  0.02f /* 0.5 s mean of the signed turn rate */
#define G             9.80665f
#define RETRY_MS      2000U  /* a sensor that did not answer is tried again */

static TaskHandle_t task;
static volatile bool want_on;
static vario_filter_t filter; /* barometer only: fallback without IMU */
static tap_t tap;
static volatile int tap_sensitivity = TAP_SENSITIVITY_DEFAULT;
static void (*volatile tap_handler)(void);
static volatile float last_tap_g;
static volatile bool handling; /* set by sensors_handling, taken by the sensor task */
static fusion_t fusion;        /* barometer + IMU */
static sensors_t data;       /* written by the task, copied under a critical section */
static float motion_max_g, motion_min_g = 99.0f, motion_rate_dps; /* since sensors_take_motion */
static uint32_t motion_samples;
static float ref_offset;     /* GNSS-referenced altitude = standard altitude + ref_offset */
static volatile bool ref_request;
static volatile float ref_alt;
static volatile bool track_request;
static volatile float track_alt;
#define TRACK_ALPHA (1.0f / 600.0f) /* 1 Hz GNSS fixes: 10 min time constant */
static volatile bool monitor;
static volatile unsigned baro_rate = DPS310_MODE_100;
static volatile bool restart_request;
static uint32_t last_baro_tick;
static float acc_off[3] = {0.0f, 0.0f, 0.0f};
static float acc_scale[3] = {1.0f, 1.0f, 1.0f};
/* calibration means (sensor task) */
static double cal_sum[3], cal_sum2[3];
static float cal_gmin[3], cal_gmax[3];
static unsigned cal_n;
static sensors_cal_sample_t cal_out;

static float isa_altitude(float pressure_pa)
{
  return 44330.77f * (1.0f - powf(pressure_pa / 101325.0f, 0.190263f));
}

/*
 * Starts the sensors that do not run yet; logs only changes. From off, as the stock does at every
 * sensor start: bus lines first, then the supply on and 10 ms, so both chips start from reset.
 */
static void start_hw(void)
{
  if (!data.on) {
    sensor_bus_init();
    board_sensor_power(true);
  }
  icm42605_quiet(); /* the IMU listens on the shared lines as an I2C device until told not to */
  bool baro = data.baro_ok || dps310_start((dps310_mode_t)baro_rate);
  bool imu = data.imu_ok || icm42605_start();
  if (baro != data.baro_ok || imu != data.imu_ok || !data.on) {
    log_printf("Sensors: barometer %s (id %02X), IMU %s (id %02X)", baro ? "ok" : "NO ANSWER", dps310_id(),
               imu ? "ok" : "NO ANSWER", icm42605_id());
  }
  if (baro && !data.baro_ok) {
    /* the altitude variance is per sample at 32 Hz: the same weight per second at other rates */
    vario_filter_init(&filter, VARIO_FILTER_Q, VARIO_FILTER_R * (float)dps310_rate_hz() / 32.0f);
    last_baro_tick = 0;
  }
  if (imu && !data.imu_ok) {
    fusion_init(&fusion);
  }
  fusion_set_baro_rate(&fusion, (float)dps310_rate_hz());
  taskENTER_CRITICAL();
  data.on = true;
  data.baro_ok = baro;
  data.imu_ok = imu;
  data.baro_id = dps310_id();
  data.imu_id = icm42605_id();
  taskEXIT_CRITICAL();
}

/* As the stock: chips asleep, supply off, bus lines released, 10 ms. */
static void stop_hw(void)
{
  vario_audio_release();
  dps310_sleep();
  icm42605_sleep();
  board_sensor_power(false);
  sensor_bus_release();
  taskENTER_CRITICAL();
  data = (sensors_t){0};
  taskEXIT_CRITICAL();
}

static void sample_baro(void)
{
  dps310_sample_t s;
  if (!dps310_read(&s)) {
    return;
  }
  float p = s.pressure_pa;
  float t = s.temp_c;
  float alt = isa_altitude(p);
  /* the measured interval: the chip's own clock runs a few percent off in the continuous modes */
  uint32_t now = HAL_GetTick();
  float dt = last_baro_tick ? (float)(now - last_baro_tick) / 1000.0f : 1.0f / (float)dps310_rate_hz();
  last_baro_tick = now;
  dt = dt < 0.005f ? 0.005f : dt > 0.1f ? 0.1f : dt;
  vario_filter_update(&filter, alt, dt);
  bool fused = data.imu_ok && fusion.attitude_ok;
  if (fused) {
    fusion_baro(&fusion, alt);
    /* spiral mode in the raw log: the analyzer shows the events */
    static bool spiral_logged;
    bool spiral = fusion.mode == FUSION_MODE_SPIRAL;
    if (spiral != spiral_logged) {
      rawlog_event("spiral mode %s", spiral ? "on" : "off");
      spiral_logged = spiral;
    }
  }
  float h = fused ? fusion.h : filter.h;
  float v = fused ? fusion.vz : filter.v;
  rawlog_baro(&s, h, v);
  if (ref_request) {
    ref_offset = ref_alt - h;
    ref_request = false;
    log_printf("Sensors: altitude referenced to GNSS %.1f m (offset %.1f m)", (double)ref_alt, (double)ref_offset);
    taskENTER_CRITICAL();
    data.referenced = true;
    taskEXIT_CRITICAL();
  } else if (track_request) {
    /* weather drift: the barometric altitude slowly follows the GNSS altitude (a bike ride
     * showed 20 m in 90 min); far too slow to show in the climb rate */
    if (data.referenced) {
      ref_offset += TRACK_ALPHA * (track_alt - h - ref_offset);
    }
    track_request = false;
  }
  taskENTER_CRITICAL();
  data.pressure_pa = p;
  data.temp_c = t;
  data.alt_std_m = alt;
  data.alt_raw_m = alt + ref_offset;
  data.alt_m = h + ref_offset;
  data.vario_ms = v;
  data.fused = fused;
  data.at_rest = fused && fusion.at_rest;
  data.samples++;
  data.valid = data.samples >= SETTLE_S * dps310_rate_hz();
  taskEXIT_CRITICAL();
  if (monitor && data.samples % dps310_rate_hz() == 0U) {
    log_printf("p %.1f Pa  T %.2f C  alt %.2f m  vario %+.2f m/s (baro only %+.2f)  a %.2f m/s2%s", (double)p,
               (double)t, (double)data.alt_m, (double)v, (double)filter.v, (double)data.accel_ms2,
               data.at_rest ? "  rest" : "");
  }
}

static void sample_imu(void)
{
  int16_t raw[6];
  if (!icm42605_read(raw)) {
    return;
  }
  rawlog_imu(raw);
  const float ka = G / ICM42605_ACCEL_LSB_G;
  const float kg = 0.017453293f / ICM42605_GYRO_LSB_DPS;
  float a[3], w[3];
  for (int k = 0; k < 3; k++) {
    float ar = raw[k] * ka;
    float gd = raw[3 + k] / ICM42605_GYRO_LSB_DPS;
    a[k] = (ar - acc_off[k]) * acc_scale[k];
    w[k] = raw[3 + k] * kg;
    /* calibration means: raw accelerometer, gyro span */
    cal_sum[k] += ar;
    cal_sum2[k] += (double)ar * ar;
    if (cal_n == 0U || gd < cal_gmin[k]) {
      cal_gmin[k] = gd;
    }
    if (cal_n == 0U || gd > cal_gmax[k]) {
      cal_gmax[k] = gd;
    }
  }
  if (++cal_n >= CAL_BLOCK) {
    sensors_cal_sample_t c;
    c.still = true;
    for (int k = 0; k < 3; k++) {
      double m = cal_sum[k] / cal_n;
      double var = cal_sum2[k] / cal_n - m * m;
      c.mean[k] = (float)m;
      if (var > (double)(CAL_ACC_SD * CAL_ACC_SD) || cal_gmax[k] - cal_gmin[k] > CAL_GYRO_SPAN) {
        c.still = false;
      }
      cal_sum[k] = 0.0;
      cal_sum2[k] = 0.0;
    }
    cal_n = 0;
    taskENTER_CRITICAL();
    c.seq = cal_out.seq + 1U;
    cal_out = c;
    taskEXIT_CRITICAL();
  }
  fusion_imu(&fusion, w, a, 1.0f / ICM42605_RATE_HZ);
  float yaw = fusion_yaw_rate(&fusion, w) * 57.29578f;

  if (tap.threshold == 0.0f) {
    tap_init(&tap, tap_sensitivity);
  }
  tap_set_sensitivity(&tap, tap_sensitivity);
  const float ag[3] = {a[0] / G, a[1] / G, a[2] / G};
  bool double_tap = tap_update(&tap, ag, sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) * 57.29578f);
  if (handling) {
    handling = false;
    fusion_hold_baro(&fusion, FUSION_HOLD_S);
  }
  if (tap.tapped) {
    tap.tapped = false;
    fusion_hold_baro(&fusion, FUSION_HOLD_S); /* knocks come with pressure pulses (door slams) */
    last_tap_g = tap.tap_jerk;
    rawlog_event("tap %.2f g (threshold %.2f g)", (double)tap.tap_jerk, (double)tap.tap_threshold);
  }
  if (double_tap && tap_handler) {
    tap_handler();
  }

  float m = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
  float rate = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) * 57.29578f;
  /* the turn's direction: the rotation about the (slowly followed) direction of the specific force, which
   * points up through the pilot in a coordinated turn; clockwise seen from above is positive */
  static float adir[3] = {0.0f, 0.0f, 1.0f};
  if (m > 0.0f) {
    for (int k = 0; k < 3; k++) {
      adir[k] += DIR_ALPHA * (a[k] / m - adir[k]);
    }
  }
  float along = w[0] * adir[0] + w[1] * adir[1] + w[2] * adir[2];
  float turn = along < 0.0f ? rate : -rate;
  taskENTER_CRITICAL();
  data.accel_ms2 = data.accel_ms2 == 0.0f ? m : data.accel_ms2 + ACCEL_ALPHA * (m - data.accel_ms2);
  data.accel_fast_ms2 = data.accel_fast_ms2 == 0.0f ? m : data.accel_fast_ms2 + FAST_ALPHA * (m - data.accel_fast_ms2);
  data.rate_dps += FAST_ALPHA * (rate - data.rate_dps);
  data.turn_dps += TURN_ALPHA * (turn - data.turn_dps);
  data.yaw_dps += YAW_ALPHA * (yaw - data.yaw_dps);
  data.yaw_ok = fusion.attitude_ok;
  float mg = m / G;
  motion_max_g = mg > motion_max_g ? mg : motion_max_g;
  motion_min_g = mg < motion_min_g ? mg : motion_min_g;
  motion_rate_dps = rate > motion_rate_dps ? rate : motion_rate_dps;
  motion_samples++;
  taskEXIT_CRITICAL();
}

bool sensors_take_motion(float *acc_max_g, float *acc_min_g, float *rate_max_dps)
{
  taskENTER_CRITICAL();
  bool any = motion_samples != 0U;
  *acc_max_g = motion_max_g;
  *acc_min_g = any ? motion_min_g : 0.0f;
  *rate_max_dps = motion_rate_dps;
  motion_max_g = 0.0f;
  motion_min_g = 99.0f;
  motion_rate_dps = 0.0f;
  motion_samples = 0;
  taskEXIT_CRITICAL();
  return any;
}

static void sensors_task(void *arg)
{
  (void)arg;
  bool on = false;
  uint32_t last_try = 0;
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    health_alive(HEALTH_SENSORS);
    if (!want_on) {
      if (on) {
        stop_hw();
        on = false;
      }
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
      last = xTaskGetTickCount();
      continue;
    }
    if (on && restart_request) {
      stop_hw(); /* new barometer rate: sensors and filters start again */
      on = false;
    }
    restart_request = false;
    if (!on) {
      taskENTER_CRITICAL();
      data = (sensors_t){0};
      taskEXIT_CRITICAL();
      start_hw();
      on = true;
      ref_offset = 0.0f;
      last_try = HAL_GetTick();
      last = xTaskGetTickCount();
    } else if ((!data.baro_ok || !data.imu_ok) && HAL_GetTick() - last_try >= RETRY_MS) {
      start_hw();
      last_try = HAL_GetTick();
      last = xTaskGetTickCount();
    }
    vTaskDelayUntil(&last, pdMS_TO_TICKS(TICK_MS));
    sample_baro();
    sample_imu();
    vario_audio_tick(data.valid ? data.vario_ms : 0.0f, HAL_GetTick());
  }
}

void sensors_init(void)
{
  xTaskCreate(sensors_task, "sensors", 512, NULL, 5, &task);
}

void sensors_start(void)
{
  want_on = true;
  xTaskNotifyGive(task);
}

void sensors_stop(void)
{
  want_on = false;
  xTaskNotifyGive(task);
}

void sensors_set_baro_rate(unsigned hz)
{
  unsigned r = hz == 32U || hz == 64U ? hz : 100U;
  if (r != baro_rate) {
    baro_rate = r;
    restart_request = true;
    xTaskNotifyGive(task);
  }
}

unsigned sensors_baro_rate(void)
{
  return baro_rate;
}

void sensors_set_accel_calibration(const float offset[3], const float scale[3])
{
  taskENTER_CRITICAL();
  for (int k = 0; k < 3; k++) {
    acc_off[k] = offset[k];
    acc_scale[k] = scale[k];
  }
  taskEXIT_CRITICAL();
  rawlog_calibration(offset, scale);
}

void sensors_accel_calibration(float offset[3], float scale[3])
{
  taskENTER_CRITICAL();
  for (int k = 0; k < 3; k++) {
    offset[k] = acc_off[k];
    scale[k] = acc_scale[k];
  }
  taskEXIT_CRITICAL();
}

void sensors_cal_sample(sensors_cal_sample_t *out)
{
  taskENTER_CRITICAL();
  *out = cal_out;
  taskEXIT_CRITICAL();
}

void sensors_set_tap_handler(void (*handler)(void))
{
  tap_handler = handler;
}

void sensors_handling(void)
{
  handling = true;
}

void sensors_set_tap_sensitivity(int sensitivity)
{
  tap_sensitivity = sensitivity < TAP_SENSITIVITY_MIN   ? TAP_SENSITIVITY_MIN
                    : sensitivity > TAP_SENSITIVITY_MAX ? TAP_SENSITIVITY_MAX
                                                        : sensitivity;
}

int sensors_tap_sensitivity(void)
{
  return tap_sensitivity;
}

float sensors_last_tap_g(void)
{
  return last_tap_g;
}

void sensors_get(sensors_t *out)
{
  taskENTER_CRITICAL();
  *out = data;
  taskEXIT_CRITICAL();
}

void sensors_set_reference(float alt_msl_m)
{
  ref_alt = alt_msl_m;
  ref_request = true;
}

void sensors_track_reference(float alt_msl_m)
{
  track_alt = alt_msl_m;
  track_request = true;
}

void sensors_sleep_hw(void)
{
  sensor_bus_init();
  dps310_sleep();
  icm42605_sleep();
}

void sensors_monitor(bool on)
{
  monitor = on;
}
