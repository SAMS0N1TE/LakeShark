#include "dmr.h"

#include <string.h>

#define WINDOW_BYTES (DMR_BURST_BITS / 8)   /* 264 bits is exactly 33 bytes */

static int rd_bit(const uint8_t *buf, unsigned int idx)
{
    return (buf[idx >> 3] >> (7u - (idx & 7u))) & 1u;
}

static void wr_bit(uint8_t *buf, unsigned int idx, int v)
{
    uint8_t mask = (uint8_t)(1u << (7u - (idx & 7u)));
    if (v) buf[idx >> 3] |= mask;
    else   buf[idx >> 3] &= (uint8_t)~mask;
}

/* Shift the whole window one bit towards the oldest end and drop `bit` in at
 * the newest.  Thirty-three bytes per bit at 9600 bit/s is about 320 kB/s of
 * memory traffic, which is nothing next to the demodulator feeding it, and a
 * plain shift keeps the window contiguous - so the burst can be handed out
 * with one memcpy and read with the same bit indices the rest of the DMR code
 * already uses.  A ring buffer would save the shift and cost that. */
static void window_push(dmr_framer_t *f, int bit)
{
    for (unsigned i = 0; i < WINDOW_BYTES - 1u; i++)
        f->window[i] = (uint8_t)((f->window[i] << 1) | (f->window[i + 1] >> 7));
    f->window[WINDOW_BYTES - 1u] =
        (uint8_t)((f->window[WINDOW_BYTES - 1u] << 1) | (bit ? 1u : 0u));
}

void dmr_framer_reset(dmr_framer_t *f, uint8_t max_sync_errors)
{
    if (!f) return;
    memset(f, 0, sizeof(*f));
    f->max_sync_errors = max_sync_errors;
}

bool dmr_framer_bit(dmr_framer_t *f, int bit, dmr_burst_frame_t *out)
{
    if (!f || !out) return false;

    window_push(f, bit);
    if (f->filled < DMR_BURST_BITS) f->filled++;
    if (f->since_hit < DMR_BURST_BITS) f->since_hit++;

    /* Nothing can be said until a whole burst has been seen: the sync is in
     * the middle of the window, so a partly filled window would match against
     * bits that were never received. */
    if (f->filled < DMR_BURST_BITS) return false;

    /* One burst, one report.  Without this a sync that survives a one-bit
     * shift would hand out the same burst twice, off by a bit. */
    if (f->since_hit < DMR_BURST_BITS) return false;

    uint8_t sync[DMR_SYNC_BITS / 8];
    memset(sync, 0, sizeof(sync));
    for (unsigned i = 0; i < DMR_SYNC_BITS; i++)
        wr_bit(sync, i, rd_bit(f->window, DMR_SYNC_OFFSET_BITS + i));

    const dmr_sync_match_t m = dmr_sync_detect(sync, f->max_sync_errors);
    if (m.class_id == DMR_SYNC_NONE) return false;

    memcpy(out->burst, f->window, WINDOW_BYTES);
    out->class_id    = m.class_id;
    out->sync_errors = m.errors;
    f->since_hit     = 0;
    return true;
}
