#include "rec_lr_ook.h"
#include <string.h>

#define FLOOR_SHIFT 6         /* floor follows about 64 quiet samples (0.5 ms) */
#define FLOOR_MIN   8

void rec_lr_ook_reset(rec_lr_ook_t *s)
{
    s->carrier = s->level = s->capturing = false;
    s->run = 0;
    s->edges = 0;
    s->span_us = 0;
    s->peak = 0;
}

void rec_lr_ook_init(rec_lr_ook_t *s, const rec_lr_ook_cfg_t *cfg)
{
    memset(s, 0, sizeof(*s));
    s->cfg = *cfg;
    if (s->cfg.min_pulse_us < REC_LR_OOK_US_PER_SAMPLE) s->cfg.min_pulse_us = REC_LR_OOK_US_PER_SAMPLE;
    if (s->cfg.gap_us < 1000) s->cfg.gap_us = 1000;
    if (s->cfg.min_edges < 2) s->cfg.min_edges = 2;
    s->floor = FLOOR_MIN;
    s->floor_acc = FLOOR_MIN << FLOOR_SHIFT;
}

/* Append a run, merging it into the last edge when the level repeats. */
static void push(rec_lr_ook_t *s, bool high, uint32_t us)
{
    if (!us) return;
    s->span_us += us;
    if (s->edges && ((s->edge[s->edges - 1] > 0) == high)) {
        s->edge[s->edges - 1] += high ? (int32_t)us : -(int32_t)us;
        return;
    }
    if (s->edges < REC_LR_OOK_MAX_EDGES)
        s->edge[s->edges++] = high ? (int32_t)us : -(int32_t)us;
}

static void finish(rec_lr_ook_t *s, rec_lr_ook_emit_t emit, void *ctx)
{
    /* The trailing quiet is not part of the signal. */
    if (s->edges && s->edge[s->edges - 1] < 0) s->edges--;
    if (s->edges >= s->cfg.min_edges && emit) emit(ctx, s->edge, s->edges, s->peak);
    s->capturing = false;
    s->edges = 0;
    s->span_us = 0;
    s->peak = 0;
}

void rec_lr_ook_feed(rec_lr_ook_t *s, const uint8_t *iq, size_t bytes,
                     rec_lr_ook_emit_t emit, void *ctx)
{
    const uint32_t min_run = s->cfg.min_pulse_us / REC_LR_OOK_US_PER_SAMPLE;
    const uint32_t gap_run = s->cfg.gap_us / REC_LR_OOK_US_PER_SAMPLE;
    for (size_t i = 0; i + 3 < bytes; i += 4) {
        const int16_t si = (int16_t)(iq[i] | (iq[i + 1] << 8));
        const int16_t sq = (int16_t)(iq[i + 2] | (iq[i + 3] << 8));
        const int32_t mag = (si < 0 ? -(int32_t)si : si) + (sq < 0 ? -(int32_t)sq : sq);

        /* The floor is only learned while nothing is on the air. */
        if (!s->carrier) {
            s->floor_acc += mag - (s->floor_acc >> FLOOR_SHIFT);
            s->floor = s->floor_acc >> FLOOR_SHIFT;
            if (s->floor < FLOOR_MIN) s->floor = FLOOR_MIN;
        }
        const int32_t on = s->floor * 4, off = s->floor * 2;
        s->carrier = s->carrier ? mag > off : mag > on;
        if (s->capturing && mag > s->peak) s->peak = mag;

        if (s->carrier == s->level) {
            s->run++;
            /* Half a second of unbroken carrier is a station, not a remote:
               learn it as the floor and stop treating it as a pulse. */
            if (s->level && s->run >= 500000u / REC_LR_OOK_US_PER_SAMPLE) {
                s->floor_acc = mag << FLOOR_SHIFT;
                s->floor = mag;
                rec_lr_ook_reset(s);
                continue;
            }
            if (s->capturing && !s->level && s->run >= gap_run) {
                finish(s, emit, ctx);
                s->run = 0;
            }
            continue;
        }
        /* A level change. A run shorter than the glitch filter is folded
           into what came before it rather than ending it. */
        if (s->capturing) {
            if (s->run >= min_run || !s->edges) push(s, s->level, s->run * REC_LR_OOK_US_PER_SAMPLE);
            else push(s, s->edges && s->edge[s->edges - 1] > 0, s->run * REC_LR_OOK_US_PER_SAMPLE);
            if (s->edges >= REC_LR_OOK_MAX_EDGES) finish(s, emit, ctx);
        } else if (s->carrier) {
            s->capturing = true;
            s->edges = 0;
            s->span_us = 0;
            s->peak = mag;
        }
        s->level = s->carrier;
        s->run = 1;
    }
}
