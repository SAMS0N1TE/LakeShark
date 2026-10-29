/* See meter.h. */
#include "meter.h"

static void span_add(meter_span_t *s, float v)
{
    if (!s->n || v < s->lo) s->lo = v;
    if (!s->n || v > s->hi) s->hi = v;
    s->sum += v;
    s->n++;
}

void meter_start(meter_t *m, uint32_t hz, uint32_t rate)
{
    memset(m, 0, sizeof(*m));
    burst_level_init(&m->lvl, rate, METER_FILTER_HZ, METER_READING_US);
    m->hz = hz;
}

void meter_feed(meter_t *m, const int16_t *iq, int n, uint64_t first, bool gap)
{
    if (gap) {
        m->gaps++;
        burst_level_restart(&m->lvl);
    }
    /* A block holds at most a few readings: the smallest is 2 ms. */
    while (n > 0) {
        const int part = n > 512 ? 512 : n;
        const int got = burst_level_feed(&m->lvl, iq, part, first, m->tt, m->db, 32);
        for (int k = 0; k < got; k++) {
            const int64_t t = m->tt[k];
            if (!m->started) { m->started = true; m->acc_t0 = t; }
            /* A reading a second or more after the first of the one being
               gathered closes that second. */
            if (t - m->acc_t0 >= 1000000) {
                m->second = m->acc;
                memset(&m->acc, 0, sizeof(m->acc));
                m->acc_t0 = t;
            }
            span_add(&m->acc, m->db[k]);
            span_add(&m->total, m->db[k]);
            m->now = m->db[k];
            m->have_now = true;
            m->reads++;
        }
        iq += 2 * part;
        first += (uint64_t)part;
        n -= part;
    }
}

static void span_line(char *out, const char *label, const meter_span_t *s)
{
    if (!s->n) snprintf(out, METER_LINE, "%-7s --", label);
    else snprintf(out, METER_LINE, "%-7s avg %6.1f min %6.1f max %6.1f", label,
                  s->sum / s->n, (double)s->lo, (double)s->hi);
}

int meter_lines(const meter_t *m, uint32_t next_hz, char (*out)[METER_LINE], int max)
{
    const uint32_t hz = m && m->hz ? m->hz : next_hz;
    int n = 0;
    if (n < max) snprintf(out[n++], METER_LINE, "FREQ    %.4f MHz", hz / 1e6);
    if (n < max) {
        if (m && m->have_now) snprintf(out[n++], METER_LINE, "NOW     %6.1f dBFS", (double)m->now);
        else snprintf(out[n++], METER_LINE, "NOW     no reading yet");
    }
    static const meter_span_t none;
    if (n < max) span_line(out[n++], "1 SEC", m ? &m->second : &none);
    if (n < max) span_line(out[n++], "START", m ? &m->total : &none);
    if (n < max) {
        float f = m && m->have_now ? (m->now - METER_BAR_LO) / (METER_BAR_HI - METER_BAR_LO) : 0.0f;
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        const int fill = (int)(f * METER_BAR_CELLS + 0.5f);
        char bar[METER_BAR_CELLS + 1];
        for (int i = 0; i < METER_BAR_CELLS; i++) bar[i] = i < fill ? '#' : '.';
        bar[METER_BAR_CELLS] = 0;
        snprintf(out[n++], METER_LINE, "%.0f [%s] %.0f", (double)METER_BAR_LO, bar, (double)METER_BAR_HI);
    }
    if (n < max)
        snprintf(out[n++], METER_LINE, "READS   %lu  gaps %lu", (unsigned long)(m ? m->reads : 0),
                 (unsigned long)(m ? m->gaps : 0));
    return n;
}
