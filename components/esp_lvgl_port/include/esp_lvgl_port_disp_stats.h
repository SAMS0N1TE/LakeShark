/* LS-781  Display refresh cadence counters.
 *
 * The 4.3 panel intermittently shows a whole-screen blue frame. That is what a
 * MIPI-DSI DPI underrun looks like: the framebuffer DMA is a continuous PSRAM
 * read (see LS-906 in the BSP, where the pixel clock was already cut to 20 MHz
 * for the same bus-contention reason), and when it cannot keep up, one frame
 * goes out short.
 *
 * Nothing in the driver reports an underrun, so this counts the next best
 * thing: the interval between on_refresh_done events. A frame that underran or
 * a refresh that stalled shows up as a gap longer than one frame period. This
 * turns "it flashed blue" into a number that can be compared between builds.
 *
 * Counters live in internal DRAM and are updated from the refresh ISR, so they
 * are safe to touch there. The callback itself is ordinary flash-resident code,
 * so gaps that occur while the cache is disabled (an NVS write) are observed as
 * a long interval afterwards rather than as an event during the stall.
 */
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

/* LS-802  Claim the most recent late frame, if one has not been reported
 * yet. Returns its gap in microseconds, or 0 when there is nothing new.
 * Four theories about the blue frame were wrong in a row, so stop guessing:
 * have the board announce each late frame as it happens, and read what else
 * the log says at the same timestamp. Called from a task, never the ISR. */
uint32_t lvgl_port_disp_stats_take_late(void);

/* LS-803  Name of the task that was running when the frame was missed, or
 * NULL. The stall has a fixed ~7.6 s period on P25 and on no other screen,
 * and naming suspects one at a time has failed four times, so let the ISR
 * record who it interrupted. */
const char *lvgl_port_disp_stats_late_task(void);

#ifdef __cplusplus
}
#endif
