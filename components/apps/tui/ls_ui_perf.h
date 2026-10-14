/* Fixed-storage TUI frame/input measurements; owned by the UI task.
 * Copy a snapshot on that task, then compute percentiles on the console. */
#ifndef LS_UI_PERF_H
#define LS_UI_PERF_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define LS_UI_PERF_N 256
/* The loop sleeps at least 5 ms: enough for every frame in five seconds. */
#define LS_UI_PERF_FPS_N 1024
typedef struct {
    uint16_t value[LS_UI_PERF_N]; /* 0.1 ms, saturated at 6553.5 ms */
    uint16_t next, count;
} ls_ui_perf_ring_t;
typedef struct {
    ls_ui_perf_ring_t frame, input;
    uint32_t finished_ms[LS_UI_PERF_FPS_N];
    uint64_t pending_us[LS_UI_PERF_N], last_frame_us;
    uint16_t fps_next, fps_count, pending_next, pending_count;
    uint32_t late_frames;
} ls_ui_perf_t;
typedef struct {
    ls_ui_perf_ring_t frame, input;
    uint32_t fps_x10, late_frames;
} ls_ui_perf_snapshot_t;

/* Reset measurement windows, preserving the since-boot late count. */
void ls_ui_perf_reset(ls_ui_perf_t *p);
void ls_ui_perf_input(ls_ui_perf_t *p, uint64_t received_us);
void ls_ui_perf_frame(ls_ui_perf_t *p, uint64_t start_us, uint64_t end_us);
void ls_ui_perf_snapshot(const ls_ui_perf_t *p, uint64_t now_us,
                         ls_ui_perf_snapshot_t *out);
/* Sorts only the supplied snapshot ring, nearest-rank percentile; empty=0. */
uint16_t ls_ui_perf_percentile(ls_ui_perf_ring_t *ring, unsigned percent);
#ifdef __cplusplus
}
#endif
#endif
