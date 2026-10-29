/* Broadcast FM multiplex decoder: stereo and RDS from the discriminator.

   In: the FM discriminator at 256 kHz in fm_dsp's units (pi = 16384, so
   75 kHz deviation reads 9600). Out: 16 kHz audio, left and right or their
   mono sum, de-emphasised; and RDS bits into rds.c.

   - A second-order PLL locks a quadrature NCO to the 19 kHz pilot. Its
     coherent arm measures the pilot, which both decides STEREO and lets the
     pilot be subtracted before anything else reads the multiplex.
   - L-R is the multiplex times the doubled NCO (38 kHz), L+R the multiplex
     itself; both are decimated 16:1 by a second-order CIC (sinc^2) and
     matrixed, then de-emphasised at 16 kHz.
   - RDS is mixed down with the tripled NCO (57 kHz, the carrier is locked to
     the third pilot harmonic), decimated by a third-order CIC, low-passed
     and phase-tracked by a Costas loop. Its bit clock is the pilot divided
     by 16 (1187.5 Hz): sixteen biphase integrate-and-dump filters, one per
     sixteenth-of-a-bit phase, run side by side and the one with the most
     energy supplies the bits, so no symbol timing loop is needed.

   References: ITU-R BS.450 (multiplex), IEC 62106 section 1 (RDS
   modulation, biphase coding, differential coding). Own implementation.

   Pure C, no allocation. The struct is about 3 KB; put it in PSRAM. */
#ifndef LS_WFM_MPX_H
#define LS_WFM_MPX_H

#include <stdbool.h>
#include <stdint.h>

#include "rds.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WFM_MPX_RATE    256000
#define WFM_MPX_DECIM   16
#define WFM_MPX_OUT     (WFM_MPX_RATE / WFM_MPX_DECIM)
/* Discriminator units of full (75 kHz) deviation. */
#define WFM_MPX_FULL    9600.0f
#define WFM_RDS_TAPS    25
#define WFM_RDS_PHASES  16

typedef struct {
    float tab[256 + 65];                  /* sin, a quarter extra for cos  */
    float w2[WFM_MPX_DECIM][2];           /* sinc^2 decimator weights      */
    float w3[WFM_MPX_DECIM][3];           /* sinc^3                        */
    float fir[WFM_RDS_TAPS];

    /* Pilot PLL; phase in cycles. */
    float    ph, fr, fr0;
    uint32_t cycles;
    float    pi_lp, pq_lp;
    int      lock_n;
    bool     pilot;          /* locked                                     */
    bool     stereo;         /* pilot locked and stereo allowed            */
    bool     force_mono;

    int   pos;
    float sum[2], dif[2], ri[3], rq[3];
    float de_l, de_r, de_a;
    float lvl_l, lvl_r;      /* EMA of |L|, |R|, full deviation = 1        */
    float pilot_level;       /* pilot amplitude, full deviation = 1        */

    /* RDS. */
    bool  rds_on;
    float zi[2 * WFM_RDS_TAPS], zq[2 * WFM_RDS_TAPS];
    int   zpos;
    float cph, cfr, cpow;
    float acc[WFM_RDS_PHASES], en[WFM_RDS_PHASES], prevq[WFM_RDS_PHASES];
    int   best;
    int   last_sym;
    float rds_level;         /* RDS injection seen by the Costas arm       */
    rds_t rds;
} wfm_mpx_t;

void wfm_mpx_init(wfm_mpx_t *m);
/* After a retune: PLL, filters and the RDS card start over. */
void wfm_mpx_reset(wfm_mpx_t *m);
void wfm_mpx_set_rds(wfm_mpx_t *m, bool on);
void wfm_mpx_set_mono(wfm_mpx_t *m, bool mono);

/* `n` discriminator samples at WFM_MPX_RATE. With `right` NULL, writes the
   mono sum to `left`. Returns the 16 kHz samples written, at most `max`. */
int wfm_mpx_process(wfm_mpx_t *m, const int16_t *disc, int n,
                    int16_t *left, int16_t *right, int max);

#ifdef __cplusplus
}
#endif

#endif
