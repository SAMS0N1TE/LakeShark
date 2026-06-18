
#ifndef SAM_TTS_H
#define SAM_TTS_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sam_tts_init(int out_rate_hz,
                       void (*write_mono_fn)(const int16_t *samples, int n));

void sam_tts_speak(const char *text);

void sam_tts_set_voice(uint8_t speed, uint8_t pitch,
                       uint8_t mouth, uint8_t throat);

typedef enum {
    SAM_PRESET_DEFAULT = 0,
    SAM_PRESET_ELVIS,
    SAM_PRESET_DEEP,
    SAM_PRESET_SOFT,
    SAM_PRESET_STUFFY,
    SAM_PRESET_COUNT
} sam_tts_voice_preset_t;

void                    sam_tts_set_preset(sam_tts_voice_preset_t p);
sam_tts_voice_preset_t  sam_tts_get_preset(void);
const char             *sam_tts_preset_name(sam_tts_voice_preset_t p);

void        sam_tts_set_lowpass(int mode);
int         sam_tts_get_lowpass(void);
const char *sam_tts_lowpass_name(int mode);

void        sam_tts_set_lowshelf(int mode);
int         sam_tts_get_lowshelf(void);
const char *sam_tts_lowshelf_name(int mode);

void sam_tts_test_speak(void);

#ifdef __cplusplus
}
#endif

#endif
