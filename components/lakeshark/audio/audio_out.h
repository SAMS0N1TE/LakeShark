#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_RATE_HZ 16000

esp_err_t audio_out_init(void);
/* Exclusive decoder lease. Acquire only from the UI task after parking RF.
   Release only after the decoder has stopped writing; restores radio format. */
esp_err_t audio_out_media_acquire(void);
void audio_out_media_release(void);

/* Live radio audio (FM, P25 voice): it takes the speaker from speech. */
void audio_write_mono(const int16_t *samples, int n);
/* Tones and chimes: same ring, but they neither stop nor discard speech. */
void audio_write_cue(const int16_t *samples, int n);

/* Speech into the ring, waiting for room. False means stop speaking: muted,
   live radio has the speaker, or the player stopped taking samples. Live
   audio that arrives while speech is still queued discards the speech. */
bool audio_write_speech(const int16_t *samples, int n);
/* Level of speech against everything else, 0-100 %, applied at the player
   after the equalizer; the codec volume still applies on top. Takes effect on
   speech already in the ring. */
void audio_out_speech_level_set(int pct);
/* Called before each announcement; clears a previous discard. */
void audio_out_speech_begin(void);
/* Stops audio_write_speech and drops speech still in the ring. */
void audio_out_speech_discard(void);

/* True while live radio audio (FM or P25 voice) is reaching the
   speaker. Speech yields to it instead of interleaving into the same ring. */
bool audio_out_live_active(void);
/* Speech stopped or discarded because live audio held the speaker. */
uint32_t audio_out_tts_yielded(void);

void audio_write_p25_voice(const int16_t *src8k, int n);

void audio_toggle_mute(void);
bool audio_is_muted(void);
void audio_volume_delta(int d);
void audio_volume_set(int v);
int  audio_volume_get(void);

void audio_out_ensure_unmuted(void);

void audio_out_reset(void);
void audio_out_reprime(void);

void audio_out_play_now(void);

uint32_t audio_drops_get(void);
uint32_t audio_underruns_get(void);
uint32_t audio_out_ring_avail(void);

#ifdef __cplusplus
}
#endif

#endif
