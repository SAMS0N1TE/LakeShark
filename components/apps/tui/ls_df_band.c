#include "ls_df_band.h"

#include <math.h>
#include <string.h>

#define ALPHA      (1.0f / 32.0f)
#define NEW_Z      4.0f
#define NEW_DB     6.0f
#define KEEP_DB    3.0f
#define KEEP_Z     3.0f
#define KEEP_LOUD  8.0f
#define REACH      2                 /* steps a track may move between passes */

void ls_dfb_reset(ls_dfb_t *b, uint32_t lo_hz, uint32_t hi_hz)
{
    memset(b, 0, sizeof(*b));
    b->lo_hz = lo_hz; b->hi_hz = hi_hz;
    b->floor = NAN;
}

uint32_t ls_dfb_bin_hz(const ls_dfb_t *b, int bin)
{
    if (bin < 0) bin = 0;
    if (bin >= LS_DFB_BINS) bin = LS_DFB_BINS - 1;
    if (b->hi_hz <= b->lo_hz) return b->lo_hz;
    return b->lo_hz + (uint32_t)((uint64_t)(b->hi_hz - b->lo_hz) * (uint64_t)bin / (LS_DFB_BINS - 1));
}

/* The quarter-way level of the pass: what the band sounds like with the
   transmitters left out. */
static float quiet_floor(const float *x)
{
    float v[LS_DFB_BINS];
    int n = 0;
    for (int i = 0; i < LS_DFB_BINS; i++) {
        if (!isfinite(x[i])) continue;
        float t = x[i];
        int j = n++;
        while (j > 0 && v[j - 1] > t) { v[j] = v[j - 1]; j--; }
        v[j] = t;
    }
    return n ? v[n / 4] : NAN;
}

static void learn(ls_dfb_t *b, int i, float x, float alpha)
{
    const float d = x - b->mean[i];
    b->mean[i] += alpha * d;
    b->var[i] = (1 - alpha) * (b->var[i] + alpha * d * d);
}

int ls_dfb_pass(ls_dfb_t *b, const float dbm[LS_DFB_BINS], int64_t now_us, ls_dfb_read_t *out, int max)
{
    if (!b || !dbm) return 0;
    b->floor = quiet_floor(dbm);
    if (b->passes < LS_DFB_WARM) {
        /* The first passes set the background, at first quickly. */
        const float a = 1.0f / (float)(b->passes + 1);
        for (int i = 0; i < LS_DFB_BINS; i++) {
            if (!isfinite(dbm[i])) continue;
            if (!b->passes) { b->mean[i] = dbm[i]; b->var[i] = 1.0f; }
            else learn(b, i, dbm[i], a);
        }
        b->passes++;
        return 0;
    }
    b->passes++;

    float over[LS_DFB_BINS];
    bool strong[LS_DFB_BINS], heard[LS_DFB_BINS], taken[LS_DFB_BINS];
    for (int i = 0; i < LS_DFB_BINS; i++) {
        taken[i] = false;
        if (!isfinite(dbm[i])) { over[i] = -INFINITY; strong[i] = heard[i] = false; continue; }
        over[i] = dbm[i] - b->mean[i];
        const float z = over[i] / sqrtf(b->var[i] + 1.0f);
        const float loud = isfinite(b->floor) ? dbm[i] - b->floor : 0;
        strong[i] = (z > NEW_Z && over[i] > NEW_DB) || loud > LS_DFB_LOUD_DB;
        heard[i] = (over[i] > KEEP_DB && z > KEEP_Z) || loud > KEEP_LOUD;
    }

    /* Runs of raised steps, a one-step dip joined over: each run is one
       emitter at its loudest step. A strong transmitter close by raises
       the receiver's floor for megahertz around it, and that skirt is part
       of the one emitter, not several. */
    int n = 0;
    for (int i = 0; i < LS_DFB_BINS; i++) {
        if (!heard[i]) continue;
        int j = i, top = i;
        bool any_strong = false;
        while (j < LS_DFB_BINS && (heard[j] || (j + 1 < LS_DFB_BINS && heard[j + 1]))) {
            if (heard[j] && dbm[j] > dbm[top]) top = j;
            any_strong |= strong[j];
            taken[j] = true;
            j++;
        }
        const int lo = i, hi = j - 1;
        i = j - 1;
        /* Every live track inside the run: the one nearest its loudest step
           carries it on, the rest were pieces of the same skirt. */
        int keep = -1, keep_d = 0;
        for (int t = 0; t < LS_DFB_TRACKS; t++) {
            ls_dfb_track_t *tr = &b->track[t];
            if (!tr->live || tr->bin + REACH < lo || tr->bin - REACH > hi) continue;
            const int d = tr->bin > top ? tr->bin - top : top - tr->bin;
            if (keep < 0 || d < keep_d || (d == keep_d && tr->first_us < b->track[keep].first_us)) { keep = t; keep_d = d; }
        }
        for (int t = 0; t < LS_DFB_TRACKS; t++) {
            ls_dfb_track_t *tr = &b->track[t];
            if (t != keep && tr->live && tr->bin + REACH >= lo && tr->bin - REACH <= hi) tr->live = false;
        }
        if (keep >= 0) {
            ls_dfb_track_t *tr = &b->track[keep];
            tr->bin = (uint8_t)top; tr->level = dbm[top]; tr->over = over[top]; tr->last_us = now_us; tr->passes++;
            if (dbm[top] > tr->peak) tr->peak = dbm[top];
            if (n < max) out[n++] = (ls_dfb_read_t){ (uint8_t)keep, tr->hz, dbm[top] };
            continue;
        }
        if (!any_strong) continue;
        /* New: it must stand out on two passes close together before it
           takes a track, so one spike opens nothing. */
        int c = -1;
        for (int k = 0; k < LS_DFB_PENDING; k++)
            if (b->pend_us[k] && now_us - b->pend_us[k] < 3000000 &&
                (b->pend_bin[k] > top ? b->pend_bin[k] - top : top - b->pend_bin[k]) <= REACH) { c = k; break; }
        if (c < 0) {
            int old = 0;
            for (int k = 1; k < LS_DFB_PENDING; k++) if (b->pend_us[k] < b->pend_us[old]) old = k;
            b->pend_bin[old] = (uint8_t)top; b->pend_us[old] = now_us;
            continue;
        }
        b->pend_us[c] = 0;
        int slot = -1;
        int64_t stalest = now_us;
        for (int t = 0; t < LS_DFB_TRACKS; t++) {
            if (!b->track[t].live) { slot = t; break; }
            if (b->track[t].last_us < stalest && now_us - b->track[t].last_us > 30000000) {
                stalest = b->track[t].last_us; slot = t;
            }
        }
        if (slot < 0) continue;
        b->track[slot] = (ls_dfb_track_t){ .live = true, .bin = (uint8_t)top, .hz = ls_dfb_bin_hz(b, top),
                                           .level = dbm[top], .peak = dbm[top], .over = over[top],
                                           .first_us = now_us, .last_us = now_us, .passes = 1 };
        b->opened++;
        if (n < max) out[n++] = (ls_dfb_read_t){ (uint8_t)slot, b->track[slot].hz, dbm[top] };
    }
    for (int t = 0; t < LS_DFB_TRACKS; t++)
        if (b->track[t].live && now_us - b->track[t].last_us > LS_DFB_FORGET_US) b->track[t].live = false;
    /* The background learns only where nothing stands out. */
    for (int i = 0; i < LS_DFB_BINS; i++)
        if (isfinite(dbm[i]) && !heard[i] && !taken[i]) learn(b, i, dbm[i], ALPHA);
    return n;
}
