#ifndef VARIO_H
#define VARIO_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Vario core without hardware access (unit-tested in tests/logic_tests.c):
 * the vertical filter and the tone map.
 */

/*
 * Vertical filter: barometric altitude -> altitude and climb rate. Kalman
 * filter with the states altitude and climb rate; the vertical acceleration
 * is white noise. Tuned on simulated baro data (32 Hz, 5 cm noise, climb
 * 0 -> 2 m/s within 1 s): climb rate noise 0.03 m/s, lag 0.4 s, overshoot
 * 0.1 m/s. A third state (acceleration) halves the lag but overshoots by
 * 0.6 m/s; the IMU will reduce the lag later.
 */
#define VARIO_FILTER_Q 2.0f  /* acceleration noise density [(m/s^2)^2 s] */
#define VARIO_FILTER_R 0.01f /* altitude measurement variance [m^2] */

typedef struct {
  float h;                /* altitude [m] */
  float v;                /* climb rate [m/s] */
  float p00, p01, p11;    /* covariance */
  float q, r;
  bool started;
} vario_filter_t;

void vario_filter_init(vario_filter_t *f, float q, float r);
void vario_filter_update(vario_filter_t *f, float altitude, float dt);

/*
 * Tone map of a profile, stock keys: vario_tones_vz/freq/duration/duty_0..7,
 * vario_liftstart_int, vario_sinkstart_int, vario_sinkalarm_bool,
 * vario_sweeptone_bool. Points are sorted by climb rate; values in between
 * are interpolated linearly.
 */
#define VARIO_TONE_POINTS 8

typedef struct {
  int16_t vz_cms;     /* climb rate [cm/s] */
  uint16_t freq_hz;
  uint16_t period_ms; /* "duration": one beep plus its pause */
  uint8_t duty;       /* percent of the period with sound, 100 = continuous */
} vario_tone_point_t;

typedef struct {
  vario_tone_point_t point[VARIO_TONE_POINTS];
  int16_t lift_start_cms; /* beeps from this climb rate on */
  int16_t sink_start_cms; /* sink tone from this climb rate down (if sink_alarm) */
  bool sink_alarm;
  bool sweep;             /* vario_sweeptone_bool: the pitch follows the climb rate during a beep */
  bool lifty;             /* vario_liftysound_bool: the weak-lift tone (vario_tone_weak) */
  uint8_t integration_pct; /* vario_integrationtime_int: damping of the climb rate the tones follow */
} vario_tones_t;

/* Weak-lift tone: this far below the lift threshold ("zero sink": the air carries the glider). */
#define VARIO_WEAK_BAND_CMS 40
#define VARIO_WEAK_ON_MS    40
#define VARIO_WEAK_PERIOD_MS 1000

typedef struct {
  bool sound;
  uint16_t freq_hz;
  uint16_t period_ms;
  uint16_t on_ms;
} vario_tone_t;

void vario_tones_defaults(vario_tones_t *t);
/*
 * Tone curve in the text form of the Bluetooth command TONES (configurator):
 * "<lift cm/s> <sink cm/s> <sink alarm 0|1> <vz,freq,period,duty> x 8 [<weak-lift 0|1> [<sweep 0|1> [<integration %>]]]".
 * Only what the text carries is set: the caller starts from the profile's curve, so the flags the
 * command leaves out stay the profile's (an old configurator sent none and got the defaults, which
 * switched the weak-lift tone on for the test).
 */
bool vario_tones_parse(const char *args, vario_tones_t *t);
vario_tone_t vario_tone(const vario_tones_t *t, float climb_ms);
/*
 * Weak-lift tone (vario_liftysound_bool): a 40 ms tick every second while the climb rate is
 * within VARIO_WEAK_BAND_CMS below the lift threshold, pitched like the curve at that climb
 * rate. Silent when disabled or outside the band (the caller plays it in flight only).
 */
vario_tone_t vario_tone_weak(const vario_tones_t *t, float climb_ms);

/*
 * A tone period normally runs to its end before the climb rate is looked at again. This says
 * when the running one is cut short so that lift is heard at once:
 *  - lift begins while a weak-lift tick or the sink tone runs (lift_running false);
 *  - the lift grew stronger during the pause of a lift period, and the shorter period of the new
 *    climb rate is already over (a beep that sounds is never cut).
 * elapsed_ms: time since the running period began, on_ms: its beep length.
 */
/*
 * Integration time (vario_integrationtime_int, 0..100 %): the stock app shows 0/25/50/75/100 % as
 * 0.3/0.7/1.4/2.8/5.7 s. The tones follow the climb rate through a first-order filter with this time
 * constant in seconds, the stock seconds less the 0.2 s the fusion already lags (0.1 ... 5.5 s,
 * linear in between). Flights of 2026-10-12: at 25 % the false lift beeps in a calm glide drop
 * from about 4 to 1 per minute, in spirals from 21 to 3, for 0.45 s more lag.
 */
float vario_integration_tau_s(uint8_t pct);

bool vario_tone_cut_short(const vario_tones_t *t, float climb_ms, bool lift_running, uint16_t on_ms,
                          uint32_t elapsed_ms);

#endif
