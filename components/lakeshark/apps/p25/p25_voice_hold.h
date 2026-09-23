#pragma once
/* Voice decoded before a call has proven itself clear.
 *
 * The encryption gate muted every IMBE frame until a valid ESS arrived, and
 * the ESS arrives from the HDU or from the END of an LDU2 - after that LDU2's
 * own nine frames. A call joined without a clean HDU therefore lost its first
 * LDU1 and its first LDU2, 360 ms, and every LDU2 whose ESS failed
 * Reed-Solomon before the call was proven cost another 360 ms. On 154.7850
 * that was 4905 of 5553 muted frames in one session on 2026-09-23: the gate was
 * unsure far more often than the traffic was encrypted.
 *
 * So those frames are decoded and held here instead of dropped. When the
 * call proves clear the held voice is released ahead of the frame that
 * proved it; when it proves encrypted, ends, or changes talkgroup, the held
 * voice is discarded unplayed. Encrypted audio still never reaches the
 * speaker - the only change is that clear audio is no longer thrown away
 * while the decoder finds out.
 */
#include <stdint.h>

/* Two LDUs: LDU1 and the LDU2 that carries the ESS. Held voice beyond that is
   dropped oldest first, so a call whose ESS keeps failing is released with
   its most recent 360 ms rather than a growing backlog. */
#define P25_HOLD_MAX_SAMPLES (2 * 9 * 160)
/* Held voice older than this without a frame belongs to a call that has
   gone; it must not be released into the next one. */
#define P25_HOLD_MAX_AGE_MS  1000u

typedef struct {
    int16_t  pcm[P25_HOLD_MAX_SAMPLES];
    int      n;
    uint32_t tg;
    uint32_t last_ms;
    uint32_t held_frames;       /* cumulative: IMBE frames that entered */
    uint32_t released_frames;   /* ... and were played once proven clear */
    uint32_t discarded_frames;  /* ... and were dropped: encrypted, ended, overflow */
} p25_voice_hold_t;

void p25_voice_hold_reset(p25_voice_hold_t *h);

/* The call ended (TDU, TDULC) or a new one began (HDU): anything held is not
   this call's. */
void p25_voice_hold_end_call(p25_voice_hold_t *h);

/* Discard held voice that has aged past P25_HOLD_MAX_AGE_MS. */
void p25_voice_hold_tick(p25_voice_hold_t *h, uint32_t now_ms);

/* After each decoded frame. pcm/n is the frame's voice (8 kHz); unproven says
   it was decoded while the ESS was unknown. Writes what should be played now
   to out - held voice first, then this frame - and returns the sample count,
   0 when the frame was held or discarded. out must hold
   P25_HOLD_MAX_SAMPLES + n samples. */
int p25_voice_hold_frame(p25_voice_hold_t *h, const int16_t *pcm, int n,
                         int unproven, int ess_valid, uint8_t algid,
                         uint32_t tg, uint32_t now_ms,
                         int16_t *out, int out_cap);
