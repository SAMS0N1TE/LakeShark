/* See ls_fsk_capture.h. No hardware, no locking - the FSK receive path is a
   single caller on one task, and the console reads it from the same one. */

#include "ls_fsk_capture.h"

#include <string.h>

static ls_fsk_capture_t s_ring[LS_FSK_CAPTURE_MAX];
static int      s_count;
static uint32_t s_evicted;

void ls_fsk_capture_reset(void)
{
    memset(s_ring, 0, sizeof(s_ring));
    s_count = 0;
    s_evicted = 0;
}

/* Same frame on the same settings. The settings are part of the identity on
   purpose: the same six bytes heard 200 kHz away is a different transmitter,
   and folding the two together would produce a capture that replays onto the
   wrong channel. */
static bool same(const ls_fsk_capture_t *a, const ls_fsk_capture_t *b)
{
    return a->freq_hz == b->freq_hz && a->bitrate == b->bitrate &&
           a->deviation_hz == b->deviation_hz &&
           a->bandwidth_hz == b->bandwidth_hz &&
           a->sync_word == b->sync_word &&
           a->preamble_bits == b->preamble_bits &&
           a->len == b->len && memcmp(a->data, b->data, a->len) == 0;
}

int ls_fsk_capture_add(const ls_fsk_capture_t *capture)
{
    if (!capture || capture->len == 0 || capture->len > LS_FSK_CAPTURE_BYTES)
        return -1;

    for (int i = 0; i < s_count; i++) {
        if (!same(&s_ring[i], capture)) continue;
        s_ring[i].heard++;
        s_ring[i].last_us = capture->first_us;
        /* The strongest sighting, not the latest: RSSI is what decides
           whether a capture is worth replaying, and the last look at a
           transmitter walking away is the least useful number of the lot. */
        if (capture->rssi_dbm > s_ring[i].rssi_dbm)
            s_ring[i].rssi_dbm = capture->rssi_dbm;
        return i;
    }

    if (s_count == LS_FSK_CAPTURE_MAX) {
        memmove(&s_ring[0], &s_ring[1], sizeof(s_ring[0]) * (LS_FSK_CAPTURE_MAX - 1));
        s_count--;
        s_evicted++;
    }

    ls_fsk_capture_t *slot = &s_ring[s_count];
    *slot = *capture;
    slot->last_us = capture->first_us;
    slot->heard = 1;
    /* Anything past len is not part of the frame and must not reach a
       replay: a caller that filled a stack struct without clearing it would
       otherwise transmit its leftovers. */
    memset(slot->data + slot->len, 0,
           sizeof(slot->data) - slot->len);
    return s_count++;
}

int ls_fsk_capture_count(void) { return s_count; }

bool ls_fsk_capture_get(int index, ls_fsk_capture_t *out)
{
    if (index < 0 || index >= s_count || !out) return false;
    *out = s_ring[index];
    return true;
}

uint32_t ls_fsk_capture_evicted(void) { return s_evicted; }

uint32_t ls_fsk_capture_period_ms(int index)
{
    if (index < 0 || index >= s_count) return 0;
    const ls_fsk_capture_t *c = &s_ring[index];
    if (c->heard < 2 || c->last_us <= c->first_us) return 0;
    /* Mean over the whole run rather than the gap between the last two: one
       missed frame doubles the last gap and would report a transmitter at
       half its real rate. */
    return (uint32_t)((c->last_us - c->first_us) / 1000 / (c->heard - 1));
}
