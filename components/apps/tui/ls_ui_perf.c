#include "ls_ui_perf.h"
#include <string.h>

static void push(ls_ui_perf_ring_t *r, uint64_t us)
{
    r->value[r->next] = us >= 6553500 ? UINT16_MAX : (uint16_t)((us + 50) / 100);
    r->next = (r->next + 1) % LS_UI_PERF_N;
    if (r->count < LS_UI_PERF_N) r->count++;
}

void ls_ui_perf_reset(ls_ui_perf_t *p)
{
    uint32_t late = p->late_frames;
    memset(p, 0, sizeof(*p));
    p->late_frames = late;
}

void ls_ui_perf_input(ls_ui_perf_t *p, uint64_t received_us)
{
    p->pending_us[p->pending_next] = received_us;
    p->pending_next = (p->pending_next + 1) % LS_UI_PERF_N;
    if (p->pending_count < LS_UI_PERF_N) p->pending_count++;
}

void ls_ui_perf_frame(ls_ui_perf_t *p, uint64_t start_us, uint64_t end_us)
{
    if (end_us < start_us) return;
    uint64_t duration = end_us - start_us;
    push(&p->frame, duration);
    if (duration > 50000) p->late_frames++;
    if (end_us - p->last_frame_us >= 5000000)
        p->fps_count = p->fps_next = 0;
    p->finished_ms[p->fps_next] = (uint32_t)(end_us / 1000);
    p->fps_next = (p->fps_next + 1) % LS_UI_PERF_FPS_N;
    if (p->fps_count < LS_UI_PERF_FPS_N) p->fps_count++;
    p->last_frame_us = end_us;
    for (unsigned i = 0; i < p->pending_count; i++) {
        unsigned at = (p->pending_next + LS_UI_PERF_N - p->pending_count + i) % LS_UI_PERF_N;
        if (p->pending_us[at] <= end_us) push(&p->input, end_us - p->pending_us[at]);
    }
    p->pending_count = p->pending_next = 0;
}

void ls_ui_perf_snapshot(const ls_ui_perf_t *p, uint64_t now_us,
                         ls_ui_perf_snapshot_t *out)
{
    out->frame = p->frame;
    out->input = p->input;
    out->late_frames = p->late_frames;
    out->fps_x10 = 0;
    /* The 64-bit guard also prevents stale timestamps aliasing after 49 days. */
    if (now_us < p->last_frame_us || now_us - p->last_frame_us >= 5000000) return;
    uint32_t now_ms = (uint32_t)(now_us / 1000);
    for (unsigned i = 0; i < p->fps_count; i++)
        if (now_ms - p->finished_ms[i] < 5000) out->fps_x10 += 2;
}

uint16_t ls_ui_perf_percentile(ls_ui_perf_ring_t *r, unsigned percent)
{
    if (!r->count) return 0;
    if (percent < 1) percent = 1;
    if (percent > 100) percent = 100;
    for (unsigned i = 1; i < r->count; i++) {
        uint16_t v = r->value[i];
        unsigned j = i;
        while (j && r->value[j - 1] > v) {
            r->value[j] = r->value[j - 1];
            j--;
        }
        r->value[j] = v;
    }
    return r->value[(r->count * percent + 99) / 100 - 1];
}
