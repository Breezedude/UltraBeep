#ifndef SENSORS_H
#define SENSORS_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Sensor task: barometer (32, 64 or 100 Hz, dps310.h) and IMU (100 Hz) ->
 * fusion -> altitude and climb rate. Runs while the device is switched on;
 * also drives the vario tones (vario_audio.c). The accelerometer calibration
 * (offset and scale per axis) is applied before the fusion and the tap
 * detection.
 */
typedef struct {
  bool on;             /* switched on (sensors_start) */
  bool baro_ok;        /* barometer answered and runs */
  bool imu_ok;
  uint8_t baro_id;     /* id registers read at the last start, for diagnostics */
  uint8_t imu_id;
  bool valid;          /* barometer running and the filter settled */
  bool referenced;     /* altitudes follow the GNSS altitude (sensors_set_reference) */
  float pressure_pa;
  float temp_c;
  float alt_std_m;     /* pressure altitude, standard atmosphere 1013.25 hPa (IGC B record) */
  float alt_raw_m;     /* barometric altitude, unfiltered, GNSS-referenced (QNH-like) */
  float alt_m;         /* filtered altitude, GNSS-referenced */
  float vario_ms;      /* filtered climb rate */
  bool fused;          /* barometer + IMU (false: barometer only) */
  bool at_rest;        /* the IMU sees the device lying still */
  float accel_ms2;     /* magnitude of the acceleration, smoothed (0.5 s) */
  float accel_fast_ms2; /* ... smoothed over 0.1 s (manoeuvre detection) */
  float rate_dps;      /* magnitude of the rotation rate, smoothed over 0.1 s [deg/s] */
  bool yaw_ok;         /* the attitude is known: yaw_dps is usable */
  float yaw_dps;       /* turn rate about the vertical, 1 s mean, clockwise positive [deg/s] */
  uint32_t samples;    /* barometer samples since the start */
} sensors_t;

void sensors_init(void);
void sensors_start(void);
void sensors_stop(void);
void sensors_get(sensors_t *out);
/* Peaks of the IMU since the last call (once per second: the landing detection); false without samples. */
bool sensors_take_motion(float *acc_max_g, float *acc_min_g, float *rate_max_dps);
/* Double tap on the device (tap.h): the handler runs in the sensor task. */
void sensors_set_tap_handler(void (*handler)(void));
/* A button press: the vario does not trust the barometer for a moment (fusion.h). */
void sensors_handling(void);
void sensors_set_tap_sensitivity(int sensitivity);
int sensors_tap_sensitivity(void);
/* Jerk of the last single tap in g (0 = none yet): the configurator shows it for the calibration. */
float sensors_last_tap_g(void);
/* GNSS altitude above mean sea level now: from then on alt_raw/alt_m follow it. */
void sensors_set_reference(float alt_msl_m);
/* Every further good fix: the reference follows the GNSS altitude slowly (10 min). */
void sensors_track_reference(float alt_msl_m);
/* Barometer samples per second (32, 64, 100): applies at once (the sensors restart). */
void sensors_set_baro_rate(unsigned hz);
unsigned sensors_baro_rate(void);
/* Accelerometer calibration: a = (raw - offset) * scale per axis [m/s²]. */
void sensors_set_accel_calibration(const float offset[3], const float scale[3]);
void sensors_accel_calibration(float offset[3], float scale[3]);
/*
 * For the calibration in the configurator: mean of the raw (uncalibrated) accelerometer over the
 * last second [m/s²] and whether the device lay still meanwhile. seq counts the seconds.
 */
typedef struct {
  float mean[3];
  bool still;
  uint32_t seq;
} sensors_cal_sample_t;
void sensors_cal_sample(sensors_cal_sample_t *out);
/* Puts barometer and IMU to sleep; no RTOS calls (standby entry). */
void sensors_sleep_hw(void);
/* Logs pressure, altitude, climb rate and load once per second. */
void sensors_monitor(bool on);

#endif
