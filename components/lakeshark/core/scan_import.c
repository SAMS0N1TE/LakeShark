#include "scan_import.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

bool scan_import_line(const char *line, scan_channel_t *out)
{
    if (!line || !out || strlen(line) >= 192) return false;
    char text[192];
    strcpy(text, line);
    text[strcspn(text, "\r\n")] = 0;
    char *f[8], *p = text;
    for (int i = 0; i < 8; ++i) {
        f[i] = p;
        char *sep = strchr(p, '|');
        if ((i < 7) != (sep != NULL)) return false;
        if (sep) { *sep = 0; p = sep + 1; }
        if (!*f[i]) return false;
    }
    if (strlen(f[0]) >= SCAN_NAME_LEN) return false;
    for (const char *q = f[0]; *q; ++q) if (*q < 32 || *q > 126) return false;
    double v[6];
    const int fields[] = {1,3,4,5,6,7};
    for (int i = 0; i < 6; ++i) {
        char *end;
        v[i] = strtod(f[fields[i]], &end);
        if (end == f[fields[i]] || *end || !isfinite(v[i])) return false;
    }
    if (v[0] < 1000000 || v[0] > 2000000000 || floor(v[0]) != v[0] ||
        v[1] < 0 || v[1] >= SCAN_MAX_ZONES || floor(v[1]) != v[1] ||
        fabs(v[2]) > 90 || fabs(v[3]) > 180 || v[4] < 0 || v[4] > 500 ||
        (v[5] != 0 && v[5] != 1)) return false;
    int mode = !strcmp(f[2], "P25") ? SCAN_MODE_P25 : !strcmp(f[2], "NFM") ? SCAN_MODE_NFM : -1;
    if (mode < 0) return false;
    scan_channel_t result = {0};
    strcpy(result.name, f[0]);
    result.freq_hz = (uint32_t)v[0]; result.mode = mode; result.zone = (uint8_t)v[1];
    result.lat_e7 = (int32_t)llround(v[2] * 1e7);
    result.lon_e7 = (int32_t)llround(v[3] * 1e7);
    result.radius_m = (uint32_t)llround(v[4] * 1000);
    result.flags = SCAN_FLAG_ENABLED | (v[5] ? SCAN_FLAG_PRIORITY : 0);
    *out = result;
    return true;
}
