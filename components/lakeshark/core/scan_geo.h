#ifndef LS_SCAN_GEO_H
#define LS_SCAN_GEO_H
#include "scan_channels.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint64_t eligible[(SCAN_MAX_CHANNELS + 63) / 64];
    int64_t fix_us;
    bool have_fix;
} scan_geo_t;
void scan_geo_update(scan_geo_t *state, const scan_channel_t *channels, int count,
                     bool valid, double lat, double lon, int64_t fix_us, int64_t now_us);
bool scan_geo_ready(const scan_geo_t *state, int64_t now_us);
bool scan_geo_admits(const scan_geo_t *state, int index, int64_t now_us);
#ifdef __cplusplus
}
#endif
#endif
