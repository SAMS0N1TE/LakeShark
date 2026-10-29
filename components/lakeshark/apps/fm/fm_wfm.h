/* FM's WFM mode on an SDR: the 256 kHz discriminator through the stereo
   and RDS decoder (dsp/wfm_mpx.c), mono to the speaker, the station card
   published for the screen (fm_bcast.h), and RDS clock time offered to the
   wall clock when nothing better has set it. Receive task only. */
#ifndef FM_WFM_H
#define FM_WFM_H

#include <stdint.h>

#include "fm_dsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mode entry and every retune: PLL, filters and card start over. */
void fm_wfm_reset(void);
/* One I/Q block in, 16 kHz mono out (at most `max`). */
int fm_wfm_block(fm_dsp_t *dsp, const uint8_t *iq, int iq_len,
                 int16_t *pcm, int max);
/* Leaving WFM: the screen stops showing a station. */
void fm_wfm_idle(void);
/* For the stats line. */
const char *fm_wfm_summary(char *out, int n);

#ifdef __cplusplus
}
#endif

#endif
