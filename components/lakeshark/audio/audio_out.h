
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

void audio_toggle_mute(void);
bool audio_is_muted(void);
void audio_volume_delta(int d);
void audio_volume_set(int v);
int  audio_volume_get(void);

void audio_out_ensure_unmuted(void);

/* Re-assert codec rate + re-prime the output ring. Call on radio-app entry to
 * avoid the first-launch underrun chop (see audio_out.c). */
void audio_out_reset(void);

uint32_t audio_drops_get(void);
uint32_t audio_underruns_get(void);
uint32_t audio_out_ring_avail(void);

#ifdef __cplusplus
}
#endif

#endif
