/* See ls_p25_desk.h. */
#include "ls_p25_desk.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

const char LS_P25D_CSV_HEAD[] = "talkgroup,name,hits,recent_hits,last_heard_s,channel_mhz,nac,seen_via,held,locked_out,allowed,encrypted";

int ls_p25d_row(ls_p25d_row_t *rows, int *n, int max, uint16_t tg)
{
    if (!tg) return -1;
    for (int i = 0; i < *n; i++)
        if (rows[i].tg == tg) return i;
    if (*n >= max) return -1;
    memset(&rows[*n], 0, sizeof(rows[0]));
    rows[*n].tg = tg;
    return (*n)++;
}

int ls_p25d_merge(ls_p25d_row_t *out, int max, const p25_tg_observed_snapshot_t *snap, uint32_t now_ms)
{
    int n = 0;
    for (unsigned i = 0; i < snap->count && i < P25_TG_OBSERVED_MAX; i++) {
        const p25_tg_observed_row_t *o = &snap->rows[i];
        const int k = ls_p25d_row(out, &n, max, o->talkgroup);
        if (k < 0) continue;
        ls_p25d_row_t *r = &out[k];
        r->hits += o->hits;
        r->sources |= o->sources;
        /* The channel and the NAC of wherever it spoke last. */
        if (!r->heard || o->last_seen_ms >= r->last_ms) { r->last_ms = o->last_seen_ms; r->hz = o->channel_hz; r->nac = o->nac; }
        r->heard = true;
        for (unsigned ago = 0; ago < P25_TG_OBSERVED_MINUTES; ago++) {
            const unsigned h = p25_tg_observed_minute_hits(o, now_ms, ago);
            const unsigned slot = P25_TG_OBSERVED_MINUTES - 1 - ago, sum = r->minute[slot] + h;
            r->minute[slot] = (uint8_t)(sum > 255 ? 255 : sum);
            r->recent += h;
        }
    }
    return n;
}

static int cmp(const ls_p25d_row_t *a, const ls_p25d_row_t *b, int col)
{
#define NUM(x, y) ((x) < (y) ? -1 : (x) > (y))
    switch (col) {
    case LS_PT_FLAGS: return NUM(b->flags, a->flags);
    case LS_PT_TG: return NUM(a->tg, b->tg);
    case LS_PT_NAME: return strcasecmp(a->name, b->name);
    case LS_PT_HITS: return NUM(b->hits, a->hits);
    case LS_PT_BUSY: return NUM(b->recent, a->recent);
    case LS_PT_LAST: return NUM(b->last_ms, a->last_ms);
    case LS_PT_CHAN: return NUM(a->hz, b->hz);
    case LS_PT_NAC: return NUM(a->nac, b->nac);
    case LS_PT_VIA: return NUM(a->sources, b->sources);
    default: return 0;
    }
#undef NUM
}

static bool has(const ls_p25d_row_t *r, int col)
{
    if (col == LS_PT_NAME) return r->name[0] != 0;
    if (col == LS_PT_TG || col == LS_PT_FLAGS) return true;
    return r->heard;
}

void ls_p25d_sort(uint8_t *order, const ls_p25d_row_t *rows, int n, int col, bool desc)
{
    for (int i = 0; i < n; i++) order[i] = (uint8_t)i;
    for (int i = 1; i < n; i++) {
        const uint8_t v = order[i];
        const ls_p25d_row_t *b = &rows[v];
        int k = i;
        while (k > 0) {
            const ls_p25d_row_t *a = &rows[order[k - 1]];
            int c;
            if (has(a, col) != has(b, col)) c = has(a, col) ? -1 : 1;
            else {
                c = cmp(a, b, col);
                if (desc) c = -c;
                /* A tie goes to the more recent, then the lower number. */
                if (!c) c = a->last_ms != b->last_ms ? (a->last_ms > b->last_ms ? -1 : 1) : (a->tg < b->tg ? -1 : a->tg > b->tg);
            }
            if (c <= 0) break;
            order[k] = order[k - 1];
            k--;
        }
        order[k] = v;
    }
}

static void age(const ls_p25d_row_t *r, uint32_t now_ms, char *out, size_t n)
{
    if (!r->heard) { snprintf(out, n, "-"); return; }
    const uint32_t s = now_ms >= r->last_ms ? (now_ms - r->last_ms) / 1000u : 0;
    if (s < 100) snprintf(out, n, "%us", (unsigned)s);
    else if (s < 6000) snprintf(out, n, "%um", (unsigned)(s / 60u));
    else snprintf(out, n, "%uh", (unsigned)(s / 3600u));
}

void ls_p25d_cell(const ls_p25d_row_t *r, int col, uint32_t now_ms, char *out, size_t n)
{
    if (!n) return;
    out[0] = 0;
    switch (col) {
    case LS_PT_FLAGS:
        snprintf(out, n, "%s%s%s%s", r->flags & LS_PF_HELD ? "H" : "", r->flags & LS_PF_LOCKED ? "L" : "",
                 r->flags & LS_PF_ALLOW ? "A" : "", r->flags & LS_PF_ENC ? "E" : "");
        break;
    case LS_PT_TG: snprintf(out, n, "%u", r->tg); break;
    case LS_PT_NAME: snprintf(out, n, "%s", r->name); break;
    case LS_PT_HITS: if (r->heard) snprintf(out, n, "%lu", (unsigned long)r->hits); break;
    case LS_PT_LAST: age(r, now_ms, out, n); break;
    case LS_PT_CHAN: if (r->heard && r->hz) snprintf(out, n, "%.4f", (double)r->hz / 1e6); break;
    case LS_PT_NAC: if (r->heard) snprintf(out, n, "%03X", r->nac); break;
    case LS_PT_VIA:
        snprintf(out, n, "%s%s%s", r->sources & P25_TG_SEEN_GRANT ? "grant " : "", r->sources & P25_TG_SEEN_HDU ? "hdu " : "",
                 r->sources & P25_TG_SEEN_LCW ? "lcw" : "");
        for (size_t k = strlen(out); k && out[k - 1] == ' '; k--) out[k - 1] = 0;
        break;
    default: break;
    }
}

bool ls_p25d_match(const ls_p25d_row_t *r, const char *needle)
{
    char num[8];
    if (!needle || !*needle) return true;
    snprintf(num, sizeof(num), "%u", r->tg);
    if (strstr(num, needle)) return true;
    const size_t k = strlen(needle);
    for (const char *c = r->name; *c; c++)
        if (!strncasecmp(c, needle, k)) return true;
    return false;
}

int ls_p25d_csv(const ls_p25d_row_t *r, uint32_t now_ms, char *out, size_t n)
{
    char last[12] = "", chan[16] = "", nac[8] = "", via[20];
    if (r->heard) {
        snprintf(last, sizeof(last), "%lu", (unsigned long)(now_ms >= r->last_ms ? (now_ms - r->last_ms) / 1000u : 0));
        if (r->hz) snprintf(chan, sizeof(chan), "%.4f", (double)r->hz / 1e6);
        snprintf(nac, sizeof(nac), "%03X", r->nac);
    }
    ls_p25d_cell(r, LS_PT_VIA, now_ms, via, sizeof(via));
    return snprintf(out, n, "%u,%s,%lu,%lu,%s,%s,%s,%s,%d,%d,%d,%d", r->tg, r->name, (unsigned long)r->hits, (unsigned long)r->recent,
                    last, chan, nac, via, !!(r->flags & LS_PF_HELD), !!(r->flags & LS_PF_LOCKED), !!(r->flags & LS_PF_ALLOW),
                    !!(r->flags & LS_PF_ENC));
}
