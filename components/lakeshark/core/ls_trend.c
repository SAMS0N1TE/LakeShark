/* See ls_trend.h. */
#include "ls_trend.h"

void ls_trend_reset(ls_trend_t *t)
{
    if (!t) return;
    t->head = 0;
    t->len = 0;
}

void ls_trend_push(ls_trend_t *t, uint16_t v)
{
    if (!t) return;
    t->v[t->head] = v;
    t->head = (uint8_t)((t->head + 1u) % LS_TREND_MAX);
    if (t->len < LS_TREND_MAX) t->len++;
}

int ls_trend_len(const ls_trend_t *t)
{
    return t ? (int)t->len : 0;
}

uint16_t ls_trend_at(const ls_trend_t *t, int back)
{
    if (!t || back < 0 || back >= (int)t->len) return 0;
    /* head is where the NEXT sample goes, so the newest is one before it.
       The second term keeps the sum positive for any back < LS_TREND_MAX. */
    const int i = ((int)t->head - 1 - back + 2 * LS_TREND_MAX) % LS_TREND_MAX;
    return t->v[i];
}

/* How many of the newest samples an extreme covers. Zero, negative or too
   large all mean everything held, which is what a caller asking about the
   whole history writes. */
static int span_of(const ls_trend_t *t, int count)
{
    const int len = t ? (int)t->len : 0;
    if (count <= 0 || count > len) return len;
    return count;
}

uint16_t ls_trend_min(const ls_trend_t *t, int count)
{
    const int n = span_of(t, count);
    if (n <= 0) return 0;

    uint16_t m = ls_trend_at(t, 0);
    for (int i = 1; i < n; i++) {
        const uint16_t v = ls_trend_at(t, i);
        if (v < m) m = v;
    }
    return m;
}

uint16_t ls_trend_max(const ls_trend_t *t, int count)
{
    const int n = span_of(t, count);
    if (n <= 0) return 0;
    uint16_t m = ls_trend_at(t, 0);
    for (int i = 1; i < n; i++) {
        const uint16_t v = ls_trend_at(t, i);
        if (v > m) m = v;
    }
    return m;
}

int ls_trend_level(uint16_t v, uint16_t lo, uint16_t hi)
{

    if (hi <= lo) return 4;

    if (v <= lo) return 1;
    if (v >= hi) return 8;

    const uint32_t span = (uint32_t)hi - (uint32_t)lo;
    const uint32_t up   = (uint32_t)v - (uint32_t)lo;
    int level = 1 + (int)((up * 7u + span / 2u) / span);
    if (level < 1) level = 1;
    if (level > 8) level = 8;
    return level;
}
