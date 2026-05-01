/*
 * sam_tts.h - SAM (Software Automatic Mouth) wrapper for ESP32-P4 ES8311.
 *
 * Renders SAM's 22050 Hz 8-bit unsigned output, resamples to the I2S rate
 * (typically 16 kHz), converts to signed-16 mono, and feeds it through the
 * existing audio_write_mono() path in class_driver.c.
 */
#ifndef SAM_TTS_H
#define SAM_TTS_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate the SAM render buffer (in PSRAM) and set initial voice params.
 * Call once at startup, after heap_caps is ready.
 *
 *   out_rate_hz   - the I2S sample rate the wrapper should resample to.
 *   write_mono_fn - callback used to push signed-16 mono PCM to I2S. Must
 *                   match the audio_write_mono() signature in class_driver.c.
 *
 * Returns ESP_OK on success, ESP_ERR_NO_MEM if PSRAM alloc fails.
 */
esp_err_t sam_tts_init(int out_rate_hz,
                       void (*write_mono_fn)(const int16_t *samples, int n));

/* Speak a plain-text string through SAM. Blocks until playback is complete.
 * Safe to call from any task; serialized internally via a mutex so two events
 * don't step on the shared SAM buffer at once. Silently drops if init failed.
 *
 * Text is uppercased and wrapped with SAM's terminator internally. Keep
 * utterances under ~4 seconds (roughly 40-60 chars of plain English) -
 * anything longer will be truncated to fit the PSRAM buffer.
 */
void sam_tts_speak(const char *text);

/* Optional: adjust SAM's voice parameters at runtime. Values 0-255.
 * Defaults: speed=72, pitch=64, mouth=128, throat=128 (classic SAM voice). */
void sam_tts_set_voice(uint8_t speed, uint8_t pitch,
                       uint8_t mouth, uint8_t throat);

/* ------------------------------------------------------------------------
 * Voice-tweak API - presets and post-processing filters.
 *
 * SAM on its own sounds thin and piercing because its output is formant-
 * synthesised at 4-bit depth with most energy in the 1-3 kHz band. These
 * knobs let the user A/B a few combinations of SAM voice parameters and
 * audio-path filters to find a more pleasant sound.
 *
 * All voice tweaks are global (not per-app). The settings page owns the
 * UI for cycling through them; this API is what that UI calls.
 * ------------------------------------------------------------------------ */

/* Named voice presets. Each is a tuple of SAM's (speed, pitch, mouth,
 * throat) parameters. CUSTOM is a slot for future "user-tuned" values -
 * not currently writable through the UI, but reserved so the enum is
 * stable when we add a custom-value editor later. */
typedef enum {
    SAM_PRESET_DEFAULT = 0,     /* 72 / 64 / 128 / 128 - classic Sam       */
    SAM_PRESET_ELVIS,           /* 72 / 110 / 105 / 110 - deeper, warmer   */
    SAM_PRESET_DEEP,            /* 80 / 110 / 200 / 100 - chest-voice      */
    SAM_PRESET_SOFT,            /* 80 / 80 / 170 / 110 - gentler, slower   */
    SAM_PRESET_STUFFY,          /* 72 / 64 / 110 / 160 - more nasal weight */
    SAM_PRESET_COUNT
} sam_tts_voice_preset_t;

void                    sam_tts_set_preset(sam_tts_voice_preset_t p);
sam_tts_voice_preset_t  sam_tts_get_preset(void);
const char             *sam_tts_preset_name(sam_tts_voice_preset_t p);

/* Low-pass filter applied in the resample loop. The 4-bit SAM output has
 * lots of high-frequency quantisation hash above ~4 kHz that reads as
 * "harsh" or "sharp". A one-pole IIR tames it.
 *   0 = off     (no filtering, unchanged)
 *   1 = soft    (fc ~4 kHz, subtle high-end trim)
 *   2 = firm    (fc ~2.5 kHz, noticeably smoother / more muffled) */
void        sam_tts_set_lowpass(int mode);
int         sam_tts_get_lowpass(void);
const char *sam_tts_lowpass_name(int mode);

/* Low-shelf boost applied before the low-pass. Adds energy around the
 * low-mids to give SAM's thin output some body / warmth.
 *   0 = off
 *   1 = warm    (+3 dB below ~300 Hz)
 *   2 = warmer  (+6 dB below ~300 Hz) */
void        sam_tts_set_lowshelf(int mode);
int         sam_tts_get_lowshelf(void);
const char *sam_tts_lowshelf_name(int mode);

/* Speak a canned test phrase through SAM, using all currently-active
 * voice tweaks. Used by the settings page so the user can A/B presets
 * and filters without waiting for a plane event. */
void sam_tts_test_speak(void);

#ifdef __cplusplus
}
#endif

#endif
