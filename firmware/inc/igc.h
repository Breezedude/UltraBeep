#ifndef IGC_H
#define IGC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * IGC lines without hardware access (unit-tested): header, B records, the
 * VBV data records (docs/igc-vbv-records.md), markers and the file name.
 * Every function returns the length written including the CR LF, or 0 if the
 * buffer was too small.
 */
#define IGC_MANUFACTURER "XUB" /* "X" = logger without IGC approval, UB = UltraBeep (G and L records) */
/* The files carry the stock firmware's identity (A record and name "XSD-UB<serial>"): the BipLink app
 * takes only tracks that look like the stock's. Our own firmware is named in an "LXUB" record. */
#define IGC_STOCK_MANUFACTURER "XSD"
#define IGC_TRACK_ID_SIZE 16
#define IGC_LINE_MAX     96

typedef struct {
  uint32_t utc;          /* date of the flight (first record) */
  unsigned flight_number;
  const char *serial;    /* 4 characters after "UB", the stock's serial number's last four */
  const char *pilot;
  const char *glider_type;
  const char *glider_id;
  const char *competition_id;
  const char *firmware;      /* HFRFWFIRMWAREVERSION in the stock's form "YYYY-MM-DD:hash" */
  const char *firmware_own;  /* this firmware, in an LXUB record */
  int16_t time_offset_min;   /* HFTZNTIMEZONE */
  const uint8_t *track_id;   /* IGC_TRACK_ID_SIZE bytes for the LXSDB record (the app's track UUID), or NULL */
} igc_header_t;

/* A, H and I records. */
size_t igc_format_header(char *out, size_t size, const igc_header_t *h);

typedef struct {
  uint32_t utc;        /* time of the fix, whole seconds */
  int32_t lat_e7;
  int32_t lon_e7;
  bool valid;          /* 3D fix: 'A', otherwise 'V' */
  int32_t press_alt_m; /* standard atmosphere */
  int32_t gnss_alt_m;  /* above mean sea level */
  uint8_t sats;        /* SIU extension */
} igc_fix_t;

size_t igc_format_b(char *out, size_t size, const igc_fix_t *b);

/* One sample of the VBV data record (50 bytes, Base64), field order as in the spec. */
typedef struct {
  uint32_t t_ms;
  float lat_deg, lon_deg, h_ell_m;
  float vel_n_ms, vel_e_ms, vel_d_ms;
  float baro_alt_m, h_msl_m, vario_ms, alt_m, accel_ms2;
  uint8_t sats;
  uint8_t status; /* bit 0: valid 3D GNSS fix */
} igc_vbv_t;

#define IGC_VBV_BYTES 50
size_t igc_format_vbv(char *out, size_t size, const igc_vbv_t *s);

/* LVBVTAKEOFFhhmmss / LVBVLANDINGhhmmss */
size_t igc_format_marker(char *out, size_t size, const char *what, uint32_t utc);
/* LVBVWINDhhmmss <km/h, one decimal> <direction it comes from, deg> */
size_t igc_format_wind(char *out, size_t size, uint32_t utc, float kmh, float from_deg);

/* G record: Base64 of the 32-byte HMAC. */
size_t igc_format_g(char *out, size_t size, const uint8_t mac[32]);

/* YYYY-MM-DD-XSD-UBSSSS-NN.igc (the stock's naming), without directory. */
size_t igc_file_name(char *out, size_t size, uint32_t utc, const char *serial, unsigned flight_number);

/* Standard Base64 with padding; out needs 4 * ceil(n / 3) + 1 bytes. */
size_t base64_encode(const uint8_t *in, size_t n, char *out);

#endif
