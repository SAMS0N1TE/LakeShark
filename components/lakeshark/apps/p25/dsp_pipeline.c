/*
 * dsp_pipeline.c — Quad-mode P25 demodulator
 *
 * C4FM:          IQ -> LPF+decim x5 -> FM discriminator -> 48kHz continuous
 * CQPSK:         IQ -> LPF+decim x5 -> Costas -> Gardner -> diff_phasor ->
 *                atan2 -> rescale by 1/(pi/4) -> scale to int16 -> DSD
 * DIFF_4FSK:     IQ -> LPF+decim x5 -> RRC pulse shaping (I+Q) ->
 *                differential demod (atan2 of cur*conj(prev)) ->
 *                equalizer (PLL + gain) -> scale to int16 -> DSD
 * FSK4_TRACKING: IQ -> LPF+decim x5 -> linear atan2 FM discriminator ->
 *                3-loop tracker (freq offset, symbol spread, symbol timing) ->
 *                MMSE fractional-sample interpolator -> one symbol per 10 samples
 *                -> scale to int16 -> DSD
 *
 * FSK4_TRACKING is a port of OP25's fsk4_demod_ff_impl.cc (GPL-3, Frank/Radio
 * Rausch 2006, Steve Glass 2011). It implements the algorithm from U.S.
 * Patent 5,553,101 (Motorola RDLAP, 1996). Unlike the original C4FM mode:
 *   - Uses linear atan2 phase output (no magnitude normalization), so the
 *     ±3/±1/-1/-3 constellation stays linear instead of being compressed.
 *   - Tracks frequency offset continuously, pulling DC bias to zero.
 *   - Tracks symbol spread (deviation), so outer/inner ratio stays at 3:1
 *     even if amplitude changes during the frame.
 *   - Tracks symbol timing, so sample points stay locked to symbol centers
 *     instead of drifting according to whatever random offset sync landed on.
 *   - Uses 128-step MMSE interpolation for fractional-sample timing (no
 *     nearest-integer jitter).
 *
 * DIFF_4FSK is inspired by SDRTrunk's P25P1DecoderC4FM which uses
 * PI/4 DQPSK differential demodulation with an equalizer that corrects
 * for DC offset (frequency error) and constellation compression.
 * Key differences from our FM discriminator path:
 *   - Phase output in radians, not FM voltage
 *   - Fixed quadrant boundaries (±π/2), not adaptive min/max
 *   - Constellation gain correction (~1.219x) for pulse-shaped signals
 *   - DC balance correction for frequency offset
 */
#include "dsp_pipeline.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

static const float lpf_taps[DSP_FIR_TAPS] = {
    -0.00072620f, -0.00035803f, +0.00025719f, +0.00153795f, +0.00392437f, +0.00779942f,
    +0.01341098f, +0.02080767f, +0.02979993f, +0.03995421f, +0.05062342f, +0.06101077f,
    +0.07025930f, +0.07755466f, +0.08222673f, +0.08383523f, +0.08222673f, +0.07755466f,
    +0.07025930f, +0.06101077f, +0.05062342f, +0.03995421f, +0.02979993f, +0.02080767f,
    +0.01341098f, +0.00779942f, +0.00392437f, +0.00153795f, +0.00025719f, -0.00035803f,
    -0.00072620f
};

void dsp_init(dsp_state_t *s)
{
    memset(s, 0, sizeof(dsp_state_t));
    for (int i = 0; i < DSP_FIR_TAPS; i++)
        s->fir_coeffs[i] = lpf_taps[i];

    s->mode = DEMOD_C4FM;          
    s->demod_gain = 9000.0f;

    s->g_period    = (float)DSP_SPS;
    s->g_mu        = 0.5f;
    s->g_gain_mu   = 0.025f;
    s->g_gain_omega = 0.025f * 0.025f * 0.25f;
    s->g_omega_rel = 0.005f;
    s->g_half = DSP_SPS / 2;

    s->c_alpha = 0.04f;
    s->c_beta  = 0.04f * 0.04f * 0.25f;

    s->diff_prev_i = 1.0f;
    s->diff_prev_q = 0.0f;
    s->cqpsk_polarity = 0;

    s->dc_avg_i = 0.0f;
    s->dc_avg_q = 0.0f;
    s->dc_alpha = 0.001f;   

    s->agc_gain  = 1.0f;
    s->agc_alpha = 0.01f;
    s->agc_ref   = 0.85f;

    s->eq_pll  = 0.0f;
    s->eq_gain = 1.219f;     

    s->diff_output_scale = 10186.0f;

    s->diff_ring_idx = 0;
    s->diff_ring_filled = 0;

    s->fsk4_input_scale = 7.0f;   
    s->ft_symbol_clock  = 0.0;
    s->ft_symbol_time   = (double)DSP_BAUD / (double)DSP_AUDIO_RATE;  
    s->ft_symbol_spread = 2.0;
    s->ft_fine_freq     = 0.0;
    s->ft_coarse_freq   = 0.0;
    for (int k = 0; k < 8; k++) s->ft_history[k] = 0.0f;
    s->ft_history_idx   = 0;
    s->ft_prev_i        = 1.0f;
    s->ft_prev_q        = 0.0f;

    s->nco_phase    = 0.0;
    s->nco_step_rad = 0.0;
    s->nco_dc_avg   = 0.0;

    s->c4fm_dc_avg  = 0.0f;
}

void dsp_set_mode(dsp_state_t *s, demod_mode_t mode)
{
    s->mode = mode;
    if (mode == DEMOD_C4FM || mode == DEMOD_FSK4_TRACKING) {
        for (int k = 0; k < RRC_FSK4_TAPS; k++) s->rrc_fsk4_buf[k] = 0.0f;
        s->rrc_fsk4_idx = 0;
    }
    if (mode == DEMOD_FSK4_TRACKING) {
        s->ft_symbol_clock  = 0.0;
        s->ft_symbol_spread = 2.0;
        s->ft_fine_freq     = 0.0;
        s->ft_coarse_freq   = 0.0;
        for (int k = 0; k < 8; k++) s->ft_history[k] = 0.0f;
        s->ft_history_idx   = 0;
        s->ft_prev_i        = 1.0f;
        s->ft_prev_q        = 0.0f;
        s->nco_phase    = 0.0;
        s->nco_step_rad = 0.0;
        s->nco_dc_avg   = 0.0;
        extern int fsk4_diag_reset_flag;
        fsk4_diag_reset_flag = 1;
    }
}
void dsp_set_gain(dsp_state_t *s, float gain) { s->demod_gain = gain; }

void dsp_set_costas_alpha(dsp_state_t *s, float alpha)
{
    s->c_alpha = alpha;
    s->c_beta = alpha * alpha * 0.25f;
}

void dsp_flip_polarity(dsp_state_t *s)
{
    if (s->mode == DEMOD_CQPSK)
        s->cqpsk_polarity = (s->cqpsk_polarity + 1) % 8;
    else
        s->demod_gain = -s->demod_gain;
}

static int lpf_decimate(dsp_state_t *s, float fi, float fq, float *oi, float *oq)
{
    s->fir_buf_i[s->fir_idx] = fi;
    s->fir_buf_q[s->fir_idx] = fq;
    s->decim_count++;
    if (s->decim_count < DSP_DECIMATION) {
        s->fir_idx = (s->fir_idx + 1) % DSP_FIR_TAPS;
        return 0;
    }
    s->decim_count = 0;

    float si = 0, sq = 0;
    int idx = s->fir_idx;
    for (int t = 0; t < DSP_FIR_TAPS; t++) {
        si += s->fir_coeffs[t] * s->fir_buf_i[idx];
        sq += s->fir_coeffs[t] * s->fir_buf_q[idx];
        if (--idx < 0) idx = DSP_FIR_TAPS - 1;
    }
    s->fir_idx = (s->fir_idx + 1) % DSP_FIR_TAPS;
    *oi = si; *oq = sq;
    return 1;
}

static void agc_apply(dsp_state_t *s, float *si, float *sq)
{
    float mag2 = (*si) * (*si) + (*sq) * (*sq);
    float mag = sqrtf(mag2 + 1e-12f);
    float target_gain = (mag > 1e-6f) ? (s->agc_ref / mag) : s->agc_gain;
    s->agc_gain += s->agc_alpha * (target_gain - s->agc_gain);
    if (s->agc_gain > 100.0f) s->agc_gain = 100.0f;
    if (s->agc_gain < 0.001f) s->agc_gain = 0.001f;
    *si *= s->agc_gain;
    *sq *= s->agc_gain;
}

static const float rrc_sym_taps[51] = {
     0.008296182f,  0.009814062f,  0.010336751f,  0.009666932f,  0.007721221f,  0.004553221f,
     0.000364146f, -0.004501144f, -0.009573439f, -0.014292968f, -0.018055279f, -0.020265396f,
    -0.020395366f, -0.018039707f, -0.012963163f, -0.005135712f,  0.005249236f,  0.017775984f,
     0.031827811f,  0.046627161f,  0.061291724f,  0.074901745f,  0.086572703f,  0.095526880f,
     0.101157307f,  0.103078214f,  0.101157307f,  0.095526880f,  0.086572703f,  0.074901745f,
     0.061291724f,  0.046627161f,  0.031827811f,  0.017775984f,  0.005249236f, -0.005135712f,
    -0.012963163f, -0.018039707f, -0.020395366f, -0.020265396f, -0.018055279f, -0.014292968f,
    -0.009573439f, -0.004501144f,  0.000364146f,  0.004553221f,  0.007721221f,  0.009666932f,
     0.010336751f,  0.009814062f,  0.008296182f,
};

static float rrc_filter(dsp_state_t *s, float sample)
{
    s->rrc_buf[s->rrc_idx] = sample;
    float acc = 0;
    int idx = s->rrc_idx;
    for (int t = 0; t < 51; t++) {
        acc += rrc_sym_taps[t] * s->rrc_buf[idx];
        if (--idx < 0) idx = 50;
    }
    s->rrc_idx = (s->rrc_idx + 1) % 51;
    return acc;
}

static const float rrc_fsk4_taps[RRC_FSK4_TAPS] = {
    +0.0099550f, -0.0098412f, -0.0312498f, -0.0443077f, -0.0394415f, -0.0112286f,
    +0.0388649f, +0.1019444f, +0.1637632f, +0.2088575f, +0.2253675f, +0.2088575f,
    +0.1637632f, +0.1019444f, +0.0388649f, -0.0112286f, -0.0394415f, -0.0443077f,
    -0.0312498f, -0.0098412f, +0.0099550f,
};

static float rrc_fsk4_filter(dsp_state_t *s, float sample)
{
    s->rrc_fsk4_buf[s->rrc_fsk4_idx] = sample;
    float acc = 0;
    int idx = s->rrc_fsk4_idx;
    for (int t = 0; t < RRC_FSK4_TAPS; t++) {
        acc += rrc_fsk4_taps[t] * s->rrc_fsk4_buf[idx];
        if (--idx < 0) idx = RRC_FSK4_TAPS - 1;
    }
    s->rrc_fsk4_idx = (s->rrc_fsk4_idx + 1) % RRC_FSK4_TAPS;
    return acc;
}

static void rrc_filter_iq(dsp_state_t *s, float si, float sq, float *oi, float *oq)
{
    int widx = s->rrc_iq_idx;
    s->rrc_i_buf[widx] = si;
    s->rrc_q_buf[widx] = sq;

    float ai = 0, aq = 0;
    int idx = widx;
    for (int t = 0; t < RRC_SYM_TAPS; t++) {
        ai += rrc_sym_taps[t] * s->rrc_i_buf[idx];
        aq += rrc_sym_taps[t] * s->rrc_q_buf[idx];
        if (--idx < 0) idx = RRC_SYM_TAPS - 1;
    }
    s->rrc_iq_idx = (widx + 1) % RRC_SYM_TAPS;
    *oi = ai;
    *oq = aq;
}

static int16_t fm_demod(dsp_state_t *s, float si, float sq)
{
    float re = s->prev_i * si + s->prev_q * sq;
    float im = s->prev_i * sq - s->prev_q * si;
    float phase = atan2f(-im, re);
    s->prev_i = si; s->prev_q = sq;

    float v = phase * s->demod_gain;
    if (v >  32767.0f) v =  32767.0f;
    if (v < -32767.0f) v = -32767.0f;
    return (int16_t)v;
}

static int cqpsk_sample(dsp_state_t *s, float si, float sq,
                         int16_t *out, int maxn)
{
    float co = cosf(s->c_phase), sn = sinf(s->c_phase);
    float mi = si * co + sq * sn;
    float mq = -si * sn + sq * co;

    float si2 = (mi >= 0) ? 1.0f : -1.0f;
    float sq2 = (mq >= 0) ? 1.0f : -1.0f;
    float pe = sq2 * mi - si2 * mq;

    s->c_freq += s->c_beta * pe;
    s->c_phase += s->c_freq + s->c_alpha * pe;
    while (s->c_phase > M_PI) s->c_phase -= 2.0f * M_PI;
    while (s->c_phase < -M_PI) s->c_phase += 2.0f * M_PI;
    if (s->c_freq > 0.5f) s->c_freq = 0.5f;
    if (s->c_freq < -0.5f) s->c_freq = -0.5f;

    s->g_sample_idx++;
    if (s->g_sample_idx == s->g_half) {
        s->g_di[1] = mi; s->g_dq[1] = mq;
    }

    s->g_clock += 1.0f;
    if (s->g_clock < s->g_period) return 0;

    s->g_clock -= s->g_period;
    s->g_sample_idx = 0;
    s->g_di[2] = mi; s->g_dq[2] = mq;

    float te = (s->g_di[2] - s->g_di[0]) * s->g_di[1]
             + (s->g_dq[2] - s->g_dq[0]) * s->g_dq[1];
    s->g_period += s->g_gain_omega * te;
    float omin = (float)DSP_SPS * (1.0f - s->g_omega_rel);
    float omax = (float)DSP_SPS * (1.0f + s->g_omega_rel);
    if (s->g_period < omin) s->g_period = omin;
    if (s->g_period > omax) s->g_period = omax;
    s->g_half = (int)(s->g_period * 0.5f);
    s->g_di[0] = s->g_di[2]; s->g_dq[0] = s->g_dq[2];

    float di2 = mi * s->diff_prev_i + mq * s->diff_prev_q;
    float dq2 = mq * s->diff_prev_i - mi * s->diff_prev_q;
    s->diff_prev_i = mi; s->diff_prev_q = mq;

    float angle = atan2f(dq2, di2);
    angle += s->cqpsk_polarity * (M_PI / 4.0f);
    while (angle > M_PI) angle -= 2.0f * M_PI;
    while (angle < -M_PI) angle += 2.0f * M_PI;
    float rescaled = angle / (M_PI / 4.0f);

    float v = rescaled * s->demod_gain;
    if (v > 32767.0f) v = 32767.0f;
    if (v < -32767.0f) v = -32767.0f;
    int16_t sym = (int16_t)v;

    int n = 0;
    for (int r = 0; r < DSP_SPS && n < maxn; r++)
        out[n++] = sym;
    return n;
}

static int16_t diff_4fsk_sample(dsp_state_t *s, float si, float sq)
{
    float fi = si, fq = sq;

    int d_idx = s->diff_ring_idx;
    float prev_i = s->diff_ring_i[d_idx];
    float prev_q = s->diff_ring_q[d_idx];
    s->diff_ring_i[d_idx] = fi;
    s->diff_ring_q[d_idx] = fq;
    s->diff_ring_idx = (d_idx + 1) % DIFF_DELAY;

    if (!s->diff_ring_filled) {
        if (s->diff_ring_idx == 0)
            s->diff_ring_filled = 1;
        return 0;
    }

    float diff_i = fi * prev_i + fq * prev_q;
    float diff_q = fq * prev_i - fi * prev_q;

    float phase = atan2f(diff_q, diff_i);

    float equalized = (phase + s->eq_pll) * s->eq_gain;

    if (equalized > M_PI)  equalized -= 2.0f * M_PI;
    if (equalized < -M_PI) equalized += 2.0f * M_PI;

    float v = equalized * s->diff_output_scale;
    if (v > 32767.0f) v = 32767.0f;
    if (v < -32767.0f) v = -32767.0f;

    {
        static int diag_count = 0;
        static int diag_enabled = 0;
        extern void sys_log(unsigned char color, const char *fmt, ...);
        if (s->diff_ring_filled && diag_count == 0) {
            diag_enabled = 1;
        }
        if (diag_enabled && diag_count < 50) {
            if (diag_count % 10 == 0) {
                sys_log(2, "DIFF[%d] phase=%.3f eq=%.3f out=%d",
                        diag_count, phase, equalized, (int)v);
            }
            diag_count++;
            if (diag_count >= 50) diag_enabled = 0;
        }
    }

    return (int16_t)v;
}


#define FSK4_NSTEPS  128
#define FSK4_NTAPS   8
#define K_SPREAD      0.0100
#define K_TIMING      0.025
#define K_FINE_FREQ   0.125
#define K_COARSE_FREQ 0.00125
#define SPREAD_MIN    1.6
#define SPREAD_MAX    3.0
#define SPREAD_NOM    2.0

static const float FSK4_TAPS[FSK4_NSTEPS + 1][FSK4_NTAPS] = {
 { 0.00000e+00f, 0.00000e+00f, 0.00000e+00f, 0.00000e+00f, 1.00000e+00f, 0.00000e+00f, 0.00000e+00f, 0.00000e+00f }, 
 {-1.54700e-04f, 8.53777e-04f,-2.76968e-03f, 7.89295e-03f, 9.98534e-01f,-5.41054e-03f, 1.24642e-03f,-1.98993e-04f }, 
 {-3.09412e-04f, 1.70888e-03f,-5.55134e-03f, 1.58840e-02f, 9.96891e-01f,-1.07209e-02f, 2.47942e-03f,-3.96391e-04f }, 
 {-4.64053e-04f, 2.56486e-03f,-8.34364e-03f, 2.39714e-02f, 9.95074e-01f,-1.59305e-02f, 3.69852e-03f,-5.92100e-04f }, 
 {-6.18544e-04f, 3.42130e-03f,-1.11453e-02f, 3.21531e-02f, 9.93082e-01f,-2.10389e-02f, 4.90322e-03f,-7.86031e-04f }, 
 {-7.72802e-04f, 4.27773e-03f,-1.39548e-02f, 4.04274e-02f, 9.90917e-01f,-2.60456e-02f, 6.09305e-03f,-9.78093e-04f }, 
 {-9.26747e-04f, 5.13372e-03f,-1.67710e-02f, 4.87921e-02f, 9.88580e-01f,-3.09503e-02f, 7.26755e-03f,-1.16820e-03f }, 
 {-1.08030e-03f, 5.98883e-03f,-1.95925e-02f, 5.72454e-02f, 9.86071e-01f,-3.57525e-02f, 8.42626e-03f,-1.35627e-03f }, 
 {-1.23337e-03f, 6.84261e-03f,-2.24178e-02f, 6.57852e-02f, 9.83392e-01f,-4.04519e-02f, 9.56876e-03f,-1.54221e-03f }, 
 {-1.38589e-03f, 7.69462e-03f,-2.52457e-02f, 7.44095e-02f, 9.80543e-01f,-4.50483e-02f, 1.06946e-02f,-1.72594e-03f }, 
 {-1.53777e-03f, 8.54441e-03f,-2.80746e-02f, 8.31162e-02f, 9.77526e-01f,-4.95412e-02f, 1.18034e-02f,-1.90738e-03f }, 
 {-1.68894e-03f, 9.39154e-03f,-3.09033e-02f, 9.19033e-02f, 9.74342e-01f,-5.39305e-02f, 1.28947e-02f,-2.08645e-03f }, 
 {-1.83931e-03f, 1.02356e-02f,-3.37303e-02f, 1.00769e-01f, 9.70992e-01f,-5.82159e-02f, 1.39681e-02f,-2.26307e-03f }, 
 {-1.98880e-03f, 1.10760e-02f,-3.65541e-02f, 1.09710e-01f, 9.67477e-01f,-6.23972e-02f, 1.50233e-02f,-2.43718e-03f }, 
 {-2.13733e-03f, 1.19125e-02f,-3.93735e-02f, 1.18725e-01f, 9.63798e-01f,-6.64743e-02f, 1.60599e-02f,-2.60868e-03f }, 
 {-2.28483e-03f, 1.27445e-02f,-4.21869e-02f, 1.27812e-01f, 9.59958e-01f,-7.04471e-02f, 1.70776e-02f,-2.77751e-03f }, 
 {-2.43121e-03f, 1.35716e-02f,-4.49929e-02f, 1.36968e-01f, 9.55956e-01f,-7.43154e-02f, 1.80759e-02f,-2.94361e-03f }, 
 {-2.57640e-03f, 1.43934e-02f,-4.77900e-02f, 1.46192e-01f, 9.51795e-01f,-7.80792e-02f, 1.90545e-02f,-3.10689e-03f }, 
 {-2.72032e-03f, 1.52095e-02f,-5.05770e-02f, 1.55480e-01f, 9.47477e-01f,-8.17385e-02f, 2.00132e-02f,-3.26730e-03f }, 
 {-2.86289e-03f, 1.60193e-02f,-5.33522e-02f, 1.64831e-01f, 9.43001e-01f,-8.52933e-02f, 2.09516e-02f,-3.42477e-03f }, 
 {-3.00403e-03f, 1.68225e-02f,-5.61142e-02f, 1.74242e-01f, 9.38371e-01f,-8.87435e-02f, 2.18695e-02f,-3.57923e-03f }, 
 {-3.14367e-03f, 1.76185e-02f,-5.88617e-02f, 1.83711e-01f, 9.33586e-01f,-9.20893e-02f, 2.27664e-02f,-3.73062e-03f }, 
 {-3.28174e-03f, 1.84071e-02f,-6.15931e-02f, 1.93236e-01f, 9.28650e-01f,-9.53307e-02f, 2.36423e-02f,-3.87888e-03f }, 
 {-3.41815e-03f, 1.91877e-02f,-6.43069e-02f, 2.02814e-01f, 9.23564e-01f,-9.84679e-02f, 2.44967e-02f,-4.02397e-03f }, 
 {-3.55283e-03f, 1.99599e-02f,-6.70018e-02f, 2.12443e-01f, 9.18329e-01f,-1.01501e-01f, 2.53295e-02f,-4.16581e-03f }, 
 {-3.68570e-03f, 2.07233e-02f,-6.96762e-02f, 2.22120e-01f, 9.12947e-01f,-1.04430e-01f, 2.61404e-02f,-4.30435e-03f }, 
 {-3.81671e-03f, 2.14774e-02f,-7.23286e-02f, 2.31843e-01f, 9.07420e-01f,-1.07256e-01f, 2.69293e-02f,-4.43955e-03f }, 
 {-3.94576e-03f, 2.22218e-02f,-7.49577e-02f, 2.41609e-01f, 9.01749e-01f,-1.09978e-01f, 2.76957e-02f,-4.57135e-03f }, 
 {-4.07279e-03f, 2.29562e-02f,-7.75620e-02f, 2.51417e-01f, 8.95936e-01f,-1.12597e-01f, 2.84397e-02f,-4.69970e-03f }, 
 {-4.19774e-03f, 2.36801e-02f,-8.01399e-02f, 2.61263e-01f, 8.89984e-01f,-1.15113e-01f, 2.91609e-02f,-4.82456e-03f }, 
 {-4.32052e-03f, 2.43930e-02f,-8.26900e-02f, 2.71144e-01f, 8.83893e-01f,-1.17526e-01f, 2.98593e-02f,-4.94589e-03f }, 
 {-4.44107e-03f, 2.50946e-02f,-8.52109e-02f, 2.81060e-01f, 8.77666e-01f,-1.19837e-01f, 3.05345e-02f,-5.06363e-03f }, 
 {-4.55932e-03f, 2.57844e-02f,-8.77011e-02f, 2.91006e-01f, 8.71305e-01f,-1.22047e-01f, 3.11866e-02f,-5.17776e-03f }, 
 {-4.67520e-03f, 2.64621e-02f,-9.01591e-02f, 3.00980e-01f, 8.64812e-01f,-1.24154e-01f, 3.18153e-02f,-5.28823e-03f }, 
 {-4.78866e-03f, 2.71272e-02f,-9.25834e-02f, 3.10980e-01f, 8.58189e-01f,-1.26161e-01f, 3.24205e-02f,-5.39500e-03f }, 
 {-4.89961e-03f, 2.77794e-02f,-9.49727e-02f, 3.21004e-01f, 8.51437e-01f,-1.28068e-01f, 3.30021e-02f,-5.49804e-03f }, 
 {-5.00800e-03f, 2.84182e-02f,-9.73254e-02f, 3.31048e-01f, 8.44559e-01f,-1.29874e-01f, 3.35600e-02f,-5.59731e-03f }, 
 {-5.11376e-03f, 2.90433e-02f,-9.96402e-02f, 3.41109e-01f, 8.37557e-01f,-1.31581e-01f, 3.40940e-02f,-5.69280e-03f }, 
 {-5.21683e-03f, 2.96543e-02f,-1.01915e-01f, 3.51186e-01f, 8.30432e-01f,-1.33189e-01f, 3.46042e-02f,-5.78446e-03f }, 
 {-5.31716e-03f, 3.02507e-02f,-1.04150e-01f, 3.61276e-01f, 8.23188e-01f,-1.34699e-01f, 3.50903e-02f,-5.87227e-03f }, 
 {-5.41467e-03f, 3.08323e-02f,-1.06342e-01f, 3.71376e-01f, 8.15826e-01f,-1.36111e-01f, 3.55525e-02f,-5.95620e-03f }, 
 {-5.50931e-03f, 3.13987e-02f,-1.08490e-01f, 3.81484e-01f, 8.08348e-01f,-1.37426e-01f, 3.59905e-02f,-6.03624e-03f }, 
 {-5.60103e-03f, 3.19495e-02f,-1.10593e-01f, 3.91596e-01f, 8.00757e-01f,-1.38644e-01f, 3.64044e-02f,-6.11236e-03f }, 
 {-5.68976e-03f, 3.24843e-02f,-1.12650e-01f, 4.01710e-01f, 7.93055e-01f,-1.39767e-01f, 3.67941e-02f,-6.18454e-03f }, 
 {-5.77544e-03f, 3.30027e-02f,-1.14659e-01f, 4.11823e-01f, 7.85244e-01f,-1.40794e-01f, 3.71596e-02f,-6.25277e-03f }, 
 {-5.85804e-03f, 3.35046e-02f,-1.16618e-01f, 4.21934e-01f, 7.77327e-01f,-1.41727e-01f, 3.75010e-02f,-6.31703e-03f }, 
 {-5.93749e-03f, 3.39894e-02f,-1.18526e-01f, 4.32038e-01f, 7.69305e-01f,-1.42566e-01f, 3.78182e-02f,-6.37730e-03f }, 
 {-6.01374e-03f, 3.44568e-02f,-1.20382e-01f, 4.42134e-01f, 7.61181e-01f,-1.43313e-01f, 3.81111e-02f,-6.43358e-03f }, 
 {-6.08674e-03f, 3.49066e-02f,-1.22185e-01f, 4.52218e-01f, 7.52958e-01f,-1.43968e-01f, 3.83800e-02f,-6.48585e-03f }, 
 {-6.15644e-03f, 3.53384e-02f,-1.23933e-01f, 4.62289e-01f, 7.44637e-01f,-1.44531e-01f, 3.86247e-02f,-6.53412e-03f }, 
 {-6.22280e-03f, 3.57519e-02f,-1.25624e-01f, 4.72342e-01f, 7.36222e-01f,-1.45004e-01f, 3.88454e-02f,-6.57836e-03f }, 
 {-6.28577e-03f, 3.61468e-02f,-1.27258e-01f, 4.82377e-01f, 7.27714e-01f,-1.45387e-01f, 3.90420e-02f,-6.61859e-03f }, 
 {-6.34530e-03f, 3.65227e-02f,-1.28832e-01f, 4.92389e-01f, 7.19116e-01f,-1.45682e-01f, 3.92147e-02f,-6.65479e-03f }, 
 {-6.40135e-03f, 3.68795e-02f,-1.30347e-01f, 5.02377e-01f, 7.10431e-01f,-1.45889e-01f, 3.93636e-02f,-6.68698e-03f }, 
 {-6.45388e-03f, 3.72167e-02f,-1.31800e-01f, 5.12337e-01f, 7.01661e-01f,-1.46009e-01f, 3.94886e-02f,-6.71514e-03f }, 
 {-6.50285e-03f, 3.75341e-02f,-1.33190e-01f, 5.22267e-01f, 6.92808e-01f,-1.46043e-01f, 3.95900e-02f,-6.73929e-03f }, 
 {-6.54823e-03f, 3.78315e-02f,-1.34515e-01f, 5.32164e-01f, 6.83875e-01f,-1.45993e-01f, 3.96678e-02f,-6.75943e-03f }, 
 {-6.58996e-03f, 3.81085e-02f,-1.35775e-01f, 5.42025e-01f, 6.74865e-01f,-1.45859e-01f, 3.97222e-02f,-6.77557e-03f }, 
 {-6.62802e-03f, 3.83650e-02f,-1.36969e-01f, 5.51849e-01f, 6.65779e-01f,-1.45641e-01f, 3.97532e-02f,-6.78771e-03f }, 
 {-6.66238e-03f, 3.86006e-02f,-1.38094e-01f, 5.61631e-01f, 6.56621e-01f,-1.45343e-01f, 3.97610e-02f,-6.79588e-03f }, 
 {-6.69300e-03f, 3.88151e-02f,-1.39150e-01f, 5.71370e-01f, 6.47394e-01f,-1.44963e-01f, 3.97458e-02f,-6.80007e-03f }, 
 {-6.71985e-03f, 3.90083e-02f,-1.40136e-01f, 5.81063e-01f, 6.38099e-01f,-1.44503e-01f, 3.97077e-02f,-6.80032e-03f }, 
 {-6.74291e-03f, 3.91800e-02f,-1.41050e-01f, 5.90706e-01f, 6.28739e-01f,-1.43965e-01f, 3.96469e-02f,-6.79662e-03f }, 
 {-6.76214e-03f, 3.93299e-02f,-1.41891e-01f, 6.00298e-01f, 6.19318e-01f,-1.43350e-01f, 3.95635e-02f,-6.78902e-03f }, 
 {-6.77751e-03f, 3.94578e-02f,-1.42658e-01f, 6.09836e-01f, 6.09836e-01f,-1.42658e-01f, 3.94578e-02f,-6.77751e-03f }, 
 {-6.78902e-03f, 3.95635e-02f,-1.43350e-01f, 6.19318e-01f, 6.00298e-01f,-1.41891e-01f, 3.93299e-02f,-6.76214e-03f }, 
 {-6.79662e-03f, 3.96469e-02f,-1.43965e-01f, 6.28739e-01f, 5.90706e-01f,-1.41050e-01f, 3.91800e-02f,-6.74291e-03f }, 
 {-6.80032e-03f, 3.97077e-02f,-1.44503e-01f, 6.38099e-01f, 5.81063e-01f,-1.40136e-01f, 3.90083e-02f,-6.71985e-03f }, 
 {-6.80007e-03f, 3.97458e-02f,-1.44963e-01f, 6.47394e-01f, 5.71370e-01f,-1.39150e-01f, 3.88151e-02f,-6.69300e-03f }, 
 {-6.79588e-03f, 3.97610e-02f,-1.45343e-01f, 6.56621e-01f, 5.61631e-01f,-1.38094e-01f, 3.86006e-02f,-6.66238e-03f }, 
 {-6.78771e-03f, 3.97532e-02f,-1.45641e-01f, 6.65779e-01f, 5.51849e-01f,-1.36969e-01f, 3.83650e-02f,-6.62802e-03f }, 
 {-6.77557e-03f, 3.97222e-02f,-1.45859e-01f, 6.74865e-01f, 5.42025e-01f,-1.35775e-01f, 3.81085e-02f,-6.58996e-03f }, 
 {-6.75943e-03f, 3.96678e-02f,-1.45993e-01f, 6.83875e-01f, 5.32164e-01f,-1.34515e-01f, 3.78315e-02f,-6.54823e-03f }, 
 {-6.73929e-03f, 3.95900e-02f,-1.46043e-01f, 6.92808e-01f, 5.22267e-01f,-1.33190e-01f, 3.75341e-02f,-6.50285e-03f }, 
 {-6.71514e-03f, 3.94886e-02f,-1.46009e-01f, 7.01661e-01f, 5.12337e-01f,-1.31800e-01f, 3.72167e-02f,-6.45388e-03f }, 
 {-6.68698e-03f, 3.93636e-02f,-1.45889e-01f, 7.10431e-01f, 5.02377e-01f,-1.30347e-01f, 3.68795e-02f,-6.40135e-03f }, 
 {-6.65479e-03f, 3.92147e-02f,-1.45682e-01f, 7.19116e-01f, 4.92389e-01f,-1.28832e-01f, 3.65227e-02f,-6.34530e-03f }, 
 {-6.61859e-03f, 3.90420e-02f,-1.45387e-01f, 7.27714e-01f, 4.82377e-01f,-1.27258e-01f, 3.61468e-02f,-6.28577e-03f }, 
 {-6.57836e-03f, 3.88454e-02f,-1.45004e-01f, 7.36222e-01f, 4.72342e-01f,-1.25624e-01f, 3.57519e-02f,-6.22280e-03f }, 
 {-6.53412e-03f, 3.86247e-02f,-1.44531e-01f, 7.44637e-01f, 4.62289e-01f,-1.23933e-01f, 3.53384e-02f,-6.15644e-03f }, 
 {-6.48585e-03f, 3.83800e-02f,-1.43968e-01f, 7.52958e-01f, 4.52218e-01f,-1.22185e-01f, 3.49066e-02f,-6.08674e-03f }, 
 {-6.43358e-03f, 3.81111e-02f,-1.43313e-01f, 7.61181e-01f, 4.42134e-01f,-1.20382e-01f, 3.44568e-02f,-6.01374e-03f }, 
 {-6.37730e-03f, 3.78182e-02f,-1.42566e-01f, 7.69305e-01f, 4.32038e-01f,-1.18526e-01f, 3.39894e-02f,-5.93749e-03f }, 
 {-6.31703e-03f, 3.75010e-02f,-1.41727e-01f, 7.77327e-01f, 4.21934e-01f,-1.16618e-01f, 3.35046e-02f,-5.85804e-03f }, 
 {-6.25277e-03f, 3.71596e-02f,-1.40794e-01f, 7.85244e-01f, 4.11823e-01f,-1.14659e-01f, 3.30027e-02f,-5.77544e-03f }, 
 {-6.18454e-03f, 3.67941e-02f,-1.39767e-01f, 7.93055e-01f, 4.01710e-01f,-1.12650e-01f, 3.24843e-02f,-5.68976e-03f }, 
 {-6.11236e-03f, 3.64044e-02f,-1.38644e-01f, 8.00757e-01f, 3.91596e-01f,-1.10593e-01f, 3.19495e-02f,-5.60103e-03f }, 
 {-6.03624e-03f, 3.59905e-02f,-1.37426e-01f, 8.08348e-01f, 3.81484e-01f,-1.08490e-01f, 3.13987e-02f,-5.50931e-03f }, 
 {-5.95620e-03f, 3.55525e-02f,-1.36111e-01f, 8.15826e-01f, 3.71376e-01f,-1.06342e-01f, 3.08323e-02f,-5.41467e-03f }, 
 {-5.87227e-03f, 3.50903e-02f,-1.34699e-01f, 8.23188e-01f, 3.61276e-01f,-1.04150e-01f, 3.02507e-02f,-5.31716e-03f }, 
 {-5.78446e-03f, 3.46042e-02f,-1.33189e-01f, 8.30432e-01f, 3.51186e-01f,-1.01915e-01f, 2.96543e-02f,-5.21683e-03f }, 
 {-5.69280e-03f, 3.40940e-02f,-1.31581e-01f, 8.37557e-01f, 3.41109e-01f,-9.96402e-02f, 2.90433e-02f,-5.11376e-03f }, 
 {-5.59731e-03f, 3.35600e-02f,-1.29874e-01f, 8.44559e-01f, 3.31048e-01f,-9.73254e-02f, 2.84182e-02f,-5.00800e-03f }, 
 {-5.49804e-03f, 3.30021e-02f,-1.28068e-01f, 8.51437e-01f, 3.21004e-01f,-9.49727e-02f, 2.77794e-02f,-4.89961e-03f }, 
 {-5.39500e-03f, 3.24205e-02f,-1.26161e-01f, 8.58189e-01f, 3.10980e-01f,-9.25834e-02f, 2.71272e-02f,-4.78866e-03f }, 
 {-5.28823e-03f, 3.18153e-02f,-1.24154e-01f, 8.64812e-01f, 3.00980e-01f,-9.01591e-02f, 2.64621e-02f,-4.67520e-03f }, 
 {-5.17776e-03f, 3.11866e-02f,-1.22047e-01f, 8.71305e-01f, 2.91006e-01f,-8.77011e-02f, 2.57844e-02f,-4.55932e-03f }, 
 {-5.06363e-03f, 3.05345e-02f,-1.19837e-01f, 8.77666e-01f, 2.81060e-01f,-8.52109e-02f, 2.50946e-02f,-4.44107e-03f }, 
 {-4.94589e-03f, 2.98593e-02f,-1.17526e-01f, 8.83893e-01f, 2.71144e-01f,-8.26900e-02f, 2.43930e-02f,-4.32052e-03f }, 
 {-4.82456e-03f, 2.91609e-02f,-1.15113e-01f, 8.89984e-01f, 2.61263e-01f,-8.01399e-02f, 2.36801e-02f,-4.19774e-03f }, 
 {-4.69970e-03f, 2.84397e-02f,-1.12597e-01f, 8.95936e-01f, 2.51417e-01f,-7.75620e-02f, 2.29562e-02f,-4.07279e-03f }, 
 {-4.57135e-03f, 2.76957e-02f,-1.09978e-01f, 9.01749e-01f, 2.41609e-01f,-7.49577e-02f, 2.22218e-02f,-3.94576e-03f }, 
 {-4.43955e-03f, 2.69293e-02f,-1.07256e-01f, 9.07420e-01f, 2.31843e-01f,-7.23286e-02f, 2.14774e-02f,-3.81671e-03f }, 
 {-4.30435e-03f, 2.61404e-02f,-1.04430e-01f, 9.12947e-01f, 2.22120e-01f,-6.96762e-02f, 2.07233e-02f,-3.68570e-03f }, 
 {-4.16581e-03f, 2.53295e-02f,-1.01501e-01f, 9.18329e-01f, 2.12443e-01f,-6.70018e-02f, 1.99599e-02f,-3.55283e-03f }, 
 {-4.02397e-03f, 2.44967e-02f,-9.84679e-02f, 9.23564e-01f, 2.02814e-01f,-6.43069e-02f, 1.91877e-02f,-3.41815e-03f }, 
 {-3.87888e-03f, 2.36423e-02f,-9.53307e-02f, 9.28650e-01f, 1.93236e-01f,-6.15931e-02f, 1.84071e-02f,-3.28174e-03f }, 
 {-3.73062e-03f, 2.27664e-02f,-9.20893e-02f, 9.33586e-01f, 1.83711e-01f,-5.88617e-02f, 1.76185e-02f,-3.14367e-03f }, 
 {-3.57923e-03f, 2.18695e-02f,-8.87435e-02f, 9.38371e-01f, 1.74242e-01f,-5.61142e-02f, 1.68225e-02f,-3.00403e-03f }, 
 {-3.42477e-03f, 2.09516e-02f,-8.52933e-02f, 9.43001e-01f, 1.64831e-01f,-5.33522e-02f, 1.60193e-02f,-2.86289e-03f }, 
 {-3.26730e-03f, 2.00132e-02f,-8.17385e-02f, 9.47477e-01f, 1.55480e-01f,-5.05770e-02f, 1.52095e-02f,-2.72032e-03f }, 
 {-3.10689e-03f, 1.90545e-02f,-7.80792e-02f, 9.51795e-01f, 1.46192e-01f,-4.77900e-02f, 1.43934e-02f,-2.57640e-03f }, 
 {-2.94361e-03f, 1.80759e-02f,-7.43154e-02f, 9.55956e-01f, 1.36968e-01f,-4.49929e-02f, 1.35716e-02f,-2.43121e-03f }, 
 {-2.77751e-03f, 1.70776e-02f,-7.04471e-02f, 9.59958e-01f, 1.27812e-01f,-4.21869e-02f, 1.27445e-02f,-2.28483e-03f }, 
 {-2.60868e-03f, 1.60599e-02f,-6.64743e-02f, 9.63798e-01f, 1.18725e-01f,-3.93735e-02f, 1.19125e-02f,-2.13733e-03f }, 
 {-2.43718e-03f, 1.50233e-02f,-6.23972e-02f, 9.67477e-01f, 1.09710e-01f,-3.65541e-02f, 1.10760e-02f,-1.98880e-03f }, 
 {-2.26307e-03f, 1.39681e-02f,-5.82159e-02f, 9.70992e-01f, 1.00769e-01f,-3.37303e-02f, 1.02356e-02f,-1.83931e-03f }, 
 {-2.08645e-03f, 1.28947e-02f,-5.39305e-02f, 9.74342e-01f, 9.19033e-02f,-3.09033e-02f, 9.39154e-03f,-1.68894e-03f }, 
 {-1.90738e-03f, 1.18034e-02f,-4.95412e-02f, 9.77526e-01f, 8.31162e-02f,-2.80746e-02f, 8.54441e-03f,-1.53777e-03f }, 
 {-1.72594e-03f, 1.06946e-02f,-4.50483e-02f, 9.80543e-01f, 7.44095e-02f,-2.52457e-02f, 7.69462e-03f,-1.38589e-03f }, 
 {-1.54221e-03f, 9.56876e-03f,-4.04519e-02f, 9.83392e-01f, 6.57852e-02f,-2.24178e-02f, 6.84261e-03f,-1.23337e-03f }, 
 {-1.35627e-03f, 8.42626e-03f,-3.57525e-02f, 9.86071e-01f, 5.72454e-02f,-1.95925e-02f, 5.98883e-03f,-1.08030e-03f }, 
 {-1.16820e-03f, 7.26755e-03f,-3.09503e-02f, 9.88580e-01f, 4.87921e-02f,-1.67710e-02f, 5.13372e-03f,-9.26747e-04f }, 
 {-9.78093e-04f, 6.09305e-03f,-2.60456e-02f, 9.90917e-01f, 4.04274e-02f,-1.39548e-02f, 4.27773e-03f,-7.72802e-04f }, 
 {-7.86031e-04f, 4.90322e-03f,-2.10389e-02f, 9.93082e-01f, 3.21531e-02f,-1.11453e-02f, 3.42130e-03f,-6.18544e-04f }, 
 {-5.92100e-04f, 3.69852e-03f,-1.59305e-02f, 9.95074e-01f, 2.39714e-02f,-8.34364e-03f, 2.56486e-03f,-4.64053e-04f }, 
 {-3.96391e-04f, 2.47942e-03f,-1.07209e-02f, 9.96891e-01f, 1.58840e-02f,-5.55134e-03f, 1.70888e-03f,-3.09412e-04f }, 
 {-1.98993e-04f, 1.24642e-03f,-5.41054e-03f, 9.98534e-01f, 7.89295e-03f,-2.76968e-03f, 8.53777e-04f,-1.54700e-04f }, 
 { 0.00000e+00f, 0.00000e+00f, 0.00000e+00f, 1.00000e+00f, 0.00000e+00f, 0.00000e+00f, 0.00000e+00f, 0.00000e+00f }, 
};

static float fm_discriminate_linear(dsp_state_t *s, float si, float sq)
{
    float re = s->ft_prev_i * si + s->ft_prev_q * sq;
    float im = s->ft_prev_i * sq - s->ft_prev_q * si;
    s->ft_prev_i = si;
    s->ft_prev_q = sq;
    return atan2f(im, re);
}

static int fsk4_track_sample(dsp_state_t *s, float phase, float *sym_out)
{
    s->ft_history[s->ft_history_idx] = phase;
    s->ft_history_idx = (s->ft_history_idx + 1) & 7;  

    s->ft_symbol_clock += s->ft_symbol_time;
    if (s->ft_symbol_clock < 1.0) return 0;
    s->ft_symbol_clock -= 1.0;

    double mu = s->ft_symbol_clock / s->ft_symbol_time;

    int imu = (int)(0.5 + (double)FSK4_NSTEPS * mu);
    if (imu < 0) imu = 0;
    if (imu > FSK4_NSTEPS) imu = FSK4_NSTEPS;
    int imu_p1 = imu + 1;
    if (imu_p1 > FSK4_NSTEPS) imu_p1 = FSK4_NSTEPS;

    double interp = 0.0, interp_p1 = 0.0;
    int j = s->ft_history_idx;
    for (int i = 0; i < FSK4_NTAPS; i++) {
        double h = (double)s->ft_history[j];
        interp    += (double)FSK4_TAPS[imu   ][i] * h;
        interp_p1 += (double)FSK4_TAPS[imu_p1][i] * h;
        j = (j + 1) & 7;
    }

    interp    -= s->ft_fine_freq;
    interp_p1 -= s->ft_fine_freq;

    double output = 2.0 * interp / s->ft_symbol_spread;

    double err;
    if (interp < -s->ft_symbol_spread) {
        err = interp + 1.5 * s->ft_symbol_spread;
        s->ft_symbol_spread -= err * 0.5 * K_SPREAD;
    } else if (interp < 0.0) {
        err = interp + 0.5 * s->ft_symbol_spread;
        if (interp < -0.25 * s->ft_symbol_spread)
            s->ft_symbol_spread -= err * K_SPREAD;
    } else if (interp < s->ft_symbol_spread) {
        err = interp - 0.5 * s->ft_symbol_spread;
        if (interp >  0.25 * s->ft_symbol_spread)
            s->ft_symbol_spread += err * K_SPREAD;
    } else {
        err = interp - 1.5 * s->ft_symbol_spread;
        s->ft_symbol_spread += err * 0.5 * K_SPREAD;
    }

    if (interp_p1 < interp) {
        s->ft_symbol_clock += err * K_TIMING;
    } else {
        s->ft_symbol_clock -= err * K_TIMING;
    }

    if (s->ft_symbol_spread < SPREAD_MIN) s->ft_symbol_spread = SPREAD_MIN;
    if (s->ft_symbol_spread > SPREAD_MAX) s->ft_symbol_spread = SPREAD_MAX;

    {
        extern int dsp_has_signal_lock;
        const double target = 2.0;
        const double k_spring = dsp_has_signal_lock ? 5.0e-4 : 5.0e-3;
        s->ft_symbol_spread += (target - s->ft_symbol_spread) * k_spring;
    }

    {
        extern int dsp_has_signal_lock;
        if (dsp_has_signal_lock) {
            s->ft_fine_freq += err * K_FINE_FREQ;
            if (s->ft_fine_freq >  0.5) s->ft_fine_freq =  0.5;
            if (s->ft_fine_freq < -0.5) s->ft_fine_freq = -0.5;
        }
    }

    s->ft_coarse_freq += (s->ft_fine_freq - s->ft_coarse_freq) * K_COARSE_FREQ;

    *sym_out = (float)output;

    {
        static int diag_count = 0;
        extern int fsk4_diag_reset_flag;
        extern int dsp_has_signal_lock;
        extern void sys_log(unsigned char color, const char *fmt, ...);
        if (fsk4_diag_reset_flag) {
            diag_count = 0;
            fsk4_diag_reset_flag = 0;
            sys_log(2, "FSK4 entered. ft_symbol_time=%.3f spread_init=2.0",
                    (float)s->ft_symbol_time);
        }
        diag_count++;
        int emit_threshold = dsp_has_signal_lock ? 2400 : 24000;
        if (diag_count >= emit_threshold) {
            diag_count = 0;
            sys_log(2, "FSK4 %s in=%+.2f out=%+.2f sprd=%.3f freq=%+.3f nco=%+.4f",
                    dsp_has_signal_lock ? "SIG" : "NOI",
                    (float)interp, (float)output,
                    (float)s->ft_symbol_spread, (float)s->ft_fine_freq,
                    (float)s->nco_dc_avg);
        }
    }

    return 1;
}

int fsk4_diag_reset_flag = 0;

int dsp_has_signal_lock = 0;

const float nco_sin_lut[256] = {
    +0.000000f, +0.024541f, +0.049068f, +0.073565f, +0.098017f, +0.122411f, +0.146730f, +0.170962f,
    +0.195090f, +0.219101f, +0.242980f, +0.266713f, +0.290285f, +0.313682f, +0.336890f, +0.359895f,
    +0.382683f, +0.405241f, +0.427555f, +0.449611f, +0.471397f, +0.492898f, +0.514103f, +0.534998f,
    +0.555570f, +0.575808f, +0.595699f, +0.615232f, +0.634393f, +0.653173f, +0.671559f, +0.689541f,
    +0.707107f, +0.724247f, +0.740951f, +0.757209f, +0.773010f, +0.788346f, +0.803208f, +0.817585f,
    +0.831470f, +0.844854f, +0.857729f, +0.870087f, +0.881921f, +0.893224f, +0.903989f, +0.914210f,
    +0.923880f, +0.932993f, +0.941544f, +0.949528f, +0.956940f, +0.963776f, +0.970031f, +0.975702f,
    +0.980785f, +0.985278f, +0.989177f, +0.992480f, +0.995185f, +0.997290f, +0.998795f, +0.999699f,
    +1.000000f, +0.999699f, +0.998795f, +0.997290f, +0.995185f, +0.992480f, +0.989177f, +0.985278f,
    +0.980785f, +0.975702f, +0.970031f, +0.963776f, +0.956940f, +0.949528f, +0.941544f, +0.932993f,
    +0.923880f, +0.914210f, +0.903989f, +0.893224f, +0.881921f, +0.870087f, +0.857729f, +0.844854f,
    +0.831470f, +0.817585f, +0.803208f, +0.788346f, +0.773010f, +0.757209f, +0.740951f, +0.724247f,
    +0.707107f, +0.689541f, +0.671559f, +0.653173f, +0.634393f, +0.615232f, +0.595699f, +0.575808f,
    +0.555570f, +0.534998f, +0.514103f, +0.492898f, +0.471397f, +0.449611f, +0.427555f, +0.405241f,
    +0.382683f, +0.359895f, +0.336890f, +0.313682f, +0.290285f, +0.266713f, +0.242980f, +0.219101f,
    +0.195090f, +0.170962f, +0.146730f, +0.122411f, +0.098017f, +0.073565f, +0.049068f, +0.024541f,
    +0.000000f, -0.024541f, -0.049068f, -0.073565f, -0.098017f, -0.122411f, -0.146730f, -0.170962f,
    -0.195090f, -0.219101f, -0.242980f, -0.266713f, -0.290285f, -0.313682f, -0.336890f, -0.359895f,
    -0.382683f, -0.405241f, -0.427555f, -0.449611f, -0.471397f, -0.492898f, -0.514103f, -0.534998f,
    -0.555570f, -0.575808f, -0.595699f, -0.615232f, -0.634393f, -0.653173f, -0.671559f, -0.689541f,
    -0.707107f, -0.724247f, -0.740951f, -0.757209f, -0.773010f, -0.788346f, -0.803208f, -0.817585f,
    -0.831470f, -0.844854f, -0.857729f, -0.870087f, -0.881921f, -0.893224f, -0.903989f, -0.914210f,
    -0.923880f, -0.932993f, -0.941544f, -0.949528f, -0.956940f, -0.963776f, -0.970031f, -0.975702f,
    -0.980785f, -0.985278f, -0.989177f, -0.992480f, -0.995185f, -0.997290f, -0.998795f, -0.999699f,
    -1.000000f, -0.999699f, -0.998795f, -0.997290f, -0.995185f, -0.992480f, -0.989177f, -0.985278f,
    -0.980785f, -0.975702f, -0.970031f, -0.963776f, -0.956940f, -0.949528f, -0.941544f, -0.932993f,
    -0.923880f, -0.914210f, -0.903989f, -0.893224f, -0.881921f, -0.870087f, -0.857729f, -0.844854f,
    -0.831470f, -0.817585f, -0.803208f, -0.788346f, -0.773010f, -0.757209f, -0.740951f, -0.724247f,
    -0.707107f, -0.689541f, -0.671559f, -0.653173f, -0.634393f, -0.615232f, -0.595699f, -0.575808f,
    -0.555570f, -0.534998f, -0.514103f, -0.492898f, -0.471397f, -0.449611f, -0.427555f, -0.405241f,
    -0.382683f, -0.359895f, -0.336890f, -0.313682f, -0.290285f, -0.266713f, -0.242980f, -0.219101f,
    -0.195090f, -0.170962f, -0.146730f, -0.122411f, -0.098017f, -0.073565f, -0.049068f, -0.024541f,
};

int dsp_process_iq(dsp_state_t *s, const uint8_t *iq_data, int iq_len,
                   int16_t *audio_out, int audio_max)
{
    int n_out = 0, n_iq = iq_len / 2;
    for (int k = 0; k < n_iq; k++) {
        float fi = ((float)iq_data[k * 2] - 127.5f) / 127.5f;
        float fq = ((float)iq_data[k * 2 + 1] - 127.5f) / 127.5f;

        s->cic_acc_i += fi;
        s->cic_acc_q += fq;
        s->cic_count++;
        if (s->cic_count < DSP_PRE_DECIM) continue;

        float ai = s->cic_acc_i * (1.0f / DSP_PRE_DECIM);
        float aq = s->cic_acc_q * (1.0f / DSP_PRE_DECIM);
        s->cic_acc_i = 0;
        s->cic_acc_q = 0;
        s->cic_count = 0;

        {
            static const int NCO_LUT_SIZE = 256;
            extern const float nco_sin_lut[];   
            double p = s->nco_phase;
            if (p < 0) p += 6.283185307;
            int idx = (int)(p * (NCO_LUT_SIZE / 6.283185307));
            idx &= (NCO_LUT_SIZE - 1);
            int idx_cos = (idx + NCO_LUT_SIZE / 4) & (NCO_LUT_SIZE - 1);
            float nc = nco_sin_lut[idx_cos];  
            float ns = nco_sin_lut[idx];
            float ai2 = ai * nc + aq * ns;
            float aq2 = aq * nc - ai * ns;
            ai = ai2; aq = aq2;
            s->nco_phase += s->nco_step_rad;
            if (s->nco_phase >  6.283185307) s->nco_phase -= 6.283185307;
            if (s->nco_phase < -6.283185307) s->nco_phase += 6.283185307;
        }

        float si, sq;
        if (!lpf_decimate(s, ai, aq, &si, &sq)) continue;

        if (s->dc_alpha > 0.0f) {
            s->dc_avg_i += s->dc_alpha * (si - s->dc_avg_i);
            s->dc_avg_q += s->dc_alpha * (sq - s->dc_avg_q);
            si -= s->dc_avg_i;
            sq -= s->dc_avg_q;
        }

        agc_apply(s, &si, &sq);

        if (s->mode == DEMOD_C4FM) {
            if (n_out < audio_max) {
                int16_t raw = fm_demod(s, si, sq);
                float filt = rrc_filter(s, (float)raw);

                {
                    extern int dsp_has_signal_lock;
                    float c4fm_alpha = dsp_has_signal_lock ? 0.00005f : 0.0005f;
                    s->c4fm_dc_avg += c4fm_alpha * (filt - s->c4fm_dc_avg);
                }
                filt -= s->c4fm_dc_avg;

                if (filt >  32767.0f) filt =  32767.0f;
                if (filt < -32767.0f) filt = -32767.0f;
                audio_out[n_out++] = (int16_t)filt;
            }
        } else if (s->mode == DEMOD_DIFF_4FSK) {
            if (n_out < audio_max) {
                audio_out[n_out++] = diff_4fsk_sample(s, si, sq);
            }
        } else if (s->mode == DEMOD_FSK4_TRACKING) {
            float phase_raw = fm_discriminate_linear(s, si, sq);

            {
                extern int dsp_has_signal_lock;
                double nco_alpha = dsp_has_signal_lock ? 0.00005 : 0.0005;
                s->nco_dc_avg += nco_alpha * ((double)phase_raw - s->nco_dc_avg);
            }
            double target_step = s->nco_dc_avg / (double)DSP_DECIMATION;
            s->nco_step_rad += 0.02 * (target_step - s->nco_step_rad);

            float phase = rrc_fsk4_filter(s, phase_raw);

            float tracker_in = phase * s->fsk4_input_scale;
            float sym;
            if (fsk4_track_sample(s, tracker_in, &sym)) {
                float polarity = (s->demod_gain < 0) ? -1.0f : 1.0f;
                float v = sym * 4000.0f * polarity;
                if (v >  32767.0f) v =  32767.0f;
                if (v < -32767.0f) v = -32767.0f;
                int16_t s16 = (int16_t)v;
                for (int r = 0; r < DSP_SPS && n_out < audio_max; r++) {
                    audio_out[n_out++] = s16;
                }
            }
        } else {
            int n = cqpsk_sample(s, si, sq, &audio_out[n_out], audio_max - n_out);
            n_out += n;
        }
    }
    return n_out;
}


float dsp_fsk4_get_fine_freq_hz(const dsp_state_t *s)
{
    return (float)(s->ft_fine_freq * (double)DSP_AUDIO_RATE / (2.0 * 3.14159265358979323846));
}

void dsp_fsk4_clear_fine_freq(dsp_state_t *s)
{
    s->ft_fine_freq   = 0.0;
    s->ft_coarse_freq = 0.0;
}

void dsp_fsk4_reset_tracker(dsp_state_t *s)
{
    s->ft_fine_freq     = 0.0;
    s->ft_coarse_freq   = 0.0;
    s->ft_symbol_spread = 2.0;
    s->ft_symbol_clock  = 0.0;
    for (int k = 0; k < 8; k++) s->ft_history[k] = 0.0f;
    s->ft_history_idx   = 0;
}
