
#include "tone.h"
#include "audio_out.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <stdint.h>

void audio_tone(float freq, float dur_s, float amp)
{
    int total = (int)(AUDIO_RATE_HZ * dur_s);
    static int16_t buf[256];
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
    audio_tone(440.0f, 0.12f, 8500.0f);
    audio_tone(660.0f, 0.12f, 8500.0f);
    audio_tone(880.0f, 0.16f, 8500.0f);
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
