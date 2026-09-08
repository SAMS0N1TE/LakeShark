#include "dmr.h"

#include <string.h>

/* ETSI TS 102 361-1 §9.1.1 - DMR sync patterns.
 * Each is 48 bits, transmitted MSB-first at the centre of a burst. */
static const uint8_t k_bs_voice[6] = { 0x75, 0x5F, 0xD7, 0xDF, 0x75, 0xF7 };
static const uint8_t k_bs_data [6] = { 0xDF, 0xF5, 0x7D, 0x75, 0xDF, 0x5D };
static const uint8_t k_ms_voice[6] = { 0x7F, 0x7D, 0x5D, 0xD5, 0x7D, 0xFD };
static const uint8_t k_ms_data [6] = { 0xD5, 0xD7, 0xF7, 0x7F, 0xD7, 0x57 };

static uint8_t popcount8(uint8_t v)
{
    v = (uint8_t)(v - ((v >> 1) & 0x55u));
    v = (uint8_t)((v & 0x33u) + ((v >> 2) & 0x33u));
    return (uint8_t)((v + (v >> 4)) & 0x0Fu);
}

static uint8_t distance48(const uint8_t *a, const uint8_t *b)
{
    unsigned d = 0;
    for (int i = 0; i < 6; i++)
        d += popcount8((uint8_t)(a[i] ^ b[i]));
    return (uint8_t)d;
}

dmr_sync_match_t dmr_sync_detect(const uint8_t bits[DMR_SYNC_BITS / 8],
                                 uint8_t max_errors)
{
    dmr_sync_match_t best = { DMR_SYNC_NONE, 48 };
    struct {
        dmr_sync_class_t id;
        const uint8_t   *pattern;
    } candidates[] = {
        { DMR_SYNC_BS_VOICE, k_bs_voice },
        { DMR_SYNC_BS_DATA,  k_bs_data  },
        { DMR_SYNC_MS_VOICE, k_ms_voice },
        { DMR_SYNC_MS_DATA,  k_ms_data  },
    };

    for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        uint8_t d = distance48(bits, candidates[i].pattern);
        if (d < best.errors) {
            best.errors   = d;
            best.class_id = candidates[i].id;
        }
    }
    if (best.errors > max_errors) {
        best.class_id = DMR_SYNC_NONE;
    }
    return best;
}
