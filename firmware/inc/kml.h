#ifndef KML_H
#define KML_H

#include <stddef.h>
#include <stdint.h>

/*
 * KML track without hardware access (unit-tested): header, one coordinate
 * per GNSS fix and a footer. The recorder writes the footer after every
 * point and overwrites it with the next one, so the file is valid KML at
 * any time (also after a power loss).
 */
size_t kml_header(char *out, size_t size, const char *name, const char *description);
/* "lon,lat,alt" (degrees, metres above mean sea level) */
size_t kml_point(char *out, size_t size, int32_t lat_e7, int32_t lon_e7, float alt_m);
size_t kml_footer(char *out, size_t size);

#endif
