/* SSTV on FM's audio: the receive task hands every block of LISTEN or AM
   audio here while the SSTV option is on. Pictures build up in PSRAM for
   the screen (fm_bcast.h) and go to /sdcard/sstv as 24-bit BMP, a few
   rows a block so the card write never holds up the receiver. */
#ifndef FM_SSTV_H
#define FM_SSTV_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 16 kHz audio; NULL with n samples for a squelched gap of that length. */
void fm_sstv_audio(const int16_t *pcm, int n);
/* A retune or a mode change: drop a picture in progress. */
void fm_sstv_reset(void);
/* Whether the option is on and the decoder may run (receive task). */
bool fm_sstv_wanted(void);
/* Every receive loop, whatever the mode: a finished picture's rows still
   going to the card get written a few at a time. */
void fm_sstv_service(void);

#ifdef __cplusplus
}
#endif

#endif
