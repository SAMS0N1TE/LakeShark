/* tone.c - sine-tone generators feeding audio_write_mono. */

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
