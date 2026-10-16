#include "igc.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "rtc_time.h"

size_t base64_encode(const uint8_t *in, size_t n, char *out)
{
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16;
    if (i + 1 < n) {
      v |= (uint32_t)in[i + 1] << 8;
    }
    if (i + 2 < n) {
      v |= in[i + 2];
    }
    out[o++] = alphabet[(v >> 18) & 63];
    out[o++] = alphabet[(v >> 12) & 63];
    out[o++] = i + 1 < n ? alphabet[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < n ? alphabet[v & 63] : '=';
  }
  out[o] = 0;
  return o;
}

/* Appends formatted text; on overflow the result is marked by *ok = false. */
static void put(char *out, size_t size, size_t *len, bool *ok, const char *fmt, ...)
{
  if (!*ok) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(out + *len, size - *len, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= size - *len) {
    *ok = false;
    return;
  }
  *len += (size_t)n;
}

/* Header values come from the configuration: no line breaks, empty = NIL. */
static const char *text(const char *s, char *buf, size_t size)
{
  size_t n = 0;
  for (; s && *s && n + 1 < size; s++) {
    buf[n++] = (*s == '\r' || *s == '\n') ? ' ' : *s;
  }
  buf[n] = 0;
  return n ? buf : "NIL";
}

/* Base64 without padding, as the stock writes its LXSD records. */
static void base64_nopad(char *out, const uint8_t *in, size_t n)
{
  static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16;
    if (i + 1 < n) {
      v |= (uint32_t)in[i + 1] << 8;
    }
    if (i + 2 < n) {
      v |= in[i + 2];
    }
    out[o++] = tab[(v >> 18) & 63];
    out[o++] = tab[(v >> 12) & 63];
    if (i + 1 < n) {
      out[o++] = tab[(v >> 6) & 63];
    }
    if (i + 2 < n) {
      out[o++] = tab[v & 63];
    }
  }
  out[o] = 0;
}

size_t igc_format_header(char *out, size_t size, const igc_header_t *h)
{
  uint16_t y;
  uint8_t mo, d, hh, mi, ss;
  civil_from_unix(h->utc, &y, &mo, &d, &hh, &mi, &ss);
  char b1[48], b2[48], b3[48], b4[48];
  size_t len = 0;
  bool ok = true;
  /* the layout and wording of the stock firmware's files: the BipLink app knows these */
  put(out, size, &len, &ok, "A%sUB%.4s\r\n", IGC_STOCK_MANUFACTURER, h->serial);
  put(out, size, &len, &ok, "HFDTE%02u%02u%02u\r\n", d, mo, y % 100U);
  put(out, size, &len, &ok, "HFPLTPILOTINCHARGE:%s\r\n", text(h->pilot, b1, sizeof(b1)));
  put(out, size, &len, &ok, "HFCM2CREW2:NIL\r\n");
  put(out, size, &len, &ok, "HFGTYGLIDERTYPE:%s\r\n", text(h->glider_type, b2, sizeof(b2)));
  put(out, size, &len, &ok, "HFGIDGLIDERID:%s\r\n", text(h->glider_id, b3, sizeof(b3)));
  put(out, size, &len, &ok, "HFCIDCOMPETITIONID:%s\r\n", text(h->competition_id, b4, sizeof(b4)));
  put(out, size, &len, &ok, "HFDTMGPSDATUM:WGS84\r\n");
  put(out, size, &len, &ok, "HFRFWFIRMWAREVERSION:%s\r\n", h->firmware);
  put(out, size, &len, &ok, "HFRHWHARDWAREVERSION:ULTRABIP 1.0\r\n");
  put(out, size, &len, &ok, "HFFTYFRTYPE:STODEUS,ULTRABIP\r\n");
  put(out, size, &len, &ok, "HFGPSRECEIVER:GOTOP,GT1110SN,22,18000\r\n");
  if (h->time_offset_min % 60 == 0) {
    put(out, size, &len, &ok, "HFTZNTIMEZONE:%d\r\n", h->time_offset_min / 60);
  } else {
    /* hours with one decimal (5.5, -3.5); integer arithmetic, no float formatting (fmt.h) */
    int m = h->time_offset_min < 0 ? -h->time_offset_min : h->time_offset_min;
    int tenths = (m * 10 + 30) / 60;
    put(out, size, &len, &ok, "HFTZNTIMEZONE:%s%d.%d\r\n", h->time_offset_min < 0 ? "-" : "", tenths / 10, tenths % 10);
  }
  put(out, size, &len, &ok, "HFPRSPRESSALTSENSOR:INFINEON,DPS310,7000\r\n");
  put(out, size, &len, &ok, "HFALGALTGPS:GEO\r\n");
  put(out, size, &len, &ok, "HFALPALTPRESSURE:ISA\r\n");
  put(out, size, &len, &ok, "LMMMGPSPERIOD1000MSEC\r\n");
  put(out, size, &len, &ok, "I013637SIU\r\n");
  if (h->track_id) {
    char b64[IGC_TRACK_ID_SIZE * 2];
    base64_nopad(b64, h->track_id, IGC_TRACK_ID_SIZE);
    put(out, size, &len, &ok, "LXSDB%s\r\n", b64); /* the track's identity for the app */
  }
  put(out, size, &len, &ok, "L%sFIRMWARE:UltraBeep %s\r\n", IGC_MANUFACTURER, h->firmware_own ? h->firmware_own : "?");
  return ok ? len : 0;
}

/* |degrees * 1e7| -> degrees and thousandths of minutes, cut (not rounded) as IGC requires. */
static void deg_min(int32_t e7, uint32_t *deg, uint32_t *min_e3)
{
  uint32_t a = (uint32_t)(e7 < 0 ? -(int64_t)e7 : e7);
  *deg = a / 10000000U;
  *min_e3 = (uint32_t)(((uint64_t)(a % 10000000U) * 60U) / 10000U);
}

size_t igc_format_b(char *out, size_t size, const igc_fix_t *b)
{
  uint16_t y;
  uint8_t mo, d, hh, mi, ss;
  civil_from_unix(b->utc, &y, &mo, &d, &hh, &mi, &ss);
  uint32_t lat_d, lat_m, lon_d, lon_m;
  deg_min(b->lat_e7, &lat_d, &lat_m);
  deg_min(b->lon_e7, &lon_d, &lon_m);
  int32_t pa = b->press_alt_m < -9999 ? -9999 : b->press_alt_m > 99999 ? 99999 : b->press_alt_m;
  int32_t ga = b->gnss_alt_m < -9999 ? -9999 : b->gnss_alt_m > 99999 ? 99999 : b->gnss_alt_m;
  int n = snprintf(out, size, "B%02u%02u%02u%02lu%05lu%c%03lu%05lu%c%c%05ld%05ld%02u\r\n", hh, mi, ss,
                   (unsigned long)lat_d, (unsigned long)lat_m, b->lat_e7 < 0 ? 'S' : 'N', (unsigned long)lon_d,
                   (unsigned long)lon_m, b->lon_e7 < 0 ? 'W' : 'E', b->valid ? 'A' : 'V', (long)pa, (long)ga,
                   b->sats > 99 ? 99U : b->sats);
  return (n > 0 && (size_t)n < size) ? (size_t)n : 0U;
}

static void put_u32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static void put_f32(uint8_t *p, float f)
{
  uint32_t v;
  memcpy(&v, &f, 4);
  put_u32(p, v);
}

size_t igc_format_vbv(char *out, size_t size, const igc_vbv_t *s)
{
  uint8_t raw[IGC_VBV_BYTES];
  const float f[11] = {s->lat_deg, s->lon_deg, s->h_ell_m, s->vel_n_ms, s->vel_e_ms, s->vel_d_ms,
                       s->baro_alt_m, s->h_msl_m, s->vario_ms, s->alt_m, s->accel_ms2};
  put_u32(raw, s->t_ms);
  for (int i = 0; i < 11; i++) {
    put_f32(raw + 4 + 4 * i, f[i]);
  }
  raw[48] = s->sats;
  raw[49] = s->status;
  if (size < 4 + 68 + 3) {
    return 0;
  }
  memcpy(out, "LVBV", 4);
  size_t n = 4 + base64_encode(raw, sizeof(raw), out + 4);
  out[n++] = '\r';
  out[n++] = '\n';
  out[n] = 0;
  return n;
}

size_t igc_format_marker(char *out, size_t size, const char *what, uint32_t utc)
{
  int n = snprintf(out, size, "LVBV%s%02lu%02lu%02lu\r\n", what, (unsigned long)(utc / 3600U % 24U),
                   (unsigned long)(utc / 60U % 60U), (unsigned long)(utc % 60U));
  return (n > 0 && (size_t)n < size) ? (size_t)n : 0U;
}

size_t igc_format_wind(char *out, size_t size, uint32_t utc, float kmh, float from_deg)
{
  unsigned k10 = (unsigned)(kmh < 0.0f ? 0.0f : kmh * 10.0f + 0.5f);
  unsigned deg = (unsigned)(from_deg < 0.0f ? 0.0f : from_deg + 0.5f) % 360U;
  int n = snprintf(out, size, "LVBVWIND%02lu%02lu%02lu %u.%u %03u\r\n", (unsigned long)(utc / 3600U % 24U),
                   (unsigned long)(utc / 60U % 60U), (unsigned long)(utc % 60U), k10 / 10U, k10 % 10U, deg);
  return (n > 0 && (size_t)n < size) ? (size_t)n : 0U;
}

size_t igc_format_g(char *out, size_t size, const uint8_t mac[32])
{
  if (size < 1 + 3 + 44 + 3) {
    return 0;
  }
  size_t n = (size_t)snprintf(out, size, "G%s", IGC_MANUFACTURER);
  n += base64_encode(mac, 32, out + n);
  out[n++] = '\r';
  out[n++] = '\n';
  out[n] = 0;
  return n;
}

size_t igc_file_name(char *out, size_t size, uint32_t utc, const char *serial, unsigned flight_number)
{
  uint16_t y;
  uint8_t mo, d, hh, mi, ss;
  civil_from_unix(utc, &y, &mo, &d, &hh, &mi, &ss);
  int n = snprintf(out, size, "%04u-%02u-%02u-%s-UB%.4s-%02u.igc", y, mo, d, IGC_STOCK_MANUFACTURER, serial,
                   flight_number % 100U);
  return (n > 0 && (size_t)n < size) ? (size_t)n : 0U;
}
