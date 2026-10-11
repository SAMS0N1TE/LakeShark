#include "uat_stream.h"
#include <string.h>

void uat_stream_init(uat_stream_t *s, uat_downlink_fn down, uat_uplink_fn up, void *user)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->downlink = down;
    s->uplink = up;
    s->user = user;
}

static bool duplicate(uat_stream_t *s, bool up)
{
    if (s->have_accepted && s->accepted_up == up &&
        s->stats.samples - s->accepted_sample <= 4) {
        s->stats.duplicates++;
        return true;
    }
    s->have_accepted = true;
    s->accepted_up = up;
    s->accepted_sample = s->stats.samples;
    return false;
}

static void bit(uat_stream_t *s, uat_phase_t *p, unsigned value)
{
    if (!p->capturing) {
        p->shift = ((p->shift << 1) | value) & UINT64_C(0xFFFFFFFFF);
        if (p->sync_bits < 36) p->sync_bits++;
        if (p->sync_bits < 36) return;
        /* Reject noise cheaply before counting all 36 bits. Most samples
           differ in more than two of even the low sixteen sync bits. */
        uint32_t lo = (uint32_t)(p->shift ^ UAT_DOWNLINK_SYNC) & 65535;
        uint32_t a = lo & (lo - 1), b = a & (a - 1);
        uint32_t inv = lo ^ 65535, c = inv & (inv - 1), d = c & (c - 1);
        if (b && d) return;
        int dn = b ? 36 : __builtin_popcountll(p->shift ^ UAT_DOWNLINK_SYNC);
        int up = d ? 36 : __builtin_popcountll(p->shift ^ UAT_UPLINK_SYNC);
        if (dn > 2 && up > 2) return;
        p->uplink = up < dn;
        p->capturing = true;
        p->bits = 0;
        memset(p->frame, 0, sizeof(p->frame));
        if (p->uplink) s->stats.sync_up++;
        else s->stats.sync_down++;
        return;
    }
    p->frame[p->bits / 8] |= value << (7 - p->bits % 8);
    p->bits++;
    if (!p->uplink && ((p->bits == 240 && (p->frame[0] >> 3) == 0) || p->bits == 384)) {
        uat_fix_t fix;
        int errors;
        const int n = p->bits == 240 ? 30 : 48;
        if (uat_frame_decode(p->frame, n, &fix, &errors)) {
            if (!duplicate(s, false)) {
                if (fix.type == 0) s->stats.short_frames++;
                else s->stats.long_frames++;
                s->stats.corrected += errors;
                if (s->downlink) s->downlink(&fix, s->user);
            }
            p->capturing = false;
        } else if (p->bits == 384) {
            s->stats.bad_frames++;
            p->capturing = false;
        }
    } else if (p->uplink && p->bits == 4416) {
        uint8_t payload[432];
        int errors = uat_uplink_decode(p->frame, payload);
        if (errors < 0) s->stats.bad_frames++;
        else if (!duplicate(s, true)) {
            s->stats.uplink_frames++;
            s->stats.corrected += errors;
            if (s->uplink) s->uplink(payload, s->user);
        }
        p->capturing = false;
    }
    if (!p->capturing) p->sync_bits = 0;
}

void uat_stream_feed(uat_stream_t *s, const uint8_t *iq, size_t bytes)
{
    if (!s || !iq) return;
    for (size_t j = 0; j < bytes; j++) {
        if (!s->have_i) {
            s->pending_i = iq[j];
            s->have_i = true;
            continue;
        }
        int i = (int)s->pending_i - 128, q = (int)iq[j] - 128;
        s->have_i = false;
        /* sin(delta phase) has the discriminator's sign over the UAT
           +/-312.5 kHz deviation at 2 samples/bit. No atan or allocation
           in the 2.08 million sample/s loop. Two pair phases acquire either
           half-bit alignment; RS validates the resulting hard decisions. */
        int metric = s->prev_i * q - s->prev_q * i;
        if (s->have_previous)
            bit(s, &s->phase[s->stats.samples & 1], metric + s->previous_metric > 0);
        s->previous_metric = metric;
        s->prev_i = i;
        s->prev_q = q;
        s->have_previous = true;
        s->stats.samples++;
    }
}
