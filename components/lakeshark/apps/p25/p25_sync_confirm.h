#pragma once
/* When a frame whose NID passed BCH counts as signal.
 *
 * BCH(63,16) corrects up to 11 errors, so about one random word in 1,100
 * decodes to some valid codeword. With the tolerant sync hunt offering about
 * 0.8 raw syncs a second in noise, a noise frame passes the NID check every
 * 20-25 minutes, and each one lit SYNC for half a second with a random NAC.
 *
 * A valid frame counts once it is corroborated:
 *   - its NAC matches the NAC already confirmed on this tune, or
 *   - it is the second frame with the same NAC within P25_SYNC_PAIR_MS.
 * Real traffic sends frames back to back with one NAC; noise repeats a random
 * 12-bit NAC inside half a second essentially never. A new call on a channel
 * already heard is counted from its first frame; the first call on a fresh
 * tune is counted from its second, one frame late. A retune forgets the NAC.
 *
 * Only what counts as signal changes. Every valid frame is still decoded. */
#include <stdbool.h>
#include <stdint.h>

/* An HDU is 82 ms and an LDU 180 ms; one lost frame between two good ones
   still pairs. */
#define P25_SYNC_PAIR_MS 500u

typedef struct {
    uint32_t generation;
    bool     have_last;
    uint32_t last_ms;
    uint16_t last_nac;
    bool     have_nac;
    uint16_t confirmed_nac;
    uint32_t confirmed;     /* frames counted as signal   */
    uint32_t unconfirmed;   /* valid frames not (yet) counted */
    uint16_t unconfirmed_nac; /* the last of them, to tell noise from a weak
                                 real channel on the console */
} p25_sync_confirm_t;

void p25_sync_confirm_reset(p25_sync_confirm_t *c);

/* Call for every frame whose NID passed BCH. generation is the tune
   generation the frame was decoded under. Returns true when this frame
   counts as signal. */
bool p25_sync_confirm_frame(p25_sync_confirm_t *c, uint16_t nac,
                            uint32_t now_ms, uint32_t generation);
