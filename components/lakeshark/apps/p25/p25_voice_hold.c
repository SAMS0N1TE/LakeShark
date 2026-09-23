#include "p25_voice_hold.h"
#include <string.h>

#define P25_ALGID_CLEAR 0x80

void p25_voice_hold_reset(p25_voice_hold_t *h)
{
    memset(h, 0, sizeof(*h));
}

static void discard(p25_voice_hold_t *h)
{
    h->discarded_frames += (uint32_t)(h->n / 160);
    h->n = 0;
    h->tg = 0;
}

void p25_voice_hold_end_call(p25_voice_hold_t *h)
{
    if (h->n) discard(h);
}

void p25_voice_hold_tick(p25_voice_hold_t *h, uint32_t now_ms)
{
    if (h->n && now_ms - h->last_ms > P25_HOLD_MAX_AGE_MS) discard(h);
}

static int emit(int16_t *out, int at, int cap, const int16_t *pcm, int n)
{
    if (n > cap - at) n = cap - at;
    if (n > 0) memcpy(out + at, pcm, (size_t)n * sizeof(int16_t));
    return n > 0 ? at + n : at;
}

int p25_voice_hold_frame(p25_voice_hold_t *h, const int16_t *pcm, int n,
                         int unproven, int ess_valid, uint8_t algid,
                         uint32_t tg, uint32_t now_ms,
                         int16_t *out, int out_cap)
{
    if (n < 0) n = 0;
    /* A talkgroup change is a different call; its predecessor's unproven
       voice must not be released on the strength of this one's ESS. */
    if (h->n && tg && h->tg && tg != h->tg) discard(h);

    if (ess_valid && algid == P25_ALGID_CLEAR) {
        int w = emit(out, 0, out_cap, h->pcm, h->n);
        h->released_frames += (uint32_t)(h->n / 160);
        h->n = 0;
        h->tg = 0;
        return emit(out, w, out_cap, pcm, n);
    }
    if (ess_valid) {
        /* Proven encrypted. Whatever was held, and anything this frame
           decoded before its ESS was read, is ciphertext. */
        discard(h);
        if (unproven) {
            h->held_frames += (uint32_t)(n / 160);
            h->discarded_frames += (uint32_t)(n / 160);
            return 0;
        }
        return emit(out, 0, out_cap, pcm, n);  /* the operator unmuted encrypted */
    }
    if (!unproven) return emit(out, 0, out_cap, pcm, n);
    if (!n) return 0;

    h->held_frames += (uint32_t)(n / 160);
    if (n > P25_HOLD_MAX_SAMPLES) {
        h->discarded_frames += (uint32_t)((n - P25_HOLD_MAX_SAMPLES) / 160);
        pcm += n - P25_HOLD_MAX_SAMPLES;
        n = P25_HOLD_MAX_SAMPLES;
    }
    int over = h->n + n - P25_HOLD_MAX_SAMPLES;
    if (over > 0) {
        memmove(h->pcm, h->pcm + over, (size_t)(h->n - over) * sizeof(int16_t));
        h->n -= over;
        h->discarded_frames += (uint32_t)(over / 160);
    }
    memcpy(h->pcm + h->n, pcm, (size_t)n * sizeof(int16_t));
    h->n += n;
    if (tg) h->tg = tg;
    h->last_ms = now_ms;
    return 0;
}
