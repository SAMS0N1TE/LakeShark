#include "tone.h"
#include "audio_out.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>

void audio_tone(float freq, float dur_s, float amp)
{
    int total = (int)(AUDIO_RATE_HZ * dur_s);
    static int16_t buf[256];

    audio_out_play_now();
    for (int i = 0; i < total; i += 256) {
        int chunk = total - i;
        if (chunk > 256) chunk = 256;
        for (int j = 0; j < chunk; j++) {
            float t = (float)(i + j) / AUDIO_RATE_HZ;
            buf[j] = (int16_t)(sinf(2.0f * (float)M_PI * freq * t) * amp);
        }
        audio_write_mono(buf, chunk);
    }
}

void snd_p25_chirp(void)
{
    static int16_t buf[2048];
    const float f1 = 1760.0f, f2 = 2350.0f;
    const float amp = 4800.0f;
    const int n1 = (int)(AUDIO_RATE_HZ * 0.045f);
    const int n2 = (int)(AUDIO_RATE_HZ * 0.050f);
    int total = n1 + n2;
    if (total > 2048) total = 2048;
    const int fade_in  = (int)(AUDIO_RATE_HZ * 0.005f);
    const int fade_out = (int)(AUDIO_RATE_HZ * 0.010f);

    audio_out_play_now();

    float ph = 0.0f;
    for (int i = 0; i < total; i++) {
        float f = (i < n1) ? f1 : f2;
        ph += 2.0f * (float)M_PI * f / (float)AUDIO_RATE_HZ;
        if (ph > 2.0f * (float)M_PI) ph -= 2.0f * (float)M_PI;
        float env = 1.0f;
        if (i < fade_in)                env = (float)i / (float)fade_in;
        else if (i >= total - fade_out) env = (float)(total - i) / (float)fade_out;
        buf[i] = (int16_t)(sinf(ph) * amp * env);
    }
    audio_write_mono(buf, total);
}

void snd_boot(void)
{
    audio_tone(440.0f, 0.06f, 7000.0f);
    vTaskDelay(pdMS_TO_TICKS(20));
    audio_tone(660.0f, 0.06f, 7000.0f);
    vTaskDelay(pdMS_TO_TICKS(20));
    audio_tone(880.0f, 0.10f, 7000.0f);
}

void snd_new_contact(void)
{
    audio_tone(1200.0f, 0.03f, 5000.0f);
    vTaskDelay(pdMS_TO_TICKS(40));
    audio_tone(1200.0f, 0.03f, 5000.0f);
}

void snd_lost_contact(void)
{
    audio_tone(800.0f, 0.05f, 4000.0f);
    vTaskDelay(pdMS_TO_TICKS(15));
    audio_tone(600.0f, 0.08f, 3000.0f);
}

void snd_position_fix(void)
{
    audio_tone(1800.0f, 0.02f, 3000.0f);
}

static void moto_tone(float freq, float dur_s, float amp)
{
    int total = (int)(AUDIO_RATE_HZ * dur_s);
    static int16_t buf[256];
    const int fade = (int)(AUDIO_RATE_HZ * 0.003f);

    audio_out_play_now();

    float ph = 0.0f, ph3 = 0.0f;
    for (int i = 0; i < total; i += 256) {
        int chunk = total - i;
        if (chunk > 256) chunk = 256;
        for (int j = 0; j < chunk; j++) {
            int n = i + j;
            ph  += 2.0f * (float)M_PI * freq         / (float)AUDIO_RATE_HZ;
            ph3 += 2.0f * (float)M_PI * freq * 3.0f  / (float)AUDIO_RATE_HZ;
            if (ph  > 2.0f * (float)M_PI) ph  -= 2.0f * (float)M_PI;
            if (ph3 > 2.0f * (float)M_PI) ph3 -= 2.0f * (float)M_PI;

            float env = 1.0f;
            if (n < fade)                env = (float)n / (float)fade;
            else if (n >= total - fade)  env = (float)(total - n) / (float)fade;

            float s = sinf(ph) + 0.18f * sinf(ph3);
            buf[j] = (int16_t)(s * amp * env * 0.85f);
        }
        audio_write_mono(buf, chunk);
    }
}

void snd_moto_power_on(void)
{

    moto_tone(784.0f,  0.070f, 7000.0f);
    moto_tone(1046.5f, 0.070f, 7200.0f);
    moto_tone(1318.5f, 0.130f, 7600.0f);
}

void snd_moto_alert(void)
{

    for (int i = 0; i < 2; i++) {
        moto_tone(1244.5f, 0.090f, 7200.0f);
        moto_tone(1661.0f, 0.090f, 7200.0f);
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}

void snd_moto_bonk(void)
{

    moto_tone(311.1f, 0.110f, 6500.0f);
    vTaskDelay(pdMS_TO_TICKS(45));
    moto_tone(233.1f, 0.170f, 6500.0f);
}

void snd_moto_full(void)
{
    snd_moto_power_on();
    vTaskDelay(pdMS_TO_TICKS(260));
    snd_moto_alert();
    vTaskDelay(pdMS_TO_TICKS(260));
    snd_moto_bonk();
}

#define TEST_AMP        8000.0f
#define TEST_CHUNK      160
#define TEST_PACE_MS    8
#define TEST_STACK_WORDS (3072 / sizeof(StackType_t))

static const char *TEST_NAMES[SND_TEST_COUNT] = {
    "sweep", "bass", "noise", "tone", "chirp", "moto"
};

static QueueHandle_t s_test_q = NULL;
static StaticQueue_t s_test_q_ctrl;
static uint8_t       s_test_q_store[2 * sizeof(uint8_t)];
static StackType_t   s_test_stack[TEST_STACK_WORDS];
static StaticTask_t  s_test_tcb;
static volatile bool s_test_busy = false;

static int16_t s_test_buf[TEST_CHUNK];

static void test_emit(int n)
{
    audio_out_play_now();
    audio_write_mono(s_test_buf, n);
    vTaskDelay(pdMS_TO_TICKS(TEST_PACE_MS));
}

static void test_sweep(void)
{
    const float f0 = 70.0f, f1 = 4500.0f, dur = 4.0f;
    const int total = (int)(AUDIO_RATE_HZ * dur);
    const float k = logf(f1 / f0) / dur;
    float ph = 0.0f;

    for (int i = 0; i < total; i += TEST_CHUNK) {
        int n = total - i;
        if (n > TEST_CHUNK) n = TEST_CHUNK;
        for (int j = 0; j < n; j++) {
            float t = (float)(i + j) / (float)AUDIO_RATE_HZ;
            float f = f0 * expf(k * t);
            ph += 2.0f * (float)M_PI * f / (float)AUDIO_RATE_HZ;
            if (ph > 2.0f * (float)M_PI) ph -= 2.0f * (float)M_PI;
            float env = 1.0f;
            if (i + j < 400) env = (float)(i + j) / 400.0f;
            else if (i + j > total - 800) env = (float)(total - i - j) / 800.0f;
            s_test_buf[j] = (int16_t)(sinf(ph) * TEST_AMP * env);
        }
        test_emit(n);
    }
}

static void test_bass(void)
{
    static const float steps[] = { 70.0f, 90.0f, 110.0f, 150.0f, 200.0f, 260.0f, 340.0f, 450.0f };
    const int nsteps = (int)(sizeof(steps) / sizeof(steps[0]));
    const int per = (int)(AUDIO_RATE_HZ * 0.45f);
    const int fade = (int)(AUDIO_RATE_HZ * 0.008f);

    for (int s = 0; s < nsteps; s++) {
        float ph = 0.0f;
        for (int i = 0; i < per; i += TEST_CHUNK) {
            int n = per - i;
            if (n > TEST_CHUNK) n = TEST_CHUNK;
            for (int j = 0; j < n; j++) {
                ph += 2.0f * (float)M_PI * steps[s] / (float)AUDIO_RATE_HZ;
                if (ph > 2.0f * (float)M_PI) ph -= 2.0f * (float)M_PI;
                float env = 1.0f;
                if (i + j < fade) env = (float)(i + j) / (float)fade;
                else if (i + j > per - fade) env = (float)(per - i - j) / (float)fade;
                s_test_buf[j] = (int16_t)(sinf(ph) * TEST_AMP * env);
            }
            test_emit(n);
        }
        vTaskDelay(pdMS_TO_TICKS(90));
    }
}

static void test_noise(void)
{
    const int total = (int)(AUDIO_RATE_HZ * 2.5f);
    uint32_t lcg = 0x1234567u;
    float lp = 0.0f;

    for (int i = 0; i < total; i += TEST_CHUNK) {
        int n = total - i;
        if (n > TEST_CHUNK) n = TEST_CHUNK;
        for (int j = 0; j < n; j++) {
            lcg = lcg * 1664525u + 1013904223u;
            float w = ((float)(int32_t)(lcg >> 8) / 8388608.0f) - 1.0f;
            lp += 0.22f * (w - lp);
            float env = 1.0f;
            if (i + j < 800) env = (float)(i + j) / 800.0f;
            else if (i + j > total - 800) env = (float)(total - i - j) / 800.0f;
            float v = (lp * 2.2f + w * 0.35f) * TEST_AMP * env;
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            s_test_buf[j] = (int16_t)v;
        }
        test_emit(n);
    }
}

static void test_worker(void *arg)
{
    (void)arg;
    uint8_t which;
    for (;;) {
        if (xQueueReceive(s_test_q, &which, portMAX_DELAY) != pdTRUE) continue;
        s_test_busy = true;
        switch (which) {
        case SND_TEST_SWEEP: test_sweep(); break;
        case SND_TEST_BASS:  test_bass();  break;
        case SND_TEST_NOISE: test_noise(); break;
        case SND_TEST_TONE:  audio_tone(1000.0f, 1.2f, TEST_AMP); break;
        case SND_TEST_CHIRP: snd_p25_chirp(); break;
        case SND_TEST_MOTO:  snd_moto_full(); break;
        default: break;
        }
        s_test_busy = false;
    }
}

void snd_test_init(void)
{
    if (s_test_q) return;
    s_test_q = xQueueCreateStatic(2, sizeof(uint8_t), s_test_q_store, &s_test_q_ctrl);
    if (!s_test_q) return;
    xTaskCreateStatic(test_worker, "snd_test", TEST_STACK_WORDS, NULL, 4,
                      s_test_stack, &s_test_tcb);
}

bool snd_test_start(int which)
{
    if (which < 0 || which >= SND_TEST_COUNT) return false;
    if (!s_test_q || s_test_busy) return false;

    uint8_t w = (uint8_t)which;
    return xQueueSend(s_test_q, &w, 0) == pdTRUE;
}

bool snd_test_busy(void) { return s_test_busy; }

const char *snd_test_name(int which)
{
    if (which < 0 || which >= SND_TEST_COUNT) return "?";
    return TEST_NAMES[which];
}

int snd_test_from_name(const char *s)
{
    if (!s || !*s) return -1;
    for (int i = 0; i < SND_TEST_COUNT; i++) {
        const char *n = TEST_NAMES[i];
        int k = 0;
        while (n[k] && s[k] && n[k] == (char)tolower((unsigned char)s[k])) k++;
        if (!n[k] && !s[k]) return i;
    }
    return -1;
}
