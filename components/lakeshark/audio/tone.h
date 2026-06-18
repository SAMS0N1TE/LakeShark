
#ifndef AUDIO_TONE_H
#define AUDIO_TONE_H

#ifdef __cplusplus
extern "C" {
#endif

void audio_tone(float freq, float dur_s, float amp);

void snd_boot(void);
void snd_new_contact(void);
void snd_lost_contact(void);
void snd_position_fix(void);

#ifdef __cplusplus
}
#endif

#endif
