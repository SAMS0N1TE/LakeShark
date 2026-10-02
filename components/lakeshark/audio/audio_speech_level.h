#ifndef AUDIO_SPEECH_LEVEL_H
#define AUDIO_SPEECH_LEVEL_H

/* The speech level, applied where the player plays the ring.

   It used to scale the samples before they entered the ring. The equalizer
   in front of the codec ends in a leveler that holds peaks at a fixed
   ceiling with make-up gain, and the default preset has it on, so a speech
   level of 100, 75 or 50 % all came out about as loud and only the lowest
   step was heard. Scaling after the equalizer is linear whatever the preset.
   The codec volume still applies on top, and the level only ever lowers.

   Positions are byte counts into the ring, as in audio_out.c, and compare
   across wraparound. A span is the speech still in the ring. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t begin;
    uint32_t end;
} audio_speech_span_t;

/* 100 % is unity (32768); anything above is held there. */
#define AUDIO_SPEECH_UNITY 32768

static inline int32_t audio_speech_gain(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return (int32_t)(((int64_t)pct * AUDIO_SPEECH_UNITY) / 100);
}

/* Speech bytes [from, to) are about to be written; `rd` is how far the
   player has read. Mark them before the player can see them, or its first
   chunk of the announcement plays unscaled. Speech still queued is joined to
   the new speech even over a gap: whatever sits in the gap is played quieter,
   never louder. A stale `rd` (it only grows) errs the same way. */
static inline void audio_speech_span_add(audio_speech_span_t *s, uint32_t rd,
                                         uint32_t from, uint32_t to)
{
    if ((int32_t)(s->end - rd) <= 0) s->begin = from;
    s->end = to;
}

/* Scales the samples of a chunk that lie inside the span. `start` is the
   ring position of pcm[0]. */
static inline void audio_speech_level_apply(const audio_speech_span_t *s,
                                            uint32_t start, int16_t *pcm,
                                            int frames, int32_t gain)
{
    if (gain >= AUDIO_SPEECH_UNITY || frames <= 0) return;
    const uint32_t b = s->begin, e = s->end;
    for (int i = 0; i < frames; i++) {
        const uint32_t pos = start + 2u * (uint32_t)i;
        if ((int32_t)(pos - b) >= 0 && (int32_t)(e - pos) > 0)
            pcm[i] = (int16_t)(((int32_t)pcm[i] * gain) >> 15);
    }
}

#ifdef __cplusplus
}
#endif

#endif
