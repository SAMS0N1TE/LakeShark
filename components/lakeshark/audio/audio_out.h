
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

void audio_write_mono(const int16_t *samples, int n);
void audio_write_mono_blocking(const int16_t *samples, int n);

void audio_write_p25_voice(const int16_t *src8k, int n);

void  audio_voice_eq_enable(bool on);
bool  audio_voice_eq_enabled(void);
void  audio_voice_eq_set_hpf(float hz);
void  audio_voice_eq_set_bass(float db);
void  audio_voice_eq_set_mid(float db);
void  audio_voice_eq_set_treble(float db);
float audio_voice_eq_hpf(void);
float audio_voice_eq_bass_db(void);
float audio_voice_eq_mid_db(void);
float audio_voice_eq_treble_db(void);

void audio_toggle_mute(void);
bool audio_is_muted(void);
void audio_volume_delta(int d);
void audio_volume_set(int v);
int  audio_volume_get(void);

void audio_out_ensure_unmuted(void);

void audio_out_reset(void);

uint32_t audio_drops_get(void);
uint32_t audio_underruns_get(void);
uint32_t audio_out_ring_avail(void);

#ifdef __cplusplus
}
#endif

#endif
