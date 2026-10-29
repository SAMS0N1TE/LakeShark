#ifndef HF_DEMOD_H
#define HF_DEMOD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* HF (shortwave) receiver back end: 256 kS/s IQ in, 16 kHz audio out.
 * AM, synchronous AM, USB, LSB and CW, a noise blanker, AGC and a fine
 * clarifier. Used by the FM app's AM mode below 30 MHz and for any sideband
 * or CW listening (fm_hf.h decides when).
 *
 *   IQ 256k -> [noise blanker] -> mix to the passband centre (NCO)
 *           -> FIR /8 (56 taps) -> 32k -> FIR /2 (24 taps) -> 16k
 *           -> channel filter (real taps, 127..255) -> demod -> AGC -> PCM
 *
 * The channel filter is a lowpass on a signal mixed so the wanted passband
 * is centred on 0 Hz; for SSB the result is turned back up by the passband
 * centre and its real part is the audio, so the opposite sideband never
 * reaches the output (the Weaver form of the phasing method). CW is the
 * carrier on 0 Hz through a narrow lowpass, turned up by the BFO pitch. AM is
 * the envelope; SAM locks a second-order PLL to the carrier and takes the
 * in-phase part, which keeps selective fading from distorting it.
 *
 * Filters are Kaiser-windowed sinc designed at configure time (Kaiser,
 * "Nonrecursive digital filter design using the I0-sinh window function",
 * 1974; the order/attenuation estimate from Oppenheim & Schafer). The AGC is
 * an envelope peak follower with instant attack, a hang time and a constant
 * dB/s decay, applied to the channel envelope rather than the audio so it
 * does not ripple at audio rate. The blanker gates impulses whose power stands
 * well above the running wideband average, a few samples either side.
 *
 * Caller owns the state (about 7 KB): PSRAM is fine. No allocation, no waits
 * and no libm calls in process. Not thread safe: one owner. */

#define HF_IN_RATE       256000u
#define HF_MID_RATE       32000u
#define HF_OUT_RATE       16000u
#define HF_S1_TAPS           56
#define HF_S2_TAPS           24
#define HF_CH_TAPS_MAX      255
#define HF_NB_DELAY           8
/* Where SSB passbands start above (USB) or below (LSB) the carrier. */
#define HF_SSB_LOW_HZ       250
/* The loudest a sample is let out, and where the AGC aims the envelope.
   NFM's path caps at +-6000 too (moderate loudness on the board speaker). */
#define HF_PCM_LIMIT       6000
#define HF_PCM_TARGET      4500

typedef enum {
    HF_DEMOD_AM = 0,
    HF_DEMOD_SAM,
    HF_DEMOD_USB,
    HF_DEMOD_LSB,
    HF_DEMOD_CW,
    HF_DEMOD_COUNT
} hf_demod_mode_t;

typedef enum { HF_AGC_FAST = 0, HF_AGC_SLOW, HF_AGC_OFF, HF_AGC_COUNT } hf_agc_t;
typedef enum { HF_NB_OFF = 0, HF_NB_LOW, HF_NB_HIGH, HF_NB_COUNT } hf_nb_t;

typedef struct {
    hf_demod_mode_t mode;
    uint32_t filter_hz;    /* full width: AM 6000/8000, SSB 2400/2800, CW 500/250 */
    int32_t  tune_hz;      /* where the dial carrier sits in the IQ, from its centre */
    int32_t  rit_hz;       /* clarifier: moves what is heard, not the dial */
    uint32_t cw_pitch_hz;  /* BFO pitch the CW carrier is heard at */
    hf_agc_t agc;
    hf_nb_t  nb;
} hf_demod_cfg_t;

typedef struct {
    hf_demod_cfg_t cfg;
    /* Designed filters. Channel taps are for a cutoff of filter/2. */
    float s1_h[HF_S1_TAPS], s2_h[HF_S2_TAPS], ch_h[HF_CH_TAPS_MAX];
    int   ch_n;
    hf_demod_mode_t ch_mode;
    uint32_t ch_width;
    /* Delay lines, each written twice so a window is always contiguous. */
    float s1_i[2 * HF_S1_TAPS], s1_q[2 * HF_S1_TAPS];
    float s2_i[2 * HF_S2_TAPS], s2_q[2 * HF_S2_TAPS];
    float ch_i[2 * HF_CH_TAPS_MAX], ch_q[2 * HF_CH_TAPS_MAX];
    int   s1_pos, s2_pos, ch_pos, s1_cnt, s2_cnt;
    /* Input NCO at 256k and the passband turn-back at 16k, as phasors. */
    float nco_re, nco_im, nco_dre, nco_dim;
    float post_re, post_im, post_dre, post_dim;
    int   nco_norm, post_norm;
    float pb_center_hz;    /* where the passband was centred before filtering */
    /* Noise blanker. */
    float nb_avg, nb_thresh;
    float nb_di[HF_NB_DELAY], nb_dq[HF_NB_DELAY];
    int   nb_pos, nb_left;
    uint32_t nb_blanked;
    /* SAM carrier loop. */
    float pll_re, pll_im, pll_freq, pll_alpha, pll_beta, pll_limit;
    int   pll_norm;
    /* Audio DC (the AM carrier) and AGC. */
    float dc, dc_k;
    float agc_peak, agc_gain, agc_decay, agc_floor;
    int   agc_hang, agc_hang_n;
    /* Channel power, slowly averaged, for a meter and a squelch. */
    float power;
} hf_demod_t;

/* Defaults: AM, 6 kHz, AGC slow, blanker off, CW pitch 700 Hz, no offset. */
void hf_demod_init(hf_demod_t *s);
/* Apply a configuration. Filters are redesigned only when the mode or width
   changed; tuning and RIT changes are free and keep every history. */
void hf_demod_set(hf_demod_t *s, const hf_demod_cfg_t *cfg);
/* Forget the signal (after a retune): histories, PLL, carrier estimate. The
   AGC gain is kept, so a retune does not blast the speaker. */
void hf_demod_reset(hf_demod_t *s);

/* 8-bit offset-binary interleaved IQ (RTL format) at HF_IN_RATE. Returns the
   PCM samples written at HF_OUT_RATE: one per 16 input pairs, at most `max`.
   A partial count of pairs carries over to the next call. */
int hf_demod_u8(hf_demod_t *s, const uint8_t *iq, size_t bytes,
                int16_t *pcm, int max);
/* The same on float IQ pairs (I, Q, I, Q ...), full scale 1.0. */
int hf_demod_cf32(hf_demod_t *s, const float *iq, size_t pairs,
                  int16_t *pcm, int max);

/* Channel power in dB below a full-scale carrier (0 dBFS = amplitude 1.0). */
float hf_demod_level_dbfs(const hf_demod_t *s);
/* Gain the AGC is applying now, dB. */
float hf_demod_gain_db(const hf_demod_t *s);
/* How far the SAM loop has pulled from the dial, Hz (0 outside SAM). */
float hf_demod_sam_offset_hz(const hf_demod_t *s);
/* Input samples the blanker has gated since init. */
uint32_t hf_demod_blanked(const hf_demod_t *s);

/* The width a mode runs: CW 100 Hz..4 kHz, SSB 1..4 kHz, AM 1..10 kHz. */
static inline uint32_t hf_demod_clamp_width(hf_demod_mode_t mode, uint32_t w)
{
    const uint32_t lo = mode == HF_DEMOD_CW ? 100 : 1000;
    const uint32_t hi = (mode == HF_DEMOD_AM || mode == HF_DEMOD_SAM) ? 10000 : 4000;
    return w < lo ? lo : w > hi ? hi : w;
}

/* The passband, as audio-domain edges relative to the carrier: USB 250 ..
   250+width, LSB the mirror, CW and AM centred. Inline so a screen can draw
   it without linking the DSP. */
static inline void hf_demod_passband(hf_demod_mode_t mode, uint32_t width_hz,
                                     int32_t *lo_hz, int32_t *hi_hz)
{
    const int32_t w = (int32_t)hf_demod_clamp_width(mode, width_hz);
    int32_t lo = -w / 2, hi = w / 2;
    if (mode == HF_DEMOD_USB) { lo = HF_SSB_LOW_HZ; hi = HF_SSB_LOW_HZ + w; }
    else if (mode == HF_DEMOD_LSB) { lo = -(HF_SSB_LOW_HZ + w); hi = -HF_SSB_LOW_HZ; }
    if (lo_hz) *lo_hz = lo;
    if (hi_hz) *hi_hz = hi;
}

#ifdef __cplusplus
}
#endif
#endif
