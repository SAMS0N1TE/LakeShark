/* Speech synthesis is disabled; the settings API remains available. */

#include "sam_tts.h"

#include "esp_log.h"

#include <stdbool.h>
#include <stdint.h>

static const char *TAG = "sam_tts";

static sam_tts_voice_preset_t s_preset   = SAM_PRESET_DEFAULT;
static int                    s_lowpass  = 0;
static int                    s_lowshelf = 0;
static bool                   s_warned   = false;

esp_err_t sam_tts_init(int out_rate_hz,
                       void (*write_mono_fn)(const int16_t *samples, int n))
{
    (void)out_rate_hz;
    (void)write_mono_fn;
    /* ESP_OK, not an error: there is nothing wrong, there is simply no engine.
       Callers treat a failed init as a fault worth reporting, and a missing
       optional voice is not one. */
    ESP_LOGI(TAG, "no speech engine in this build; voice output disabled");
    return ESP_OK;
}

void sam_tts_speak(const char *text)
{
    if (!s_warned) {
        s_warned = true;
        ESP_LOGI(TAG, "speak(\"%s\") ignored - no speech engine in this build",
                 text ? text : "");
    }
}

void sam_tts_test_speak(void)
{
    sam_tts_speak("lakeshark");
}

void sam_tts_set_voice(uint8_t speed, uint8_t pitch,
                       uint8_t mouth, uint8_t throat)
{
    (void)speed; (void)pitch; (void)mouth; (void)throat;
}

void sam_tts_set_preset(sam_tts_voice_preset_t p)
{
    if (p < SAM_PRESET_COUNT) s_preset = p;
}

sam_tts_voice_preset_t sam_tts_get_preset(void)
{
    return s_preset;
}

const char *sam_tts_preset_name(sam_tts_voice_preset_t p)
{
    switch (p) {
    case SAM_PRESET_DEFAULT: return "Default";
    case SAM_PRESET_ELVIS:   return "Elvis";
    case SAM_PRESET_DEEP:    return "Deep";
    case SAM_PRESET_SOFT:    return "Soft";
    case SAM_PRESET_STUFFY:  return "Stuffy";
    default:                 return "?";
    }
}

void sam_tts_set_lowpass(int mode)  { s_lowpass = mode; }
int  sam_tts_get_lowpass(void)      { return s_lowpass; }

const char *sam_tts_lowpass_name(int mode)
{
    switch (mode) {
    case 0:  return "Off";
    case 1:  return "Soft";
    case 2:  return "Hard";
    default: return "?";
    }
}

void sam_tts_set_lowshelf(int mode) { s_lowshelf = mode; }
int  sam_tts_get_lowshelf(void)     { return s_lowshelf; }

const char *sam_tts_lowshelf_name(int mode)
{
    switch (mode) {
    case 0:  return "Off";
    case 1:  return "Warm";
    case 2:  return "Full";
    default: return "?";
    }
}
