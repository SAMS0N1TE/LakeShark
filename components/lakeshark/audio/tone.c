#include "tone.h"
#include "audio_out.h"
#include "speech.h"
#include "settings.h"
#include "notification_tone.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <math.h>
#include <stdio.h>
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
        audio_write_cue(buf, chunk);
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
    audio_write_cue(buf, total);
}

/* Write a long sound at the speed it plays. */

static void tone_write_paced(const int16_t *buf, int n)
{
    const uint32_t half = (uint32_t)(AUDIO_RATE_HZ * 300 / 1000) *
                          (uint32_t)sizeof(int16_t);
    for (int i = 0; i < 200 && audio_out_ring_avail() > half; i++)
        vTaskDelay(pdMS_TO_TICKS(10));
    audio_write_cue(buf, n);
}

/* The power-on chirp, in the manner of a Motorola data burst: a low key-up
   thump, a frequency-shift data burst (600 baud, 700 Hz mark, 1050 Hz
   space, phase continuous), then two go-ahead beeps at 1 kHz. Every tone is
   a soft-clipped sine with a sub-octave under it, which is what gives it
   weight on a small speaker. Rendered whole before any of it is queued:
   under 0.4 s, so it sits in the ring at once and a busy boot cannot starve
   it into crackling the way a chime computed on the fly did. */
#define BOOT_MAX (AUDIO_RATE_HZ * 400 / 1000)
static EXT_RAM_BSS_ATTR int16_t s_boot_buf[BOOT_MAX];

typedef struct { float ph, sub; } boot_osc_t;

/* One sample of the voice at hz: saturated fundamental plus sub-octave. */
static float boot_voice(boot_osc_t *o, float hz)
{
    o->ph  += 2.0f * (float)M_PI * hz / AUDIO_RATE_HZ;
    o->sub += (float)M_PI * hz / AUDIO_RATE_HZ;
    if (o->ph  > 2.0f * (float)M_PI) o->ph  -= 2.0f * (float)M_PI;
    if (o->sub > 2.0f * (float)M_PI) o->sub -= 2.0f * (float)M_PI;
    return 0.80f * tanhf(2.2f * sinf(o->ph)) + 0.30f * sinf(o->sub);
}

static float boot_env(int i, int len)
{
    const int fade = AUDIO_RATE_HZ * 4 / 1000;
    if (i < fade) return (float)i / fade;
    if (i >= len - fade) return (float)(len - i) / fade;
    return 1.0f;
}

void snd_boot(void)
{
    /* 0x5555 preamble, the MDC sync word 07 09 2A 44 6F. */
    static const uint8_t BURST[] = { 0x55, 0x55, 0x07, 0x09, 0x2A, 0x44, 0x6F };
    const float amp = 6000.0f;
    memset(s_boot_buf, 0, sizeof(s_boot_buf));
    int n = 0;

    /* Key-up: a short falling thump. */
    const int thump = AUDIO_RATE_HZ * 35 / 1000;
    float tp = 0.0f;
    for (int i = 0; i < thump; i++, n++) {
        const float t = (float)i / thump;
        tp += 2.0f * (float)M_PI * (220.0f - 90.0f * t) / AUDIO_RATE_HZ;
        s_boot_buf[n] = (int16_t)(sinf(tp) * amp * (1.0f - t) * (1.0f - t) * boot_env(i, thump));
    }
    n += AUDIO_RATE_HZ * 10 / 1000;

    boot_osc_t o = { 0 };
    const int burst = (int)sizeof(BURST) * 8 * AUDIO_RATE_HZ / 600;
    for (int i = 0; i < burst && n < BOOT_MAX; i++, n++) {
        const int bit = i * 600 / AUDIO_RATE_HZ;
        const bool mark = (BURST[bit / 8] >> (7 - bit % 8)) & 1;
        s_boot_buf[n] = (int16_t)(boot_voice(&o, mark ? 700.0f : 1050.0f) * amp * boot_env(i, burst));
    }
    n += AUDIO_RATE_HZ * 35 / 1000;

    const int beep = AUDIO_RATE_HZ * 60 / 1000;
    for (int k = 0; k < 2; k++) {
        o = (boot_osc_t){ 0 };
        for (int i = 0; i < beep && n < BOOT_MAX; i++, n++)
            s_boot_buf[n] = (int16_t)(boot_voice(&o, 1000.0f) * amp * boot_env(i, beep));
        if (k == 0) n += AUDIO_RATE_HZ * 35 / 1000;
    }

    if (n > BOOT_MAX) n = BOOT_MAX;
    const uint32_t room = (uint32_t)(AUDIO_RATE_HZ * 150 / 1000) * sizeof(int16_t);
    for (int i = 0; i < 200 && audio_out_ring_avail() > room; i++)
        vTaskDelay(pdMS_TO_TICKS(10));
    audio_out_play_now();
    if (!audio_write_cue_wait(s_boot_buf, n, 500)) printf("boot chirp: not queued whole\n");
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
        audio_write_cue(buf, chunk);
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
/* 3072 with 2488 unused - about 584 B in use.
   And back to 3072: that measurement was test tones only, and this
   worker has since taken the boot sound, the greeting and the alert. See
   SND_WORKER_STACK_BYTES in tone.h. */
#define TEST_STACK_WORDS (SND_WORKER_STACK_BYTES / sizeof(StackType_t))

static const char *TEST_NAMES[SND_TEST_COUNT] = {
    "sweep", "bass", "noise", "tone", "p25chirp", "moto",
    "radiokey", "roger", "chirp"
};

static QueueHandle_t s_test_q = NULL;
static StaticQueue_t s_test_q_ctrl;
static uint8_t       s_test_q_store[2 * sizeof(uint8_t)];
static StackType_t   s_test_stack[TEST_STACK_WORDS];
static StaticTask_t  s_test_tcb;
static TaskHandle_t  s_test_task;
static volatile bool s_test_busy = false;

static EXT_RAM_BSS_ATTR int16_t s_test_buf[TEST_CHUNK];

static void test_emit(int n)
{
    audio_out_play_now();
    tone_write_paced(s_test_buf, n);
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

/* A tone longer than the ring, written at the speed it plays. */
static void test_tone_long(float freq, float dur_s)
{
    const int total = (int)(AUDIO_RATE_HZ * dur_s);
    static int16_t buf[256];
    float ph = 0.0f;
    audio_out_play_now();
    for (int i = 0; i < total; i += 256) {
        int chunk = total - i;
        if (chunk > 256) chunk = 256;
        for (int j = 0; j < chunk; j++) {
            ph += 2.0f * (float)M_PI * freq / AUDIO_RATE_HZ;
            if (ph > 2.0f * (float)M_PI) ph -= 2.0f * (float)M_PI;
            buf[j] = (int16_t)(sinf(ph) * TEST_AMP);
        }
        tone_write_paced(buf, chunk);
    }
}

/* From ls_audio_hw.h, which brings the I2S driver in with it: the counter
   is all this file needs, and the host bench compiles this file alone. */
uint32_t ls_audio_hw_starved(int64_t *first_us, int64_t *last_us);

static void play_boot_chime(void)
{
    audio_out_ensure_unmuted();
    const uint32_t starved0 = ls_audio_hw_starved(NULL, NULL);
    const TickType_t t0 = xTaskGetTickCount();
    snd_boot();
    static const int16_t sil[1600] = { 0 };
    audio_write_cue(sil, 1600);
    for (int i = 0; i < 250 && audio_out_ring_avail() > 320; i++)
        vTaskDelay(pdMS_TO_TICKS(20));
    printf("boot chirp: at %lu ms, %lu ms to play, speaker starved %u times\n",
           (unsigned long)(t0 * portTICK_PERIOD_MS),
           (unsigned long)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS),
           (unsigned)(ls_audio_hw_starved(NULL, NULL) - starved0));
}

static void sweep_pattern(unsigned category)
{
    static const uint16_t notes[5][3] = {
        {700, 700, 0}, {900, 600, 0}, {600, 900, 1200},
        {1400, 0, 0}, {1100, 500, 1100}
    };
    for (unsigned i=0; i<3 && notes[category][i]; i++) {
        if (audio_is_muted() || audio_volume_get() <= 0) return;
        audio_tone(notes[category][i], 0.035f, 2200.0f);
        vTaskDelay(pdMS_TO_TICKS(45));
    }
}

static void sweep_click_pattern(void) {
    if(!audio_is_muted() && audio_volume_get()>0) audio_tone(1200.0f,0.008f,1800.0f);
}

/* Only called on snd_test; shares its PSRAM chunk and the normal cue path. */
static void notification_play(int preset)
{
    if (audio_is_muted() || audio_volume_get()<=0) return;
    notification_tone_load();
    audio_out_play_now();
    size_t offset=0, n;
    while ((n=notification_tone_render(preset,offset,s_test_buf,TEST_CHUNK))!=0) {
        if (audio_is_muted() || audio_volume_get()<=0) return;
        tone_write_paced(s_test_buf,(int)n);
        offset+=n;
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
        /* Paced, for the same reason snd_boot is: 1.2 s against a
           600 ms ring meant this test tone had been playing for half a
           second and reporting itself as 1.2 for as long as it existed. */
        case SND_TEST_TONE:  test_tone_long(1000.0f, 1.2f); break;
        case SND_TEST_CHIRP: snd_p25_chirp(); break;
        case SND_TEST_MOTO:  snd_moto_full(); break;
        case SND_TEST_RADIOKEY: notification_play(0); break;
        case SND_TEST_ROGER: notification_play(1); break;
        case SND_TEST_UPCHIRP: notification_play(2); break;
        case SND_JOB_ALERT:  snd_moto_alert(); break;
        case SND_JOB_BOOT:   play_boot_chime(); break;
        /* Never speech on this stack while there is no engine: the
           no-engine stub's one log line is what overflowed it. snd_boot_start
           already turns mode 2 into the chime, and this is the same rule for
           anything that queues the job directly. */
        case SND_JOB_WELCOME:
            if (!speech_available()) { play_boot_chime(); break; }
            audio_out_ensure_unmuted();
            speech_say("WELCOME.");
            break;
        case 75:
            sweep_click_pattern();
            break;
        case 70: case 71: case 72: case 73: case 74:
            sweep_pattern(which-70); break;
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
    s_test_task = xTaskCreateStatic(test_worker, "snd_test", TEST_STACK_WORDS,
                                    NULL, 4, s_test_stack, &s_test_tcb);
}

bool snd_test_start(int which)
{
    if (which < 0 || which >= SND_TEST_COUNT) return false;
    if (!s_test_q || s_test_busy) return false;

    uint8_t w = (uint8_t)which;
    return xQueueSend(s_test_q, &w, 0) == pdTRUE;
}

/* The audible half of a notification, and why it is here. */

bool snd_sweep_click(void) {
    if(!s_test_q || s_test_busy || audio_is_muted() || audio_volume_get()<=0) return false;
    uint8_t w=75;return xQueueSend(s_test_q,&w,0)==pdTRUE;
}

bool snd_sweep_start(int category)
{
    if (category < 0 || category > 4 || !s_test_q || s_test_busy || audio_is_muted() || audio_volume_get() <= 0) return false;
    uint8_t w = (uint8_t)(70 + category);
    return xQueueSend(s_test_q, &w, 0) == pdTRUE;
}

bool snd_notification_start(int sound)
{
    if (!s_test_q || s_test_busy) return false;
    if (sound < SETTINGS_NOTIFY_ALERT || sound >= SETTINGS_NOTIFY_OFF) return false;
    if (sound==SETTINGS_NOTIFY_OFF || audio_is_muted() || audio_volume_get()<=0) return false;
    uint8_t w = SND_JOB_ALERT;
    if (sound==SETTINGS_NOTIFY_RADIOKEY) w=SND_TEST_RADIOKEY;
    else if (sound==SETTINGS_NOTIFY_ROGER) w=SND_TEST_ROGER;
    else if (sound==SETTINGS_NOTIFY_CHIRP) w=SND_TEST_UPCHIRP;
    return xQueueSend(s_test_q, &w, 0) == pdTRUE;
}

bool snd_alert_start(void) { return snd_notification_start(settings_get_notification_sound()); }

bool snd_boot_start(int mode)
{
    if (mode != 1 && mode != 2) return false;
    if (!s_test_q || s_test_busy) return false;
    /* Spoken only when something can speak. With no engine the
       greeting is the chime, which is also what SETTINGS now says it is. */
    if (mode == 2 && !speech_available()) mode = 1;
    uint8_t w = (uint8_t)(mode == 2 ? SND_JOB_WELCOME : SND_JOB_BOOT);
    return xQueueSend(s_test_q, &w, 0) == pdTRUE;
}

unsigned snd_test_stack_unused(void)
{
    if (!s_test_task) return 0;
    return (unsigned)uxTaskGetStackHighWaterMark(s_test_task) *
           (unsigned)sizeof(StackType_t);
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
