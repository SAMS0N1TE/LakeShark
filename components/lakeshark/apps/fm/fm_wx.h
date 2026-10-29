/* NOAA Weather Radio in FM's SAME mode: the seven WX channels, finding
   the strongest in one look (all seven fit one 176 kHz window), and the
   weather broadcast on the speaker: always, or only while a SAME alert is
   running (header heard, until its end-of-message or ten minutes), the way
   a weather radio on the kitchen shelf behaves. SAME decoding is unchanged:
   alerts still go to same_store and the ALERTS page. Receive task only. */
#ifndef FM_WX_H
#define FM_WX_H

#include <stdbool.h>
#include <stdint.h>

#include "fm_dsp.h"
#include "same.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Speaker in SAME mode (fm_bcast option bits). */
enum { FM_WX_AUDIO_OFF = 0, FM_WX_AUDIO_ON = 1, FM_WX_AUDIO_ALERTS = 2 };

void fm_wx_install(void (*tune)(uint32_t hz));
/* The SAME decoder's sink: forwards to same_store and opens the speaker
   for an alert. */
void fm_wx_on_same(const same_alert_t *a, void *user);
/* Take the screen's requests (SAME mode, every block). */
void fm_wx_poll(void);
/* Looking for the strongest channel: the block goes here instead of the
   SAME decoder. */
bool fm_wx_finding(void);
void fm_wx_block(const uint8_t *iq, int iq_len);
/* After SAME has had the block: the speaker, if the option says so. */
void fm_wx_audio(fm_dsp_t *dsp, int demod_samples, const int16_t *pcm, int n);
/* Leaving SAME mode. */
void fm_wx_stop(void);

#ifdef __cplusplus
}
#endif

#endif
