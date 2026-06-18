
#include "sam_tts.h"
#include "sam/sam.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>

extern int TextToPhonemes(unsigned char *);

static const char *TAG = "sam_tts";

#define SAM_BUF_BYTES  (22050 * 7)
#define SAM_NATIVE_HZ  22050

#define CHUNK_SAMPLES  512

#define LEAD_SILENCE_SAMPLES  (16000 / 32)
#define TAIL_SILENCE_SAMPLES  (16000 / 16)

static SemaphoreHandle_t tts_mutex = NULL;
static char             *sam_buf   = NULL;
static int               out_hz    = 16000;
static void            (*s_write)(const int16_t *, int) = NULL;
static bool              s_ready   = false;

typedef struct {
    const char *name;
    uint8_t speed;
    uint8_t pitch;
    uint8_t mouth;
    uint8_t throat;
} sam_preset_t;

static const sam_preset_t PRESETS[SAM_PRESET_COUNT] = {
    [SAM_PRESET_DEFAULT] = { "DEFAULT",  72,  64, 128, 128 },
    [SAM_PRESET_ELVIS]   = { "ELVIS",    72, 110, 105, 110 },
    [SAM_PRESET_DEEP]    = { "DEEP",     80, 110, 200, 100 },
    [SAM_PRESET_SOFT]    = { "SOFT",     80,  80, 170, 110 },
    [SAM_PRESET_STUFFY]  = { "STUFFY",   72,  64, 110, 160 },
};

static sam_tts_voice_preset_t s_preset   = SAM_PRESET_DEFAULT;
static int                    s_lp_mode  = 0;
static int                    s_sh_mode  = 0;

static int32_t s_lp_y    = 0;
static int32_t s_shelf_y = 0;

#define A_Q15_SHELF  3735
#define A_Q15_LP_SOFT 25955
#define A_Q15_LP_FIRM 20525

#define SHELF_EXTRA_WARM    128
#define SHELF_EXTRA_WARMER  256

static void voice_apply_preset_to_sam(sam_tts_voice_preset_t p)
{
    if (p < 0 || p >= SAM_PRESET_COUNT) return;
    const sam_preset_t *pp = &PRESETS[p];
    SetSpeed (pp->speed);
    SetPitch (pp->pitch);
    SetMouth (pp->mouth);
    SetThroat(pp->throat);
}

void sam_tts_set_preset(sam_tts_voice_preset_t p)
{
    if (p < 0 || p >= SAM_PRESET_COUNT) return;
    s_preset = p;
    voice_apply_preset_to_sam(p);
}
sam_tts_voice_preset_t sam_tts_get_preset(void) { return s_preset; }
const char *sam_tts_preset_name(sam_tts_voice_preset_t p)
{
    if (p < 0 || p >= SAM_PRESET_COUNT) return "?";
    return PRESETS[p].name;
}

void sam_tts_set_lowpass(int mode)
{
    if (mode < 0 || mode > 2) mode = 0;
    s_lp_mode = mode;
}
int sam_tts_get_lowpass(void) { return s_lp_mode; }
const char *sam_tts_lowpass_name(int mode)
{
    switch (mode) {
        case 1: return "SOFT";
        case 2: return "FIRM";
        default: return "OFF";
    }
}

void sam_tts_set_lowshelf(int mode)
{
    if (mode < 0 || mode > 2) mode = 0;
    s_sh_mode = mode;
}
int sam_tts_get_lowshelf(void) { return s_sh_mode; }
const char *sam_tts_lowshelf_name(int mode)
{
    switch (mode) {
        case 1: return "WARM";
        case 2: return "WARMER";
        default: return "OFF";
    }
}

void sam_tts_test_speak(void)
{

    sam_tts_speak("TEST. THIS IS THE A D S B VOICE CHECK.");
}

esp_err_t sam_tts_init(int out_rate_hz,
                       void (*write_mono_fn)(const int16_t *samples, int n))
{
    if (s_ready) return ESP_OK;
    if (!write_mono_fn || out_rate_hz <= 0) return ESP_ERR_INVALID_ARG;

    const char *alloc_where = "PSRAM";
    sam_buf = heap_caps_malloc(SAM_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!sam_buf) {
        ESP_LOGW(TAG, "PSRAM alloc failed, trying internal RAM");
        alloc_where = "SRAM";
        sam_buf = heap_caps_malloc(SAM_BUF_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!sam_buf) {
        ESP_LOGE(TAG, "Could not allocate %d byte SAM buffer", SAM_BUF_BYTES);
        return ESP_ERR_NO_MEM;
    }

    tts_mutex = xSemaphoreCreateMutex();
    if (!tts_mutex) {
        free(sam_buf);
        sam_buf = NULL;
        return ESP_ERR_NO_MEM;
    }

    sam_set_buffer(sam_buf);
    s_write = write_mono_fn;
    out_hz  = out_rate_hz;
    s_ready = true;

    voice_apply_preset_to_sam(s_preset);

    ESP_LOGI(TAG, "SAM TTS ready - %d B in %s, resample %d -> %d Hz",
             SAM_BUF_BYTES, alloc_where, SAM_NATIVE_HZ, out_hz);
    return ESP_OK;
}

void sam_tts_set_voice(uint8_t speed, uint8_t pitch,
                       uint8_t mouth, uint8_t throat)
{
    SetSpeed(speed);
    SetPitch(pitch);
    SetMouth(mouth);
    SetThroat(throat);
}

static void prepare_input(const char *text, unsigned char *out, size_t out_sz)
{
    size_t n = 0;

    size_t max_text = out_sz - 4;
    for (; text && *text && n < max_text; text++, n++) {
        unsigned char c = (unsigned char)*text;
        out[n] = (unsigned char)toupper(c);
    }

    out[n++] = '[';
    out[n]   = 0;
}

void sam_tts_speak(const char *text)
{
    if (!s_ready || !text || !*text) return;

    if (xSemaphoreTake(tts_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGD(TAG, "busy, dropping utterance: %s", text);
        return;
    }

    unsigned char input_buf[256];
    prepare_input(text, input_buf, sizeof(input_buf));

    if (!TextToPhonemes(input_buf)) {
        ESP_LOGW(TAG, "TextToPhonemes failed for: %s", text);
        xSemaphoreGive(tts_mutex);
        return;
    }

    SetInput(input_buf);
    if (!SAMMain()) {
        ESP_LOGW(TAG, "SAMMain failed for: %s", text);
        xSemaphoreGive(tts_mutex);
        return;
    }

    int out_samples = GetBufferLength() / 50;
    if (out_samples <= 0) { xSemaphoreGive(tts_mutex); return; }
    if (out_samples > SAM_BUF_BYTES) out_samples = SAM_BUF_BYTES;

    const uint8_t *src = (const uint8_t *)GetBuffer();

    int16_t chunk[CHUNK_SAMPLES];
    int      chunk_n = 0;

    memset(chunk, 0, sizeof(chunk));
    {
        int remaining = LEAD_SILENCE_SAMPLES;
        while (remaining > 0) {
            int push = remaining > CHUNK_SAMPLES ? CHUNK_SAMPLES : remaining;
            s_write(chunk, push);
            remaining -= push;
        }
    }

    uint64_t step = ((uint64_t)SAM_NATIVE_HZ << 16) / (uint32_t)out_hz;
    uint64_t phase = 0;

    int64_t out_total = (int64_t)out_samples * out_hz / SAM_NATIVE_HZ;

    int gain = 180;
    if (s_sh_mode == 1) gain = 150;
    else if (s_sh_mode == 2) gain = 120;

    int32_t a_lp = 0;
    if      (s_lp_mode == 1) a_lp = A_Q15_LP_SOFT;
    else if (s_lp_mode == 2) a_lp = A_Q15_LP_FIRM;

    int32_t shelf_extra = 0;
    if      (s_sh_mode == 1) shelf_extra = SHELF_EXTRA_WARM;
    else if (s_sh_mode == 2) shelf_extra = SHELF_EXTRA_WARMER;

    s_lp_y    = 0;
    s_shelf_y = 0;

    for (int64_t i = 0; i < out_total; i++) {
        uint32_t idx  = (uint32_t)(phase >> 16);
        uint32_t frac = (uint32_t)(phase & 0xFFFF);
        phase += step;

        if ((int)idx >= out_samples - 1) {

            idx = out_samples - 1;
            frac = 0;
        }

        int a = (int)src[idx]     - 128;
        int b = (int)src[idx + 1] - 128;

        int lerp = a + (int)(((int64_t)(b - a) * (int64_t)frac) >> 16);
        int32_t s = lerp * gain;

        if (shelf_extra) {
            s_shelf_y += ((s - s_shelf_y) * A_Q15_SHELF) >> 15;
            s += (s_shelf_y * shelf_extra) >> 8;
        }

        if (a_lp) {
            s_lp_y += ((s - s_lp_y) * a_lp) >> 15;
            s = s_lp_y;
        }

        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;

        chunk[chunk_n++] = (int16_t)s;
        if (chunk_n == CHUNK_SAMPLES) {
            s_write(chunk, chunk_n);
            chunk_n = 0;
        }
    }
    if (chunk_n > 0) s_write(chunk, chunk_n);

    memset(chunk, 0, sizeof(chunk));
    {
        int remaining = TAIL_SILENCE_SAMPLES;
        while (remaining > 0) {
            int push = remaining > CHUNK_SAMPLES ? CHUNK_SAMPLES : remaining;
            s_write(chunk, push);
            remaining -= push;
        }
    }

    xSemaphoreGive(tts_mutex);
}
