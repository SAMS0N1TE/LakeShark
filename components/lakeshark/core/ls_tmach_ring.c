/* See ls_tmach_ring.h. */
#include "ls_tmach_ring.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------- names -- */

const char *ls_tm_fmt_name(unsigned fmt)
{
    switch (fmt) {
    case LS_TM_FMT_CU8:  return "cu8";
    case LS_TM_FMT_CS8:  return "cs8";
    case LS_TM_FMT_CS16: return "cs16";
    default:             return "?";
    }
}

unsigned ls_tm_fmt_parse(const char *name)
{
    if (!name) return LS_TM_FMT_NONE;
    if (!strcmp(name, "cu8")) return LS_TM_FMT_CU8;
    if (!strcmp(name, "cs8")) return LS_TM_FMT_CS8;
    if (!strcmp(name, "cs16")) return LS_TM_FMT_CS16;
    return LS_TM_FMT_NONE;
}

static const char *const RADIO_NAMES[] = { "", "RTL-SDR", "HackRF", "LR2021", "FILE" };

const char *ls_tm_radio_name(unsigned radio)
{
    return radio < sizeof(RADIO_NAMES) / sizeof(RADIO_NAMES[0]) ? RADIO_NAMES[radio] : "";
}

unsigned ls_tm_radio_parse(const char *name)
{
    if (!name || !*name) return LS_TM_RADIO_NONE;
    for (unsigned i = 1; i < sizeof(RADIO_NAMES) / sizeof(RADIO_NAMES[0]); i++)
        if (!strcmp(name, RADIO_NAMES[i])) return i;
    return LS_TM_RADIO_NONE;
}

/* ------------------------------------------------------------- stage -- */

void ls_tm_stage_init(ls_tm_stage_t *st, uint8_t *data)
{
    memset(st, 0, sizeof(*st));
    atomic_init(&st->w, 0);
    atomic_init(&st->r, 0);
    atomic_init(&st->drops, 0);
    atomic_init(&st->dropped_bytes, 0);
    st->data = data;
}

static void stage_publish(ls_tm_stage_t *st, int64_t now_us)
{
    if (!st->open) return;
    const unsigned pb = ls_tm_pair_bytes(st->open_src.fmt);
    if (st->open_bytes > LS_TM_SLOT_BYTES) st->open_bytes = LS_TM_SLOT_BYTES;
    /* A trailing part pair stays out; the next slot skips its other half. */
    st->open_bytes -= pb ? st->open_bytes % pb : st->open_bytes;
    st->open = false;
    const uint32_t w = atomic_load_explicit(&st->w, memory_order_relaxed);
    ls_tm_slot_meta_t *m = &st->meta[w & (LS_TM_SLOTS - 1)];
    if (!st->open_bytes) {
        if (m->gap_before) st->pending_gap = true;
        return;
    }
    m->bytes = st->open_bytes;
    m->end_us = now_us;
    atomic_store_explicit(&st->w, w + 1, memory_order_release);
}

size_t ls_tm_stage_put(ls_tm_stage_t *st, const ls_tm_src_t *src,
                       const void *data, size_t bytes, int64_t now_us)
{
    const unsigned pb = ls_tm_pair_bytes(src ? src->fmt : 0);
    if (!st || !st->data || !src || !data || !pb) return 0;
    const uint8_t *p = data;
    size_t kept = 0;
    if (st->open && !ls_tm_src_same(&st->open_src, src)) stage_publish(st, now_us);
    while (bytes) {
        if (!st->open) {
            const uint32_t w = atomic_load_explicit(&st->w, memory_order_relaxed);
            const uint32_t r = atomic_load_explicit(&st->r, memory_order_acquire);
            if (w - r >= LS_TM_SLOTS) {
                atomic_fetch_add(&st->drops, 1);
                atomic_fetch_add(&st->dropped_bytes, (uint32_t)bytes);
                st->stream_bytes += bytes;
                st->pending_gap = true;
                return kept;
            }
            ls_tm_slot_meta_t *m = &st->meta[w & (LS_TM_SLOTS - 1)];
            m->src = *src;
            m->gap_before = st->pending_gap;
            m->bytes = 0;
            st->pending_gap = false;
            st->open = true;
            st->open_bytes = 0;
            st->open_src = *src;
            /* Back on a pair boundary of the source's stream. */
            size_t skip = (pb - (size_t)(st->stream_bytes % pb)) % pb;
            if (skip > bytes) skip = bytes;
            p += skip; bytes -= skip; st->stream_bytes += skip;
            if (!bytes) break;
        }
        /* A full slot goes out before anything else is written to it; the
           room left can never come out negative. */
        if (st->open_bytes >= LS_TM_SLOT_BYTES) { stage_publish(st, now_us); continue; }
        const uint32_t w = atomic_load_explicit(&st->w, memory_order_relaxed);
        uint8_t *slot = st->data + (size_t)(w & (LS_TM_SLOTS - 1)) * LS_TM_SLOT_BYTES;
        size_t n = (size_t)(LS_TM_SLOT_BYTES - st->open_bytes);
        if (n > bytes) n = bytes;
        memcpy(slot + st->open_bytes, p, n);
        st->open_bytes += (uint32_t)n;
        st->stream_bytes += n;
        kept += n; p += n; bytes -= n;
        if (st->open_bytes >= LS_TM_SLOT_BYTES) stage_publish(st, now_us);
    }
    return kept;
}

void ls_tm_stage_flush(ls_tm_stage_t *st, bool gap, int64_t now_us)
{
    if (!st) return;
    stage_publish(st, now_us);
    if (gap) st->pending_gap = true;
}

const ls_tm_slot_meta_t *ls_tm_stage_peek(ls_tm_stage_t *st, const uint8_t **data)
{
    const uint32_t r = atomic_load_explicit(&st->r, memory_order_relaxed);
    const uint32_t w = atomic_load_explicit(&st->w, memory_order_acquire);
    if (r == w) return NULL;
    const unsigned slot = r & (LS_TM_SLOTS - 1);
    if (data) *data = st->data + (size_t)slot * LS_TM_SLOT_BYTES;
    return &st->meta[slot];
}

void ls_tm_stage_release(ls_tm_stage_t *st)
{
    const uint32_t r = atomic_load_explicit(&st->r, memory_order_relaxed);
    atomic_store_explicit(&st->r, r + 1, memory_order_release);
}

unsigned ls_tm_stage_discard(ls_tm_stage_t *st)
{
    const uint32_t r = atomic_load_explicit(&st->r, memory_order_relaxed);
    const uint32_t w = atomic_load_explicit(&st->w, memory_order_acquire);
    atomic_store_explicit(&st->r, w, memory_order_release);
    return w - r;
}

bool ls_tm_claim_enter(ls_tm_claim_t *c, unsigned who, int64_t now_us, int64_t stale_us)
{
    unsigned held = atomic_load_explicit(&c->owner, memory_order_acquire);
    while (held != who) {
        if (held && now_us - atomic_load_explicit(&c->seen_us, memory_order_relaxed) < stale_us) return false;
        if (atomic_compare_exchange_weak_explicit(&c->owner, &held, who,
                                                  memory_order_acq_rel, memory_order_acquire)) break;
    }
    bool idle = false;
    if (!atomic_compare_exchange_strong_explicit(&c->busy, &idle, true,
                                                 memory_order_acquire, memory_order_relaxed)) {
        atomic_fetch_add_explicit(&c->collisions, 1, memory_order_relaxed);
        return false;
    }
    /* Taken over while the busy flag was won: the newer owner keeps it. */
    if (atomic_load_explicit(&c->owner, memory_order_acquire) != who) {
        atomic_store_explicit(&c->busy, false, memory_order_release);
        return false;
    }
    atomic_store_explicit(&c->seen_us, now_us, memory_order_relaxed);
    return true;
}

void ls_tm_claim_leave(ls_tm_claim_t *c)
{
    atomic_store_explicit(&c->busy, false, memory_order_release);
}

/* -------------------------------------------------------------- ring -- */

void ls_tm_ring_init(ls_tm_ring_t *r, uint8_t *buf, size_t cap, uint32_t keep_hz)
{
    memset(r, 0, sizeof(*r));
    r->buf = buf;
    r->cap = cap & ~(size_t)3;
    r->keep_hz = keep_hz;
    r->need_seg = true;
}

void ls_tm_ring_reset(ls_tm_ring_t *r)
{
    ls_tm_ring_init(r, r->buf, r->cap, r->keep_hz);
}

unsigned ls_tm_decim_for(uint32_t rate_hz, uint32_t keep_hz)
{
    if (!keep_hz || rate_hz <= keep_hz) return 1;
    unsigned d = rate_hz / keep_hz;
    return d < 1 ? 1 : d > LS_TM_DECIM_MAX ? LS_TM_DECIM_MAX : d;
}

const ls_tm_seg_t *ls_tm_ring_seg_newest(const ls_tm_ring_t *r)
{
    return r->seg_count ? &r->seg[(r->seg_count - 1) % LS_TM_SEGS] : NULL;
}

const ls_tm_seg_t *ls_tm_ring_seg_at(const ls_tm_ring_t *r, int64_t t_us)
{
    const uint32_t kept = r->seg_count < LS_TM_SEGS ? r->seg_count : LS_TM_SEGS;
    for (uint32_t k = 0; k < kept; k++) {
        const ls_tm_seg_t *s = &r->seg[(r->seg_count - 1 - k) % LS_TM_SEGS];
        /* The newest one runs on to now; a little slack either side, since
           a decoder's stamp trails the samples it came from. */
        if (t_us >= s->start_us - 250000 && (k == 0 || t_us < s->end_us + 250000)) return s;
    }
    return NULL;
}

/* Pairs of `s` still in the ring, from its first. */
static uint64_t seg_first_kept(const ls_tm_ring_t *r, const ls_tm_seg_t *s)
{
    const uint64_t floor = ls_tm_ring_floor(r);
    if (floor <= s->first_byte) return 0;
    return (floor - s->first_byte + s->pair_bytes - 1) / s->pair_bytes;
}

double ls_tm_ring_held_s(const ls_tm_ring_t *r)
{
    const ls_tm_seg_t *s = ls_tm_ring_seg_newest(r);
    if (!s || !s->rate_hz) return 0;
    const uint64_t first = seg_first_kept(r, s);
    return first >= s->pairs ? 0 : (double)(s->pairs - first) / s->rate_hz;
}

bool ls_tm_ring_read(const ls_tm_ring_t *r, uint64_t at, void *out, size_t bytes)
{
    if (!r->cap || at < ls_tm_ring_floor(r) || at + bytes > r->head) return false;
    const size_t pos = (size_t)(at % r->cap);
    const size_t first = bytes < r->cap - pos ? bytes : r->cap - pos;
    memcpy(out, r->buf + pos, first);
    if (bytes > first) memcpy((uint8_t *)out + first, r->buf, bytes - first);
    return true;
}

static void tick_close(ls_tm_ring_t *r)
{
    ls_tm_tick_t *t = &r->tick[r->tick_count % LS_TM_TICKS];
    t->end_us = r->tick_end_us;
    t->flags = r->tick_flags;
    const ls_tm_seg_t *s = ls_tm_ring_seg_newest(r);
    t->seg = s ? (uint8_t)s->id : 0;
    if (r->tick_pairs) {
        t->flags |= LS_TM_TICK_DATA;
        double mean = r->tick_power / r->tick_pairs;
        double db = mean > 1e-12 ? 10.0 * log10(mean) : -120.0;
        double pk = r->tick_peak > 1e-12 ? 10.0 * log10(r->tick_peak) : -120.0;
        int lv = (int)lround((db + 100.0) * 2.0), pv = (int)lround((pk + 100.0) * 2.0);
        t->level = (uint8_t)(lv < 1 ? 1 : lv > 255 ? 255 : lv);
        t->peak = (uint8_t)(pv < 1 ? 1 : pv > 255 ? 255 : pv);
    } else {
        t->level = t->peak = 0;
    }
    r->tick_count++;
    r->tick_power = r->tick_peak = 0;
    r->tick_pairs = 0;
    r->tick_flags = 0;
}

void ls_tm_ring_break(ls_tm_ring_t *r)
{
    if (!r->need_seg) r->gaps++;
    r->need_seg = true;
    r->tick_flags |= LS_TM_TICK_GAP;
}

static void seg_start(ls_tm_ring_t *r, const ls_tm_slot_meta_t *meta, unsigned decim, size_t pairs_in)
{
    const uint8_t fmt = meta->src.fmt == LS_TM_FMT_CU8 ? LS_TM_FMT_CS8 : LS_TM_FMT_CS16;
    const unsigned pb = ls_tm_pair_bytes(fmt);
    /* Whole pairs never straddle the wrap: start on a multiple of the pair. */
    r->head = (r->head + pb - 1) / pb * pb;
    ls_tm_seg_t *s = &r->seg[r->seg_count % LS_TM_SEGS];
    memset(s, 0, sizeof(*s));
    s->src = meta->src;
    s->first_byte = r->head;
    s->decim = (uint16_t)decim;
    s->fmt = fmt;
    s->pair_bytes = (uint8_t)pb;
    s->rate_hz = meta->src.rate_hz / decim;
    s->id = ++r->seg_count;
    s->start_us = meta->end_us - (int64_t)((double)pairs_in * 1e6 / meta->src.rate_hz);
    s->end_us = s->start_us;
    memset(r->s1, 0, sizeof(r->s1)); memset(r->s2, 0, sizeof(r->s2));
    memset(r->z1, 0, sizeof(r->z1)); memset(r->z2, 0, sizeof(r->z2));
    r->phase = 0;
    r->need_seg = false;
    r->tick_flags |= LS_TM_TICK_SEG | (meta->gap_before ? LS_TM_TICK_GAP : 0);
    r->tick_need = s->rate_hz / 10 ? s->rate_hz / 10 : 1;
}

static inline int clamp_i(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Divide rounding half away from zero. */
static inline int32_t div_round(int32_t v, int32_t d) { return v >= 0 ? (v + d / 2) / d : -((-v + d / 2) / d); }

size_t ls_tm_ring_feed(ls_tm_ring_t *r, const ls_tm_slot_meta_t *meta, const uint8_t *data)
{
    const unsigned in_pb = ls_tm_pair_bytes(meta->src.fmt);
    if (!r->cap || !in_pb || !meta->src.rate_hz || (meta->src.fmt != LS_TM_FMT_CU8 &&
        meta->src.fmt != LS_TM_FMT_CS16)) return 0;
    const size_t pairs_in = meta->bytes / in_pb;
    if (!pairs_in) return 0;
    const unsigned decim = ls_tm_decim_for(meta->src.rate_hz, r->keep_hz);
    const ls_tm_seg_t *newest = ls_tm_ring_seg_newest(r);
    if (r->need_seg || !newest || meta->gap_before || !ls_tm_src_same(&newest->src, &meta->src) ||
        newest->decim != decim)
        seg_start(r, meta, decim, pairs_in);
    ls_tm_seg_t *s = &r->seg[(r->seg_count - 1) % LS_TM_SEGS];
    const unsigned pb = s->pair_bytes;

    /* The whole block or none of it: a clip being written out has the
       oldest bytes, and the block may not run over them. */
    const size_t out_max = (r->phase + pairs_in) / decim + 1;
    if (r->held && r->head + out_max * pb > r->hold + r->cap) {
        r->held_drops++;
        r->tick_flags |= LS_TM_TICK_HELD;
        ls_tm_ring_break(r);
        return 0;
    }

    const bool cu8 = meta->src.fmt == LS_TM_FMT_CU8;
    const double full = cu8 ? 128.0 * 128.0 : 32768.0 * 32768.0;
    const int32_t norm = (int32_t)(decim * decim);
    size_t written = 0;
    uint32_t s1i = r->s1[0], s1q = r->s1[1], s2i = r->s2[0], s2q = r->s2[1];
    uint32_t phase = r->phase;
    for (size_t k = 0; k < pairs_in; k++) {
        int32_t xi, xq;
        if (cu8) {
            xi = (int32_t)data[2 * k] - 128;
            xq = (int32_t)data[2 * k + 1] - 128;
        } else {
            xi = (int16_t)(data[4 * k] | data[4 * k + 1] << 8);
            xq = (int16_t)(data[4 * k + 2] | data[4 * k + 3] << 8);
        }
        int32_t yi, yq;
        if (decim == 1) {
            yi = xi; yq = xq;
        } else {
            s1i += (uint32_t)xi; s2i += s1i;
            s1q += (uint32_t)xq; s2q += s1q;
            if (++phase < decim) continue;
            phase = 0;
            const uint32_t c1i = s2i - r->z1[0], c1q = s2q - r->z1[1];
            r->z1[0] = s2i; r->z1[1] = s2q;
            yi = (int32_t)(c1i - r->z2[0]); yq = (int32_t)(c1q - r->z2[1]);
            r->z2[0] = c1i; r->z2[1] = c1q;
        }
        uint8_t *at = r->buf + (size_t)(r->head % r->cap);
        int oi, oq;
        if (cu8) {
            oi = clamp_i(div_round(yi, norm), -128, 127);
            oq = clamp_i(div_round(yq, norm), -128, 127);
            at[0] = (uint8_t)(int8_t)oi; at[1] = (uint8_t)(int8_t)oq;
        } else {
            oi = clamp_i(div_round(yi, norm), -32768, 32767);
            oq = clamp_i(div_round(yq, norm), -32768, 32767);
            at[0] = (uint8_t)oi; at[1] = (uint8_t)((uint16_t)oi >> 8);
            at[2] = (uint8_t)oq; at[3] = (uint8_t)((uint16_t)oq >> 8);
        }
        r->head += pb;
        s->pairs++;
        written++;
        const double pw = ((double)oi * oi + (double)oq * oq) / full;
        r->tick_power += pw;
        if (pw > r->tick_peak) r->tick_peak = pw;
        if (++r->tick_pairs >= r->tick_need) {
            r->tick_end_us = s->start_us + (int64_t)((double)s->pairs * 1e6 / s->rate_hz);
            tick_close(r);
        }
    }
    r->s1[0] = s1i; r->s1[1] = s1q; r->s2[0] = s2i; r->s2[1] = s2q;
    r->phase = phase;
    s->end_us = s->start_us + (int64_t)((double)s->pairs * 1e6 / s->rate_hz);
    return written;
}

/* ----------------------------------------------------- clip windows -- */

bool ls_tm_window_begin(ls_tm_ring_t *r, ls_tm_window_t *w, int64_t t_from_us, int64_t t_to_us)
{
    memset(w, 0, sizeof(*w));
    const ls_tm_seg_t *s = t_to_us ? ls_tm_ring_seg_at(r, t_to_us) : ls_tm_ring_seg_newest(r);
    if (!s || !s->pairs || !s->rate_hz) return false;
    const uint64_t kept = seg_first_kept(r, s);
    if (kept >= s->pairs) return false;
    int64_t from_p = (int64_t)((double)(t_from_us - s->start_us) * s->rate_hz / 1e6);
    int64_t to_p = t_to_us ? (int64_t)((double)(t_to_us - s->start_us) * s->rate_hz / 1e6) : (int64_t)s->pairs;
    if (from_p < (int64_t)kept) from_p = (int64_t)kept;
    if (to_p > (int64_t)s->pairs) to_p = (int64_t)s->pairs;
    if (to_p <= from_p) return false;
    w->seg = *s;
    w->from = s->first_byte + (uint64_t)from_p * s->pair_bytes;
    w->to = s->first_byte + (uint64_t)to_p * s->pair_bytes;
    w->at = w->from;
    w->start_us = s->start_us + (int64_t)((double)from_p * 1e6 / s->rate_hz);
    w->end_us = s->start_us + (int64_t)((double)to_p * 1e6 / s->rate_hz);
    w->active = true;
    r->held = true;
    r->hold = w->from;
    return true;
}

size_t ls_tm_window_step(ls_tm_ring_t *r, ls_tm_window_t *w, void *out, size_t cap)
{
    if (!w->active || w->at >= w->to) return 0;
    size_t n = (size_t)(w->to - w->at);
    if (n > cap) n = cap;
    n -= n % w->seg.pair_bytes;
    if (!n || !ls_tm_ring_read(r, w->at, out, n)) return 0;
    w->at += n;
    r->hold = w->at;
    return n;
}

void ls_tm_window_end(ls_tm_ring_t *r, ls_tm_window_t *w)
{
    w->active = false;
    r->held = false;
}
