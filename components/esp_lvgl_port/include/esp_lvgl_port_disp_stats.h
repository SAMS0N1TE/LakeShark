

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t frames;        /* on_refresh_done events counted since reset */
    uint32_t late;          /* intervals longer than threshold_us */
    uint32_t max_gap_us;    /* worst interval observed */
    uint32_t last_gap_us;   /* most recent interval */
    uint32_t threshold_us;  /* current late threshold */
    uint32_t uptime_ms;     /* device uptime when the snapshot was taken */
} lvgl_port_disp_stats_t;

/* Snapshot the counters. Safe from a task; briefly enters a critical section. */
void lvgl_port_disp_stats_get(lvgl_port_disp_stats_t *out);

/* Zero the counters. threshold_us of 0 keeps the current threshold; the
 * default is 37000, about 1.5 frames at the 4.3 panel's ~40 fps. */
void lvgl_port_disp_stats_reset(uint32_t threshold_us);

uint32_t lvgl_port_disp_stats_take_late(void);

const char *lvgl_port_disp_stats_late_task(void);

#ifdef __cplusplus
}
#endif
