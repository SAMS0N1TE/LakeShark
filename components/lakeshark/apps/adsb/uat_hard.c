#include "uat_hard.h"
#include "uat_stream.h"
#include "../../core/ls_spsc.h"
#include <string.h>

#define TURN (1u << 24)
static void reset_clock(uat_hard_t *s)
{
    memset(s->lane, 0, sizeof(s->lane));
    for (unsigned i = 0; i < s->lanes; ++i)
        s->lane[i].phase = TURN * i / s->lanes;
    s->have_previous = false;
    memset(s->have_sync, 0, sizeof(s->have_sync));
    s->have_valid = s->have_bad = false;
}
void uat_hard_init(uat_hard_t *s, uint32_t rate)
{
    memset(s, 0, sizeof(*s));
    if (rate >= 1041667) s->step = (uint32_t)((UINT64_C(1041667) * TURN + rate / 2) / rate);
    s->lanes = rate >= 2000000 ? 4 : 8;
    reset_clock(s);
}
static bool new_event(uint64_t now, uint64_t *at, bool *have)
{
    /* Adjacent clock hypotheses see the same sync within a few samples.
       Key acceptance to sync start, so basic and long attempts also merge. */
    if (*have && (now > *at ? now - *at : *at - now) < 16) return false;
    *at = now; *have = true;
    return true;
}
static void symbol(uat_hard_t *s, uat_hard_lane_t *p, unsigned b)
{
    if (!p->collecting) {
        p->shift = ((p->shift << 1) | b) & UINT64_C(0xFFFFFFFFF);
        if (p->searched < 36) ++p->searched;
        if (p->searched < 36) return;
        /* Complement syncs: one popcount searches both polarities. */
        unsigned dn = __builtin_popcountll(p->shift ^ UAT_DOWNLINK_SYNC);
        if (dn > 4 && dn < 32) return;
        p->up = dn >= 32;
        p->start = s->stats.samples;
        if (new_event(p->start, &s->sync_at[p->up], &s->have_sync[p->up]))
            ++s->stats.sync[p->up];
        p->collecting = true; p->bits = 0;
        memset(p->frame, 0, sizeof(p->frame));
        return;
    }
    p->frame[p->bits / 8] |= b << (7 - p->bits % 8);
    ++p->bits;
    if (!p->up && (p->bits == 240 || p->bits == 384)) {
        uat_fix_t fix;
        int corrected = 0;
        if (uat_frame_decode(p->frame, p->bits / 8, &fix, &corrected)) {
            if (new_event(p->start, &s->valid_at, &s->have_valid)) {
                if (s->have_bad && s->stats.bad &&
                    (p->start > s->bad_at ? p->start-s->bad_at : s->bad_at-p->start) < 16)
                    --s->stats.bad;
                ++s->stats.valid[fix.type != 0];
                s->stats.corrected += corrected;
                s->stats.fix = fix; s->stats.have_fix = true;
            }
            p->collecting = false;
        } else if (p->bits == 384 && new_event(p->start, &s->bad_at, &s->have_bad)) {
            /* Do not call a losing phase bad if another phase passed RS. */
            if (!s->have_valid || p->start > s->valid_at + 16 || p->start + 16 < s->valid_at)
                ++s->stats.bad;
        }
    }
    /* Uplink is intentionally only a 48-byte prefix, as in PACKET mode. */
    if (p->bits == 384) p->collecting = false;
    if (!p->collecting) p->searched = 0;
}
void uat_hard_feed(uat_hard_t *s, const uint8_t *data, size_t n, bool seam)
{
    if (seam) { ++s->stats.seams; reset_clock(s); }
    if (!s->step || !data) return;
    for (size_t j = 0; j < n; ++j) for (unsigned bit = 0; bit < 8; ++bit) {
        unsigned b = (data[j] >> (7 - bit)) & 1;
        bool capturing = false;
        for (unsigned i = 0; i < s->lanes; ++i) capturing |= s->lane[i].collecting;
        int32_t correction = 0;
        if (!capturing && s->have_previous && b != s->previous) {
            /* Midpoint of the transition interval observes the boundary.
               One slow DPLL moves all hypotheses together: independent loops
               would collapse to the same phase. Hold through a short burst
               so noisy edges cannot insert/delete payload symbols. */
            int32_t error = (int32_t)((s->lane[0].phase - s->step / 2 + TURN / 2) & (TURN - 1)) - TURN / 2;
            correction = error / 32;
        }
        for (unsigned i = 0; i < s->lanes; ++i) {
            uat_hard_lane_t *p = &s->lane[i];
            uint32_t phase = (p->phase - correction) & (TURN - 1);
            /* Nearest sample to a symbol centre; wrapping the centre window
               avoids adding/deleting a symbol when phase corrections cross 0. */
            uint32_t before = (phase + TURN / 2 - s->step / 2) & (TURN - 1);
            if (before + s->step >= TURN) symbol(s, p, b);
            p->phase = (phase + s->step) & (TURN - 1);
        }
        s->previous = b; s->have_previous = true;
        ++s->stats.samples;
    }
}
void uat_hard_ring_init(uat_hard_ring_t *r)
{
    _Static_assert(ATOMIC_INT_LOCK_FREE == 2, "RF ring indices must be lock-free");
    (void)ls_spsc_init(&r->write, &r->read); r->pending_seam = false;
}
bool uat_hard_ring_push(uat_hard_ring_t *r, const uint8_t *bytes, size_t n, bool seam)
{
    r->pending_seam |= seam;
    if (!bytes || !n || n > UAT_HARD_CHUNK) { r->pending_seam = true; return false; }
    unsigned w;
    if (!ls_spsc_write_slot(&r->write, &r->read, UAT_HARD_SLOTS, &w)) { r->pending_seam = true; return false; }
    uat_hard_chunk_t *c = &r->slot[w % UAT_HARD_SLOTS];
    c->n = n; c->seam = r->pending_seam;
    memcpy(c->bytes, bytes, n); r->pending_seam = false;
    ls_spsc_advance(&r->write, w);
    return true;
}
bool uat_hard_ring_pop(uat_hard_ring_t *r, uat_hard_chunk_t *out)
{
    unsigned rd;
    if (!ls_spsc_read_slot(&r->write, &r->read, &rd)) return false;
    *out = r->slot[rd % UAT_HARD_SLOTS];
    ls_spsc_advance(&r->read, rd);
    return true;
}


