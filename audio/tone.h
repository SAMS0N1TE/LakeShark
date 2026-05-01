/*
 * tone.h - sine-tone generators for audio events (beeps).
 *
 * These bypass the audio_events queue and play immediately. Use them
 * for short fixed-pitch alerts only; anything longer should go through
 * audio_events (which serialises on the audio task).
 */
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
