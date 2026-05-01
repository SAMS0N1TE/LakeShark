/*
 * audio_out.h - I2S + ES8311 codec output, shared across all apps.
 *
 * audio_write_mono() is the canonical PCM sink. SAM TTS and the tone
 * generator both call it. It blocks until DMA accepts every sample
 * (portMAX_DELAY) because partial writes silently drop samples and
 * produce chipmunk/cut-off speech.
 */
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

void audio_write_p25_voice(const int16_t *src8k, int n);

void audio_toggle_mute(void);
bool audio_is_muted(void);
void audio_volume_delta(int d);
int  audio_volume_get(void);

#ifdef __cplusplus
}
#endif

#endif
