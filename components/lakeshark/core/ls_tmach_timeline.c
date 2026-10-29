/* See ls_tmach_timeline.h. */
#include "ls_tmach_timeline.h"

#include <string.h>

void ls_tm_tl_init(ls_tm_timeline_t *tl)
{
    memset(tl, 0, sizeof(*tl));
    tl->next_seq = 1;
}

int64_t ls_tm_tl_window(uint16_t kind)
{
    switch (kind) {
    case LS_DECODED_ADSB:   return 300000000;  /* one aircraft, one row while it is about */
    case LS_DECODED_AIS:    return 300000000;
    case LS_DECODED_APRS:   return 60000000;
    case LS_DECODED_P25:    return 3000000;    /* a call's later bursts       */
    case LS_DECODED_PAGE:   return 2000000;    /* a pager system's repeats    */
    case LS_DECODED_REC:    return 5000000;    /* a remote's repeated frames  */
    case LS_DECODED_DETECT: return 10000000;
    default:                return 0;
    }
}

char ls_tm_kind_letter(uint16_t kind)
{
    switch (kind) {
    case LS_DECODED_PAGE:   return 'P';
    case LS_DECODED_APRS:   return 'A';
    case LS_DECODED_AIS:    return 'V';
    case LS_DECODED_ADSB:   return 'J';
    case LS_DECODED_MESH:   return 'M';
    case LS_DECODED_P25:    return 'T';
    case LS_DECODED_REC:    return 'R';
    case LS_DECODED_DETECT: return '!';
    default:                return '?';
    }
}

const char *ls_tm_kind_name(uint16_t kind)
{
    switch (kind) {
    case LS_DECODED_PAGE:   return "PAGE";
    case LS_DECODED_APRS:   return "APRS";
    case LS_DECODED_AIS:    return "AIS";
    case LS_DECODED_ADSB:   return "ADS-B";
    case LS_DECODED_MESH:   return "MESH";
    case LS_DECODED_P25:    return "P25";
    case LS_DECODED_REC:    return "SUB-GHZ";
    case LS_DECODED_DETECT: return "DETECT";
    default:                return "EVENT";
    }
}

static bool same_thing(const ls_tm_event_t *e, const ls_decoded_t *d)
{
    if (e->kind != d->kind) return false;
    if (d->address || e->address) return e->address == d->address;
    return !strncmp(e->from, d->from, sizeof(e->from) - 1) &&
           !strncmp(e->text, d->text, sizeof(e->text) - 1);
}

static void link_clip(ls_tm_timeline_t *tl, ls_tm_event_t *e)
{
    for (uint32_t k = 0; k < tl->clips && k < LS_TM_TL_CLIPS; k++) {
        const uint32_t i = (tl->clips - 1 - k) % LS_TM_TL_CLIPS;
        if (e->at_us >= tl->clip[i].from_us && e->at_us <= tl->clip[i].to_us) {
            e->clip = tl->clip[i].no;
            e->clip_ms = (int32_t)((e->at_us - tl->clip[i].from_us) / 1000);
            return;
        }
    }
}

static void fill(ls_tm_event_t *e, const ls_decoded_t *d)
{
    if (d->from[0]) {
        memcpy(e->from, d->from, sizeof(e->from) - 1);
        e->from[sizeof(e->from) - 1] = 0;
    }
    if (d->text[0]) {
        size_t n = strnlen(d->text, sizeof(d->text));
        if (n > sizeof(e->text) - 1) n = sizeof(e->text) - 1;
        memcpy(e->text, d->text, n);
        e->text[n] = 0;
    }
    if (d->has_pos) { e->has_pos = true; e->lat = d->lat; e->lon = d->lon; }
}

uint32_t ls_tm_tl_add(ls_tm_timeline_t *tl, const ls_decoded_t *d,
                      ls_tm_tuned_fn tuned, void *ctx)
{
    if (!tl || !d || !d->kind) return 0;
    tl->version++;
    /* The same thing again, inside its window: one row, counted. */
    const int64_t win = ls_tm_tl_window(d->kind);
    if (win) {
        /* All rows: one merged for a long time keeps an old at_us. */
        for (uint32_t k = tl->n; k-- > 0;) {
            ls_tm_event_t *e = &tl->ev[k];
            const int64_t since = d->at_us - e->last_us;
            if (since > win || since < -win || !same_thing(e, d)) continue;
            if (e->count < UINT16_MAX) e->count++;
            if (d->at_us > e->last_us) e->last_us = d->at_us;
            fill(e, d);
            tl->merged++;
            return e->seq;
        }
    }
    /* A new row, in time order: usually at the end. */
    uint32_t at = tl->n;
    while (at > 0 && tl->ev[at - 1].at_us > d->at_us) at--;
    if (tl->n == LS_TM_TL_MAX) {
        if (at == 0) { tl->dropped++; return 0; }     /* older than everything kept */
        memmove(&tl->ev[0], &tl->ev[1], sizeof(tl->ev[0]) * (LS_TM_TL_MAX - 1));
        tl->n--; at--;
        tl->dropped++;
    }
    memmove(&tl->ev[at + 1], &tl->ev[at], sizeof(tl->ev[0]) * (tl->n - at));
    ls_tm_event_t *e = &tl->ev[at];
    memset(e, 0, sizeof(*e));
    e->kind = d->kind;
    e->at_us = e->last_us = d->at_us;
    e->address = d->address;
    e->count = 1;
    e->seq = tl->next_seq++;
    e->freq_hz = d->freq_hz;
    e->radio = d->radio;
    if ((!e->freq_hz || !e->radio) && tuned) {
        uint32_t f = 0; uint8_t r = 0;
        if (tuned(ctx, d->at_us, &f, &r)) {
            if (!e->freq_hz) e->freq_hz = f;
            if (!e->radio) e->radio = r;
        }
    }
    fill(e, d);
    link_clip(tl, e);
    tl->n++;
    tl->added++;
    return e->seq;
}

void ls_tm_tl_clip(ls_tm_timeline_t *tl, uint32_t no, int64_t from_us, int64_t to_us)
{
    const uint32_t i = tl->clips % LS_TM_TL_CLIPS;
    tl->clip[i].no = no;
    tl->clip[i].from_us = from_us;
    tl->clip[i].to_us = to_us;
    tl->clips++;
    for (uint32_t k = 0; k < tl->n; k++) {
        ls_tm_event_t *e = &tl->ev[k];
        if (!e->clip && e->at_us >= from_us && e->at_us <= to_us) {
            e->clip = no;
            e->clip_ms = (int32_t)((e->at_us - from_us) / 1000);
        }
    }
    tl->version++;
}

const ls_tm_event_t *ls_tm_tl_newest(const ls_tm_timeline_t *tl, uint32_t index)
{
    return index < tl->n ? &tl->ev[tl->n - 1 - index] : NULL;
}
