/* See scope.h. */
#include "scope.h"

#define STRIP_CELLS       36
#define STRIP_SPAN_US     2000000
#define STRIP_SPAN_MIN_US 50000
#define STRIP_SPAN_MAX_US 10000000

void scope_start(scope_t *s, uint32_t hz, uint32_t rate, uint32_t filter_hz, int thresh_db)
{
    memset(s, 0, sizeof(*s));
    burst_level_init(&s->lvl, rate, filter_hz, SCOPE_READING_US);
    burst_det_init(&s->det, (float)thresh_db);
    burst_log_reset(&s->log);
    s->hz = hz;
    s->filter_hz = rate / (uint32_t)s->lvl.filt;
    s->thresh_db = thresh_db;
}

void scope_feed(scope_t *s, const int16_t *iq, int n, uint64_t first, bool gap)
{
    if (gap) {
        s->gaps++;
        burst_level_restart(&s->lvl);
    }
    if (n > 2048) n = 2048;
    const int got = burst_level_feed(&s->lvl, iq, n, first, s->tt, s->db, 2048);
    for (int k = 0; k < got; k++) {
        if (!s->started) { s->started = true; s->t_start = s->tt[k]; }
        s->t_now = s->tt[k];
        s->now_db = s->db[k];
        s->have_now = true;
        s->reads++;
        burst_t b;
        if (burst_det_feed(&s->det, s->tt[k], s->db[k], &b)) burst_log_add(&s->log, &b);
    }
}

int scope_idle_lines(uint32_t hz, uint32_t filter_hz, int thresh_db, char (*out)[SCOPE_LINE], int max)
{
    int n = 0;
    if (n < max) snprintf(out[n++], SCOPE_LINE, "FREQ %.4f MHz  FILTER %.1f kHz  +%d dB",
                          hz / 1e6, filter_hz / 1e3, thresh_db);
    if (n < max) snprintf(out[n++], SCOPE_LINE, "Times every burst over the floor there.");
    return n;
}

int scope_lines(const scope_t *s, char (*out)[SCOPE_LINE], int max)
{
    static char t[SCOPE_LINES][SCOPE_LINE];
    int n = 0;
    const int64_t now = s->t_now;
    const double secs = (now - s->t_start) / 1e6;
    snprintf(t[n++], SCOPE_LINE, "FREQ %.4f MHz  FILTER %.1f kHz  +%d dB",
             s->hz / 1e6, s->filter_hz / 1e3, s->thresh_db);
    if (s->reads)
        snprintf(t[n++], SCOPE_LINE, "RATE %.1fk/s, %luus a read  FLOOR %.0f dBFS",
                 secs > 0 ? s->reads / secs / 1e3 : 0.0, (unsigned long)burst_level_us(&s->lvl),
                 (double)s->det.floor_db);
    else snprintf(t[n++], SCOPE_LINE, "RATE --  FLOOR --");
    if (s->have_now)
        snprintf(t[n++], SCOPE_LINE, "NOW %.0f dBFS  BURSTS %lu  PEAK %.0f", (double)s->now_db,
                 (unsigned long)s->log.count, s->log.count ? (double)s->log.peak_db : 0.0);
    else snprintf(t[n++], SCOPE_LINE, "NOW no reading yet  gaps %lu", (unsigned long)s->gaps);
    /* The strip spans eight periods once there is a rhythm, so each burst
       and each gap gets cells of its own; two seconds until then. What the
       ring no longer holds is blank, not quiet. */
    burst_rhythm_t r;
    burst_rhythm(&s->log, &r);
    int64_t span = STRIP_SPAN_US;
    if (r.mode_us) {
        span = (int64_t)r.mode_us * 8;
        if (span < STRIP_SPAN_MIN_US) span = STRIP_SPAN_MIN_US;
        if (span > STRIP_SPAN_MAX_US) span = STRIP_SPAN_MAX_US;
    }
    int64_t since = s->t_start;
    if (s->log.n == BURST_RING) {
        const int64_t oldest = burst_log_at(&s->log, s->log.n - 1)->start_us;
        if (oldest > since) since = oldest;
    }
    char strip[STRIP_CELLS + 1], span_s[12];
    burst_strip(&s->log, now, span, since, strip, STRIP_CELLS);
    burst_fmt_us((uint32_t)span, span_s, sizeof(span_s));
    snprintf(t[n++], SCOPE_LINE, "[%s] %s", strip, span_s);

    uint32_t len[BURST_LEN_BUCKETS], gap[BURST_GAP_BUCKETS];
    burst_hist(&s->log, len, gap);
    char a[SCOPE_LINE], b[SCOPE_LINE];
    int pa = snprintf(a, sizeof(a), "LEN "), pb = snprintf(b, sizeof(b), "    ");
    for (int i = 0; i < BURST_LEN_BUCKETS; i++) {
        pa += snprintf(a + pa, sizeof(a) - pa, "%4s", burst_len_label(i));
        pb += snprintf(b + pb, sizeof(b) - pb, "%4lu", (unsigned long)len[i]);
    }
    snprintf(t[n++], SCOPE_LINE, "%s", a);
    snprintf(t[n++], SCOPE_LINE, "%s", b);
    pa = snprintf(a, sizeof(a), "GAP ");
    pb = snprintf(b, sizeof(b), "    ");
    for (int i = 0; i < BURST_GAP_BUCKETS; i++) {
        pa += snprintf(a + pa, sizeof(a) - pa, "%4s", burst_gap_label(i));
        pb += snprintf(b + pb, sizeof(b) - pb, "%4lu", (unsigned long)gap[i]);
    }
    snprintf(t[n++], SCOPE_LINE, "%s", a);
    snprintf(t[n++], SCOPE_LINE, "%s", b);

    char mode[12], med[12];
    burst_fmt_us(r.mode_us, mode, sizeof(mode));
    burst_fmt_us(r.median_len_us, med, sizeof(med));
    if (r.mode_us)
        snprintf(t[n++], SCOPE_LINE, "EVERY %s: %d of %d gaps fit, len ~%s", mode, r.period_hits, r.intervals, med);
    else snprintf(t[n++], SCOPE_LINE, "EVERY -- (too few bursts yet)");

    /* The newest, three to a line. */
    for (int row = 0; row < 2 && n < SCOPE_LINES; row++) {
        int p = snprintf(a, sizeof(a), row ? "     " : "LAST ");
        int shown = 0;
        for (int k = row * 3; k < row * 3 + 3; k++) {
            const burst_t *x = burst_log_at(&s->log, k);
            if (!x) break;
            char l[12];
            burst_fmt_us(x->len_us, l, sizeof(l));
            p += snprintf(a + p, sizeof(a) - p, "%s%s %.0f%s", shown ? " | " : "", l,
                          (double)x->peak_db, x->cut ? "~" : "");
            shown++;
        }
        if (!shown && row) break;
        if (!shown) snprintf(a + p, sizeof(a) - p, "none yet");
        snprintf(t[n++], SCOPE_LINE, "%s", a);
    }
    if (n > max) n = max;
    memcpy(out, t, (size_t)n * SCOPE_LINE);
    return n;
}
