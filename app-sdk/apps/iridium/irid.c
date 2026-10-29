/* See irid.h. */
#include "irid.h"

/* An Iridium burst is at most a slot long; a level held for longer is
   something else, and becomes the floor. */
#define BURST_MAX_US 100000
/* An Iridium TDMA burst fills most of an 8.28 ms slot; anything well
   outside that is a spur or a carrier, counted apart and never shown as a
   satellite. */
#define SLOT_MIN_US  4000u
#define SLOT_MAX_US 12000u

const irid_plan_t IRID_PLAN[IRID_PLAN_N] = {
    { "SIMPLEX", 1626000000u, 1626500000u, 12 },
    { "WHOLE BAND", 1616000000u, 1626500000u, 21 },
};

uint32_t irid_ch_hz(const irid_t *e, int ch)
{
    return e->plan.lo_hz + e->spacing / 2 + e->spacing * (uint32_t)ch;
}

void irid_start(irid_t *e, int plan, int dwell_ms, int thresh_db, uint32_t rate)
{
    memset(e, 0, sizeof(*e));
    if (plan < 0 || plan >= IRID_PLAN_N) plan = 0;
    e->plan = IRID_PLAN[plan];
    e->spacing = (e->plan.hi_hz - e->plan.lo_hz) / (uint32_t)e->plan.ch;
    e->rate = rate ? rate : 1;
    e->settle_us = burst_settle_us(e->spacing);
    e->dwell_ms = dwell_ms;
    e->thresh_db = thresh_db;
    burst_level_init(&e->lvl, e->rate, e->spacing, IRID_READING_US);
    for (int c = 0; c < e->plan.ch; c++) {
        burst_det_init(&e->det[c], (float)thresh_db);
        e->det[c].max_len_us = BURST_MAX_US;
        e->ch_peak[c] = -200.0f;
    }
    burst_log_reset(&e->log);
    e->cyc_ch = -1;
}

static void note_burst(irid_t *e, int ch, const burst_t *b)
{
    if (b->len_us < SLOT_MIN_US || b->len_us > SLOT_MAX_US) { e->spurs++; return; }
    burst_log_add(&e->log, b);
    e->ch_count[ch]++;
    if (b->peak_db > e->ch_peak[ch]) e->ch_peak[ch] = b->peak_db;
    if (e->cyc_ch < 0 || b->peak_db > e->cyc_db) { e->cyc_ch = ch; e->cyc_db = b->peak_db; }
    const uint32_t s = (uint32_t)((e->t_now - e->t_start) / 1000000);
    if (e->sec_at[s % 60] != s) { e->sec_at[s % 60] = s; e->sec_n[s % 60] = 0; }
    e->sec_n[s % 60]++;
}

static void end_cycle(irid_t *e)
{
    e->cycles++;
    if (e->cyc_ch >= 0) {
        irid_trace_t *t = &e->trace[e->trace_head];
        t->t_s = (uint32_t)((e->t_now - e->t_start) / 1000000);
        t->ch = e->cyc_ch;
        t->db = e->cyc_db;
        e->trace_head = (e->trace_head + 1) % IRID_TRACE_N;
        if (e->trace_n < IRID_TRACE_N) e->trace_n++;
    }
    e->cyc_ch = -1;
}

bool irid_feed(irid_t *e, const int16_t *iq, int n, uint64_t first, uint64_t center_hz, bool gap)
{
    const uint64_t want = irid_ch_hz(e, e->ch);
    const uint64_t off = center_hz > want ? center_hz - want : want - center_hz;
    if (off * 2 >= e->spacing) return false;               /* still the last channel */
    if (gap) {
        e->gaps++;
        burst_level_restart(&e->lvl);
    }
    if (!e->listening) {
        e->listening = true;
        e->listen_from = first + (uint64_t)e->settle_us * e->rate / 1000000u;
        e->listen_until = e->listen_from + (uint64_t)e->dwell_ms * e->rate / 1000u;
        burst_level_restart(&e->lvl);
    }
    if (n > 2048) n = 2048;
    /* Only the pairs inside the listening window. */
    int a = 0, b = n;
    if (first < e->listen_from) a = e->listen_from - first >= (uint64_t)n ? n : (int)(e->listen_from - first);
    if (first + (uint64_t)n > e->listen_until)
        b = e->listen_until <= first ? 0 : (int)(e->listen_until - first);
    if (b > a) {
        e->listen_pairs += (uint64_t)(b - a);
        const int got = burst_level_feed(&e->lvl, iq + 2 * a, b - a, first + (uint64_t)a, e->tt, e->db, 2048);
        for (int k = 0; k < got; k++) {
            if (!e->started) { e->started = true; e->t_start = e->tt[k]; }
            e->t_now = e->tt[k];
            e->reads++;
            burst_t x;
            if (burst_det_feed(&e->det[e->ch], e->tt[k], e->db[k], &x)) note_burst(e, e->ch, &x);
        }
    }
    return first + (uint64_t)n >= e->listen_until;
}

uint32_t irid_hop(irid_t *e)
{
    burst_t b;
    if (burst_det_flush(&e->det[e->ch], &b)) note_burst(e, e->ch, &b);
    e->ch = (e->ch + 1) % e->plan.ch;
    if (e->ch == 0) end_cycle(e);
    e->hops++;
    e->listening = false;
    return irid_ch_hz(e, e->ch);
}

int irid_idle_lines(int plan, int dwell_ms, int thresh_db, char (*out)[IRID_LINE], int max)
{
    if (plan < 0 || plan >= IRID_PLAN_N) plan = 0;
    const irid_plan_t *p = &IRID_PLAN[plan];
    int n = 0;
    if (n < max) snprintf(out[n++], IRID_LINE, "%s %.1f-%.1f MHz in %d channels", p->name,
                          p->lo_hz / 1e6, p->hi_hz / 1e6, p->ch);
    if (n < max) snprintf(out[n++], IRID_LINE, "%d ms on each, bursts +%d dB over the floor", dwell_ms, thresh_db);
    return n;
}

int irid_lines(const irid_t *e, char (*out)[IRID_LINE], int max)
{
    static char t[IRID_LINES][IRID_LINE];
    int n = 0;
    const int nch = e->plan.ch;
    snprintf(t[n++], IRID_LINE, "%s %.1f-%.1f MHz, %d x %.1f kHz", e->plan.name,
             e->plan.lo_hz / 1e6, e->plan.hi_hz / 1e6, nch, e->spacing / 1e3);
    /* The cycle as it is, retunes and all, once there has been one. */
    const double listened = (double)e->listen_pairs / e->rate;
    const double cycle = e->cycles ? (e->t_now - e->t_start) / 1e6 / e->cycles : nch * e->dwell_ms / 1000.0;
    snprintf(t[n++], IRID_LINE, "DWELL %d ms  CYCLE %.1f s  +%d dB  %.1fk/s",
             e->dwell_ms, cycle, e->thresh_db, listened > 0 ? e->reads / listened / 1e3 : 0.0);
    const uint32_t s_now = (uint32_t)((e->t_now - e->t_start) / 1000000);
    uint32_t minute = 0;
    for (int i = 0; i < 60; i++)
        if (e->sec_at[i] + 60 > s_now && e->sec_at[i] <= s_now) minute += e->sec_n[i];
    if (e->log.count)
        snprintf(t[n++], IRID_LINE, "BURSTS %lu  LAST MIN %lu  PEAK %.0f dBFS",
                 (unsigned long)e->log.count, (unsigned long)minute, (double)e->log.peak_db);
    else snprintf(t[n++], IRID_LINE, "BURSTS 0 in %lu cycles  gaps %lu",
                  (unsigned long)e->cycles, (unsigned long)e->gaps);
    if (e->spurs)
        snprintf(t[n++], IRID_LINE, "SPURS %lu (not slot length, not counted)", (unsigned long)e->spurs);
    int best = -1;
    uint32_t most = 0;
    for (int c = 0; c < nch; c++) if (e->ch_count[c] > most) { most = e->ch_count[c]; best = c; }
    if (best >= 0)
        snprintf(t[n++], IRID_LINE, "BEST %.3f MHz  %lu bursts  %.0f dBFS",
                 irid_ch_hz(e, best) / 1e6, (unsigned long)most, (double)e->ch_peak[best]);
    /* Bursts per channel, darker to brighter. */
    static const char LEVEL[] = " .:-=+*#%@";
    char bar[IRID_CH_MAX + 1];
    for (int c = 0; c < nch; c++)
        bar[c] = most ? LEVEL[(e->ch_count[c] * 9 + most - 1) / most] : '.';
    bar[nch] = 0;
    char floor_s[8] = "--";
    if (e->det[e->ch].seeded) snprintf(floor_s, sizeof(floor_s), "%.0f", (double)e->det[e->ch].floor_db);
    snprintf(t[n++], IRID_LINE, "CH [%s] floor %s", bar, floor_s);
    char len_s[12];
    burst_rhythm_t r;
    burst_rhythm(&e->log, &r);
    burst_fmt_us(r.median_len_us, len_s, sizeof(len_s));
    if (e->log.count) snprintf(t[n++], IRID_LINE, "LEN ~%s  (Iridium slots are ~8 ms)", len_s);
    snprintf(t[n++], IRID_LINE, "TRACE  time       MHz  dBFS  channel");
    for (int k = 0; k < e->trace_n && n < IRID_LINES; k++) {
        const irid_trace_t *x = &e->trace[(e->trace_head - 1 - k + 2 * IRID_TRACE_N) % IRID_TRACE_N];
        char clock[12], where[IRID_CH_MAX + 1];
        burst_fmt_clock(x->t_s, clock, sizeof(clock));
        for (int c = 0; c < nch; c++) where[c] = c == x->ch ? '*' : '.';
        where[nch] = 0;
        snprintf(t[n++], IRID_LINE, "%6s %9.3f %5.0f  %s", clock, irid_ch_hz(e, x->ch) / 1e6, (double)x->db, where);
    }
    if (!e->trace_n && n < IRID_LINES) snprintf(t[n++], IRID_LINE, "  no burst yet");
    if (n > max) n = max;
    memcpy(out, t, (size_t)n * IRID_LINE);
    return n;
}
