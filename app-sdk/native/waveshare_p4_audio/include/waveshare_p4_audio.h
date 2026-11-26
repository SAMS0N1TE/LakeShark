#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

typedef struct waveshare_p4_audio waveshare_p4_audio;

/* Waveshare P4 Touch LCD 4.3 speaker. Frames contain signed 16-bit stereo PCM. */
esp_err_t waveshare_p4_audio_open(waveshare_p4_audio **out, uint32_t rate, unsigned volume);
esp_err_t waveshare_p4_audio_set_volume(waveshare_p4_audio *audio, unsigned volume);
esp_err_t waveshare_p4_audio_write(waveshare_p4_audio *audio, const int16_t *frames,
                                   size_t count, size_t *written, uint32_t timeout_ms);
void waveshare_p4_audio_close(waveshare_p4_audio *audio);
