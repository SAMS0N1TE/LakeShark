#ifndef SPEECH_H
#define SPEECH_H

/* Spoken announcements. One worker owns the synthesizer; callers hand it
   text. Nothing here speaks over live radio: a transmission stops the
   announcement and the rest of it is discarded. */

#include "esp_err.h"
#include "speech_engine.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPEECH_TEXT_MAX 192

typedef enum {
    SPEECH_SPOKEN = 0,
    SPEECH_STOPPED,       /* live radio, mute or speech_cancel() */
    SPEECH_FAILED,        /* the engine refused the text or ran out of memory */
    SPEECH_BUSY,          /* the queue was full */
    SPEECH_UNAVAILABLE,   /* speech_init() did not succeed */
} speech_result_t;

typedef struct {
    bool     available;
    uint32_t spoken, stopped, failed, busy;
    int      last_error;          /* speech_engine code of the last failure */
    uint32_t last_first_ms;       /* request to first sample in the ring */
    uint32_t last_synth_ms;       /* time spent synthesizing, waits excluded */
    uint32_t last_audio_ms;       /* length of what was spoken */
    uint32_t last_units;
    uint32_t max_first_ms, max_synth_ms;
    size_t   arena_bytes, arena_peak;
    unsigned stack_bytes, stack_unused;
} speech_stats_t;

/* Allocates the arena and starts the worker. On any failure speech stays
   unavailable and the callers keep their tones. */
esp_err_t speech_init(void);
bool      speech_available(void);

/* Blocks until the text has been queued for playback, stopped or refused.
   Longer text is cut at the last space before SPEECH_TEXT_MAX. */
speech_result_t speech_say(const char *text);
/* Queues and returns at once; SPEECH_SPOKEN here means queued. */
speech_result_t speech_say_async(const char *text);
/* Stops what is being said and drops anything queued. */
void speech_cancel(void);

speech_voice_t speech_voice_get(void);
void           speech_voice_set(speech_voice_t v);
/* Next (+1) or previous (-1) voice, saved to settings. */
speech_voice_t speech_voice_step(int dir);

/* Speech level, 0-100 %, relative to radio audio and tones. It scales the
   speech linearly after the equalizer and never raises it; the volume of the
   codec still applies on top. In memory only; callers save it with
   settings_speech_volume_set(). */
int  speech_volume_get(void);
void speech_volume_set(int pct);

void speech_stats_get(speech_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif
