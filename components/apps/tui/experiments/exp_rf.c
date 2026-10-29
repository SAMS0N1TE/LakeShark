/* See exp_rf.h. */
#include "exp_rf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ls_lora.h"

void exp_fmt_clock(uint32_t s, char *out, size_t n)
{
    if (s < 6000) snprintf(out, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
    else          snprintf(out, n, "%luh%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60));
}

/* ------------------------------------------------------- band survey -- */

int exp_survey_plan(uint32_t lo_hz, uint32_t hi_hz, uint32_t caps,
                    uint32_t max_chunk_hz, exp_span_t *out, int max)
{
    exp_span_t reach[2];
    int nr = 0;
    reach[nr++] = (exp_span_t){ ls_lora_rx_min_hz(caps), ls_lora_rx_lf_max_hz(caps) };
    if ((caps & LS_LORA_CAP_RX_WIDE) && (caps & LS_LORA_CAP_BAND_1G5_2G5))
        reach[nr++] = (exp_span_t){ LS_LORA_RX_HF_MIN_HZ, ls_lora_rx_hf_max_hz(caps) };
    if (max_chunk_hz < 1000000u) max_chunk_hz = 1000000u;
    int n = 0;
    for (int r = 0; r < nr && n < max; r++) {
        const uint32_t a = lo_hz > reach[r].lo_hz ? lo_hz : reach[r].lo_hz;
        const uint32_t z = hi_hz < reach[r].hi_hz ? hi_hz : reach[r].hi_hz;
        if (z <= a || z - a < 1000000u) continue;
        const uint32_t w = z - a;
        const uint32_t pieces = (w + max_chunk_hz - 1) / max_chunk_hz;
        /* Inner edges on whole megahertz from the start, so a chunk reads as
           "300-342" and never wider than ceil(w / pieces). */
        uint32_t edge = a;
        for (uint32_t p = 0; p < pieces && n < max; p++) {
            const uint32_t next = p + 1 == pieces ? z
                : a + (uint32_t)((uint64_t)w * (p + 1) / pieces / 1000000u * 1000000u);
            out[n].lo_hz = edge;
            out[n].hi_hz = next;
            edge = next;
            n++;
        }
    }
    return n;
}

void exp_survey_chunk_init(exp_survey_chunk_t *c, exp_span_t span, int n)
{
    memset(c, 0, sizeof(*c));
    c->span = span;
    c->n = n < 2 ? 2 : n > EXP_SURVEY_BINS ? EXP_SURVEY_BINS : n;
    for (int i = 0; i < EXP_SURVEY_BINS; i++) c->bin[i].peak_dbm = -200.0f;
}

uint32_t exp_survey_bin_hz(const exp_survey_chunk_t *c, int i)
{
    if (i < 0) i = 0;
    if (i >= c->n) i = c->n - 1;
    const uint64_t span = c->span.hi_hz - c->span.lo_hz;
    return c->span.lo_hz + (uint32_t)(span * (uint64_t)i / (uint64_t)(c->n - 1));
}

static int cmp_float(const void *a, const void *b)
{
    const float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

int exp_survey_row(exp_survey_chunk_t *c, const float *row, float thresh_db,
                   uint32_t t_s, exp_survey_event_t *ev, int max_ev)
{
    float sorted[EXP_SURVEY_BINS];
    memcpy(sorted, row, sizeof(float) * (size_t)c->n);
    qsort(sorted, (size_t)c->n, sizeof(float), cmp_float);
    const float median = sorted[c->n / 2];
    /* The median, not the minimum: one quiet bin would drag the floor down,
       and a band busy across more than half its width is rare enough to
       read as a raised floor when it happens. */
    if (!c->seeded) { c->floor_dbm = median; c->seeded = true; }
    else c->floor_dbm += (median - c->floor_dbm) * 0.2f;
    c->rows++;

    const float on = c->floor_dbm + thresh_db;
    int nev = 0;
    /* A run of bins rising together this row is one event, at its peak. */
    int run_best = -1;
    for (int i = 0; i <= c->n; i++) {
        bool rose = false;
        if (i < c->n) {
            exp_survey_bin_t *b = &c->bin[i];
            if (row[i] > b->peak_dbm) b->peak_dbm = row[i];
            const bool above = row[i] >= on;
            if (above) {
                if (!b->above) b->first_s = t_s;
                b->above++;
                b->last_s = t_s;
                rose = !b->was_above;
            }
            b->was_above = above;
        }
        if (rose) {
            if (run_best < 0 || row[i] > row[run_best]) run_best = i;
        } else if (run_best >= 0) {
            if (nev < max_ev && ev) {
                ev[nev].hz = exp_survey_bin_hz(c, run_best);
                ev[nev].dbm = row[run_best];
                ev[nev].t_s = t_s;
                nev++;
            }
            run_best = -1;
        }
    }
    return nev;
}

int exp_survey_top(const exp_survey_chunk_t *c, exp_survey_emitter_t *out, int max)
{
    bool taken[EXP_SURVEY_BINS] = { false };
    int n = 0;
    while (n < max) {
        int best = -1;
        for (int i = 0; i < c->n; i++) {
            if (!c->bin[i].above || taken[i]) continue;
            if ((i > 0 && taken[i - 1] && c->bin[i - 1].above) ||
                (i + 1 < c->n && taken[i + 1] && c->bin[i + 1].above)) continue;
            if (best < 0 || c->bin[i].peak_dbm > c->bin[best].peak_dbm) best = i;
        }
        if (best < 0) break;
        taken[best] = true;
        const exp_survey_bin_t *b = &c->bin[best];
        out[n].hz = exp_survey_bin_hz(c, best);
        out[n].peak_dbm = b->peak_dbm;
        out[n].duty = c->rows ? (float)b->above / (float)c->rows : 0.0f;
        out[n].first_s = b->first_s;
        out[n].last_s = b->last_s;
        n++;
    }
    return n;
}
