#ifndef AUDIO_TONE_H
#define AUDIO_TONE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SND_TEST_SWEEP = 0,
    SND_TEST_BASS,
    SND_TEST_NOISE,
    SND_TEST_TONE,
    SND_TEST_CHIRP,
    SND_TEST_MOTO,
    SND_TEST_COUNT
} snd_test_t;

void        snd_test_init(void);
bool        snd_test_start(int which);
int         snd_test_from_name(const char *s);
const char *snd_test_name(int which);
bool        snd_test_busy(void);

void audio_tone(float freq, float dur_s, float amp);

void snd_p25_chirp(void);

void snd_boot(void);

void snd_moto_power_on(void);
void snd_moto_alert(void);
void snd_moto_bonk(void);
void snd_moto_full(void);
void snd_new_contact(void);
void snd_lost_contact(void);
void snd_position_fix(void);

#ifdef __cplusplus
}
#endif

#endif
