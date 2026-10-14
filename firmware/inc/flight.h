#ifndef FLIGHT_H
#define FLIGHT_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Takeoff and landing detection without hardware access (unit-tested),
 * called once per second.
 *
 * Takeoff (with a GPS fix; the vario is switched on at the launch, 2026-10-13),
 * any one of:
 *   launch point  the device averages its standing position (good fixes,
 *                 below 2 km/h); moving 50 m away from it is the takeoff. Only
 *                 good fixes count (HDOP <= 3), and the way out must be backed
 *                 by the ground speed (Doppler): a GPS jump or a position that
 *                 wanders while standing moves no speed. Standing anywhere again
 *                 moves the launch point along, so a few steps around the launch
 *                 never add up; walking 50 m away in one go counts as a takeoff.
 *                 Before the first takeoff only: after a landing the pilot walks
 *                 away, there the speed and the altitude below decide.
 *   speed         ground speed >= 15 km/h for 5 s in a row (good fixes)
 *   altitude      >= 15 m altitude change within 30 s, or |climb rate| >=
 *                 1.5 m/s for 5 s (barometer: no GPS errors)
 * The takeoff time is when it began: the last second standing at the launch
 * point, the start of the fast phase or of the climb or sink.
 *
 * Landing, any of three rules (each can be switched off, all of them together
 * too: calm soaring in strong wind can look like a landing):
 *   calm      ground speed < 3 km/h and |climb rate| < 0.3 m/s for the landing
 *             time (default 60 s) with a fix
 *   impact    a touchdown shock (|a| peak >= 4 g while the load also drops below
 *             1 g in that second: a spiral pulls 4 g too, but steadily), then for
 *             10 s ground speed < 10 km/h (when there is a fix) and no constant
 *             climb or sink (barometer over 5 s < 0.5 m/s); landing time = the shock
 *   handling  ground handling: within 30 s three seconds with a rotation > 250
 *             deg/s while the load drops below 0.5 g (packing, walking with the
 *             wing; the flights of 2026-10-12 never had one in 600 s of flight),
 *             and 20 s slow without constant climb or sink
 * Walking away at 3-6 km/h no longer keeps the device in flight mode; the
 * impact and handling rules also work without a fix. A landing never ends the
 * recording: after a top landing the same rules detect the next takeoff and the
 * track goes on.
 */
#define FLIGHT_LANDING_DEFAULT_S 60U
#define FLIGHT_WINDOW_S 30

/* Takeoff thresholds (flight_takeoff* keys in config_ext.ultrabip); flight_init sets the defaults above. */
typedef struct {
  float speed_ms;   /* ground speed at least this ... */
  uint8_t speed_s;  /* ... for this many seconds in a row */
  float distance_m; /* distance from the launch point */
  float altitude_m; /* altitude change within the window ... */
  float climb_ms;   /* ... or |climb rate| at least this ... */
  uint8_t climb_s;  /* ... for this many seconds in a row */
} flight_takeoff_t;

/* Landing rules besides the calm one (flight_landing* keys); flight_init sets the defaults above. */
typedef struct {
  bool impact;      /* landing after a touchdown shock ... */
  float impact_g;   /* ... of at least this peak ... */
  float slow_ms;    /* ... then slower than this (impact and handling rules) ... */
  uint8_t impact_s; /* ... and without constant climb or sink for this long */
  bool handling;    /* landing from ground handling motion */
} flight_landing_t;

typedef enum { FLIGHT_GROUND, FLIGHT_AIRBORNE, FLIGHT_LANDED } flight_state_t;
typedef enum { FLIGHT_EVENT_NONE, FLIGHT_EVENT_TAKEOFF, FLIGHT_EVENT_LANDING } flight_event_t;

typedef struct {
  uint32_t t_s;     /* monotonic seconds (device clock) */
  bool fix;         /* valid 3D GPS fix */
  int32_t lat_e7;
  int32_t lon_e7;
  float speed_ms;   /* ground speed */
  float alt_m;      /* pressure altitude (standard atmosphere: never re-referenced) */
  float vario_ms;
  bool motion;        /* the IMU values below are valid */
  float acc_max_g;    /* highest |acceleration| within the last second */
  float acc_min_g;    /* lowest |acceleration| within the last second */
  float rate_max_dps; /* highest rotation rate within the last second */
  float hdop;          /* of the fix; 0 = unknown (not good enough for the launch point) */
} flight_input_t;

typedef struct {
  flight_input_t hist[FLIGHT_WINDOW_S + 1];
  unsigned count;
  unsigned head;
  flight_state_t state;
  uint32_t fast_since;  /* ground speed >= limit since (0 = not) */
  uint32_t vario_since; /* |climb rate| >= limit since */
  int32_t anchor_lat_e7, anchor_lon_e7; /* the launch point ... */
  uint16_t anchor_n;    /* ... averaged over this many standing seconds (0 = none yet) */
  uint32_t still_s;     /* last second standing (good fix, below 2 km/h) */
  float away_m;         /* way out by the ground speed since standing */
  uint8_t away_s;       /* seconds in a row beyond the distance */
  uint32_t calm_since;  /* landing conditions since */
  uint32_t ground_since; /* slow and no constant climb or sink since */
  uint32_t shock_s;      /* last touchdown-like shock (0 = none) */
  uint32_t first_shock_s; /* first shock of the current series: the touchdown */
  uint32_t handling_bits; /* one bit per second, newest lowest: ground handling motion */
  uint32_t takeoff_s;   /* estimated takeoff: when the indicator that detected it began */
  uint32_t landing_s;
  bool landing_enabled;
  uint32_t landing_time_s;
  flight_landing_t landing;
  flight_takeoff_t takeoff;
} flight_detector_t;

void flight_init(flight_detector_t *d);
/* Landing detection on/off and the calm time it needs (10..600 s). */
void flight_set_landing(flight_detector_t *d, bool enabled, uint32_t seconds);
/* Impact and handling rules, clamped: 3..15 g, 3..30 km/h, 3..60 s. */
void flight_set_landing_rules(flight_detector_t *d, const flight_landing_t *l);
/* Takeoff thresholds, clamped: 5..50 km/h, 1..30 s, 10..500 m, 5..100 m, 0.5..5 m/s, 1..30 s. */
void flight_set_takeoff(flight_detector_t *d, const flight_takeoff_t *t);
flight_event_t flight_update(flight_detector_t *d, const flight_input_t *in);
/* Takeoff detection off: airborne from now on (the caller treats it as a takeoff). */
void flight_force_airborne(flight_detector_t *d, uint32_t now_s);
const char *flight_state_name(flight_state_t s);

#endif
