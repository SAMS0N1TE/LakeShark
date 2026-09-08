#ifndef LOCATION_PREF_H
#define LOCATION_PREF_H
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
/* One NVS u64 stores the pair atomically. Zero explicitly means unset;
 * the high bit identifies v1; biased microdegrees leave reserved bits. */
static inline bool location_pack(double lat, double lon, uint64_t *out)
{
    if (!out || !isfinite(lat) || !isfinite(lon) || lat < -90 || lat > 90 || lon < -180 || lon > 180) return false;
    uint64_t a = (uint64_t)llround((lat + 90) * 1000000);
    uint64_t b = (uint64_t)llround((lon + 180) * 1000000);
    *out = (UINT64_C(1) << 63) | (a << 29) | b;
    return true;
}
static inline bool location_unpack(uint64_t stored, float *lat, float *lon)
{
    uint64_t a = (stored >> 29) & UINT64_C(0xfffffff);
    uint64_t b = stored & UINT64_C(0x1fffffff);
    if (lat) *lat = 0;
    if (lon) *lon = 0;
    if ((stored >> 57) != 64 || a > 180000000 || b > 360000000) return false;
    if (lat) *lat = (float)((double)a / 1000000 - 90);
    if (lon) *lon = (float)((double)b / 1000000 - 180);
    return true;
}
static inline bool location_parse(const char *text, bool longitude, double *out)
{
    if (!text || !out || !*text) return false;
    const char *p = text;
    if (*p == '-' || *p == '+') ++p;
    unsigned digits = 0, dots = 0;
    for (; *p; ++p) {
        if (*p >= '0' && *p <= '9') ++digits;
        else if (*p == '.' && ++dots == 1) {}
        else return false;
    }
    if (!digits) return false;
    double v = strtod(text, NULL), bound = longitude ? 180 : 90;
    if (!isfinite(v) || v < -bound || v > bound) return false;
    *out = v;
    return true;
}
static inline uint64_t location_load(bool found, uint64_t stored,
                                    bool legacy_found, int32_t lat, int32_t lon)
{
    if (found) return location_unpack(stored, NULL, NULL) ? stored : 0;
    uint64_t result = 0;
    /* Legacy (0,0) was explicitly treated as unset by the old getter. */
    if (legacy_found && (lat || lon)) location_pack(lat / 1000000.0, lon / 1000000.0, &result);
    return result;
}
#endif
