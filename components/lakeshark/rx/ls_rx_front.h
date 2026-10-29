/* RX HEAD front: one demodulator per stream, run once per block for every
   plugin attached to it.

   The stream's front kind picks the chain (ls_rx_front_kind_t):
     NFM  discriminator at 32 kHz (DISC), then 16 kHz de-emphasised audio
          (PCM16) when anything takes it;
     AM   the envelope at 32 kHz (ENV) for a data plugin (ACARS);
     NONE raw blocks only.
   AM and WFM voice still run on FM's own path; their PCM16 fronts come with
   an AM voice plugin.
   Raw I/Q is handed on first to plugins that take it (AIS reads U8 I/Q).

   The DSP state belongs to the owner (PSRAM, fm_dsp.h / fm_lr_dsp.h); the
   scratch here is sized for the SDR's 16 KB blocks and lives wherever the
   owner puts the front (EXT_RAM_BSS_ATTR on the target). The LR path keeps
   FM's bounded per-block sizes: 84 discriminator and 44 audio samples. */
#ifndef LS_RX_FRONT_H
#define LS_RX_FRONT_H

#include "ls_rx_head.h"
#include "fm_dsp.h"
#include "fm_lr_dsp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS_RX_FRONT_DISC_MAX 1100
#define LS_RX_FRONT_PCM_MAX  600
#define LS_RX_FRONT_LR_DISC  84
#define LS_RX_FRONT_LR_PCM   44

typedef struct ls_rx_front {
    fm_dsp_t    *dsp;
    fm_lr_dsp_t *lr;
    int   nd, na;         /* samples in the last block                       */
    float rms;            /* RMS of the last DISC/ENV block, or PCM / 8000   */
    float    disc[LS_RX_FRONT_DISC_MAX];
    int16_t  pcm[LS_RX_FRONT_PCM_MAX];
} ls_rx_front_t;

void ls_rx_front_init(ls_rx_front_t *f, fm_dsp_t *dsp, fm_lr_dsp_t *lr);
/* The head this front feeds; plugins reach the DSP through it. */
void ls_rx_front_bind(ls_rx_front_t *f, ls_rx_head_t *h);
fm_dsp_t *ls_rx_front_dsp(const ls_rx_head_t *h);

/* LR2021: 125 kS/s signed 16-bit I/Q. Returns DISC/ENV samples made. */
int ls_rx_front_lr(ls_rx_front_t *f, ls_rx_head_t *h, const uint8_t *iq, size_t bytes, int64_t at_us);
/* External SDR: 256 kS/s unsigned 8-bit I/Q. */
int ls_rx_front_u8(ls_rx_front_t *f, ls_rx_head_t *h, const uint8_t *iq, int len, int64_t at_us);

#ifdef __cplusplus
}
#endif
#endif
