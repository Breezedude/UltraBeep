#ifndef GPS_H
#define GPS_H

#include <stdbool.h>
#include <stdint.h>

#include "nmea.h"

/*
 * GNSS module GT-1110SN (Sony CXD5603GF) on USART1, load switch PC6. The
 * module forgets every setting at power off, so each start sends them again:
 * power on, 1.25 s, "@WUP" (up to 3 tries), "@GSTP" (it positions on its own
 * after power on; the settings below need the idle state), "@BSSL 45" (GGA,
 * GSA, GSV, RMC), "@ABUP 0" (no backup into the module's own flash: a backup
 * also stores the UART rate and the positioning cycle, and a module that boots
 * at 460800 baud is lost for every host that opens the UART at 115200 - the
 * repair by a rate search and "@BUPC" is in start_sequence),  * "@GNS" (satellite systems), "@GSOP" (positioning cycle, 5 Hz by default),
 * "@CSBR" (a faster UART for the 5 Hz output), "@GTIM" with the RTC time if
 * it can be trusted, then "@GSR" (hot start; the module falls back to a warm
 * or cold start itself). Every step that the module refuses falls back to
 * what worked before: GPS only, 1 Hz, 115200 baud.
 */

typedef enum { GPS_OFF, GPS_STARTING, GPS_SEARCHING, GPS_FIX } gps_state_t;

typedef enum {
  GPS_START_AUTO, /* like stock: time only if the last GPS time sync is < 1 h old */
  GPS_START_COLD, /* never give the time */
  GPS_START_TIME, /* always give the RTC time if it was ever set */
} gps_start_t;

typedef struct {
  gps_state_t state;
  gps_start_t start_mode;
  bool time_injected;
  const char *systems; /* satellite systems the module accepted (@GNS), e.g. "GPS+GLONASS" */
  uint16_t cycle_ms;   /* positioning cycle the module accepted (@GSOP): 200 = 5 Hz */
  uint32_t baud;       /* UART baud rate (@CSBR) */
  uint32_t overruns;   /* bytes lost in the UART (the task or the interrupt was too late) */
  uint32_t placeholders; /* invalid epochs right after a valid one, skipped: the module could not keep the rate */
  uint32_t restarts;   /* restarts after the module went silent (10 s without a sentence) */
  uint32_t power_on_tick;
  /* milestones in ms after power on (0 = not reached yet) */
  uint32_t t_first_nmea;
  uint32_t t_first_signal; /* first satellite >= 30 dB-Hz */
  uint32_t t_four_good;    /* 4 satellites >= 30 dB-Hz */
  uint32_t t_time;         /* first valid UTC date/time */
  uint32_t t_fix2d;
  uint32_t t_fix3d;
  uint8_t fix_type;  /* GSA: 1 none, 2 2D, 3 3D */
  uint8_t quality;   /* GGA */
  uint8_t sats_used; /* GGA */
  uint8_t sats_view; /* GSV, all systems */
  uint8_t sats_good; /* >= 30 dB-Hz in the last 3 s */
  float hdop;
  int32_t lat_e7;
  int32_t lon_e7;
  float alt_m;
  float speed_kmh;
  float course_deg;
  bool time_valid;
  uint32_t unix_time;
  uint32_t lines;
  uint32_t bad_checksums;
  bool alive;          /* a sentence arrived within the last 5 s */
  uint32_t rx_bytes;   /* every byte the UART received since power on, sentences or not (diagnostics) */
  uint32_t rx_errors;  /* framing and noise errors: a wrong baud rate or a bad line */
} gps_status_t;

void gps_init(void);
void gps_start(gps_start_t mode);
void gps_stop(void);
/* Fixes per second from the next start on: 5 (default) or 1 (system_gpsrate_int); more counts as 5. */
void gps_set_rate(unsigned hz);
/* True once after the module failed twice at 5 Hz in this power-on (restarted itself or went silent) and was set to
 * 1 Hz: the caller stores the info flag (system_gpsfallback_int), so the next power-on starts at 1 Hz. */
bool gps_take_rate_fallback(void);
/* Satellite systems to ask for (system_gpssystems_string), at the next start. */
typedef enum { GPS_SYSTEMS_ALL = 0, GPS_SYSTEMS_GPS_GLONASS = 1, GPS_SYSTEMS_GPS = 2 } gps_systems_t;
void gps_set_systems(uint8_t systems);
/* Raw module command, e.g. "@VER" (sent with CR LF). */
void gps_send(const char *command);
/* Diagnostics (console): listen at another baud rate without restarting the module. */
void gps_set_baud(uint32_t baud);
/* Diagnostics: the RX line read as an input with a pull-up and with a pull-down (a line nobody
 * drives follows the pull: the module is unpowered or not connected). */
void gps_probe_rx(bool *with_pull_up, bool *with_pull_down);
/* Diagnostics: sends 0x55 continuously for this long, so the TX line to the module can be found with a
 * meter (half the supply voltage on average instead of the idle high). */
void gps_blast(unsigned seconds);
/* Diagnostics: power-cycles the module, then sends a bare line end every 250 ms for this long at
 * this baud rate and prints every byte that comes back (hex and text) on the console. A module whose
 * firmware no longer boots may answer from its boot loader; a healthy one answers nothing to an empty
 * line but sends NMEA on its own. Baud 0: a rate scan instead, 0.8 s at every rate, with the
 * count of sentences that parse at each. The module is off afterwards. */
void gps_boot_probe(uint32_t baud, unsigned seconds);
/* Mirrors every line from the module to the console. */
void gps_set_monitor(bool on);
/* Console: the satellites heard in the last seconds with their signal strength (C/N0), strongest first. */
void gps_print_sats(void);
void gps_get(gps_status_t *s);
/* Last complete fix (GGA + GSA + RMC of one epoch, 5 per second at 5 Hz); seq changes with every new one. */
void gps_get_fix(gps_fix_t *out);
const char *gps_state_name(gps_state_t state);
const char *gps_start_name(gps_start_t mode);

#endif
