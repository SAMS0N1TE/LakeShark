#ifndef RS41_STORE_H
#define RS41_STORE_H
#include "rs41_decode.h"
#define RS41_SONDES 8
#define RS41_POINTS 128
typedef struct { double lat, lon; float alt; int64_t us; } rs41_point_t;
typedef struct {
    rs41_report_t report;
    int64_t heard_us, fix_us;
    unsigned count;
    rs41_point_t track[RS41_POINTS];
} rs41_sonde_t;
void rs41_store_put(const rs41_report_t *r, int64_t now);
bool rs41_store_copy(int slot, rs41_sonde_t *out, int64_t now);
void rs41_store_clear(void);
int rs41_keep_hours(void);
void rs41_set_keep_hours(int hours);
bool rs41_show_map(void);
void rs41_set_show_map(bool show);
#endif
