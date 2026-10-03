#ifndef SPEECH_ENGINE_H
#define SPEECH_ENGINE_H

/* Text to 16 kHz PCM16 through the formant synthesizer in components/klatt_tts.
   No tasks, no locks: speech.c owns the one caller. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPEECH_RATE_HZ 16000

/* Stored in NVS by number. Append only. */
typedef enum {
    SPEECH_VOICE_GLITCH = 0,
    SPEECH_VOICE_DARK   = 1,
    SPEECH_VOICE_FEMALE = 2,
    SPEECH_VOICE_COUNT
} speech_voice_t;

enum {
    SPEECH_ENGINE_OK        =  0,
    SPEECH_ENGINE_BAD_ARG   = -1,
    SPEECH_ENGINE_ARENA     = -2,  /* the unit needs more arena than there is */
    SPEECH_ENGINE_NO_PHONES = -3,  /* nothing sayable in the text */
    SPEECH_ENGINE_NO_MEMORY = -4,  /* the text front end could not allocate */
    SPEECH_ENGINE_STOPPED   = -5,  /* the sink asked to stop */
};

typedef struct {
    uint32_t units;         /* pieces the text was synthesized in */
    uint32_t samples;       /* PCM samples the sink accepted */
    size_t   arena_peak;    /* largest arena use of any unit, bytes */
} speech_engine_stats_t;

/* Returns false to stop: the rest of the text is discarded. */
typedef bool (*speech_sink_fn)(const int16_t *pcm, int n, void *ctx);

/* Builds both voices' phone tables on the heap. Call once. */
int  speech_engine_init(void);

/* Speaks `text` into `sink`. Short text goes through as one utterance; text
   that does not fit the arena is split at sentences, then between words, so
   nothing is dropped to make it fit. Each piece restarts the voice and its
   effect state. */
int  speech_engine_say(speech_voice_t voice, const char *text,
                       uint8_t *arena, size_t arena_size,
                       speech_sink_fn sink, void *ctx,
                       speech_engine_stats_t *stats);

const char *speech_voice_name(speech_voice_t voice);

#ifdef __cplusplus
}
#endif

#endif
