#include "kml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Text for XML: & < > become entities. */
static size_t xml_text(char *out, size_t size, const char *s)
{
  size_t n = 0;
  for (; s && *s; s++) {
    const char *rep = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : NULL;
    size_t need = rep ? strlen(rep) : 1U;
    if (n + need + 1U > size) {
      break;
    }
    if (rep) {
      memcpy(out + n, rep, need);
      n += need;
    } else {
      out[n++] = *s;
    }
  }
  out[n] = 0;
  return n;
}

size_t kml_header(char *out, size_t size, const char *name, const char *description)
{
  char n[80], d[160];
  xml_text(n, sizeof(n), name);
  xml_text(d, sizeof(d), description);
  int len = snprintf(out, size,
                     "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                     "<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n"
                     "<Document>\n"
                     "<name>%s</name>\n"
                     "<Style id=\"track\"><LineStyle><color>ff00a5ff</color><width>3</width></LineStyle></Style>\n"
                     "<Placemark>\n"
                     "<name>%s</name>\n"
                     "<description>%s</description>\n"
                     "<styleUrl>#track</styleUrl>\n"
                     "<LineString>\n"
                     "<altitudeMode>absolute</altitudeMode>\n"
                     "<coordinates>\n",
                     n, n, d);
  return (len > 0 && (size_t)len < size) ? (size_t)len : 0U;
}

static void e7_text(char *out, size_t size, int32_t e7)
{
  uint32_t a = (uint32_t)(e7 < 0 ? -(int64_t)e7 : e7);
  snprintf(out, size, "%s%lu.%07lu", e7 < 0 ? "-" : "", (unsigned long)(a / 10000000U), (unsigned long)(a % 10000000U));
}

size_t kml_point(char *out, size_t size, int32_t lat_e7, int32_t lon_e7, float alt_m)
{
  char lat[16], lon[16];
  e7_text(lat, sizeof(lat), lat_e7);
  e7_text(lon, sizeof(lon), lon_e7);
  long dm = (long)(alt_m * 10.0f + (alt_m < 0.0f ? -0.5f : 0.5f));
  int len = snprintf(out, size, "%s,%s,%s%ld.%ld\n", lon, lat, dm < 0 ? "-" : "", labs(dm) / 10, labs(dm) % 10);
  return (len > 0 && (size_t)len < size) ? (size_t)len : 0U;
}

size_t kml_footer(char *out, size_t size)
{
  int len = snprintf(out, size, "</coordinates>\n</LineString>\n</Placemark>\n</Document>\n</kml>\n");
  return (len > 0 && (size_t)len < size) ? (size_t)len : 0U;
}
