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
#include "esp_timer.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/*
 * Anti-alias LPF for 240k → 24k decimation (÷10).
 * 31-tap Hamming, fc=9 kHz, unity DC gain.
 *
 * Session 9b: shortened from 61 taps back to 31. The longer filter with
 * fc=6 kHz had 125 µs group delay — 60% of a P25 symbol (208 µs) — which
 * smeared every transition across the next symbol's window. At 5 samples/
 * symbol this meant NO plateaus to median-filter, only ramps.
 *
 * 31-tap fc=9 kHz has 62.5 µs group delay = 30% of symbol. Transitions
 * take ~1.5 samples, plateaus take 3-4 samples — median-friendly.
 *
 * Passband: flat to 5 kHz (-1.5 dB). Covers full P25 C4FM bandwidth.
 * Stopband: -9 dB @ Nyquist (12 kHz), -25 dB @ 18 kHz, -53 dB @ 24 kHz.
 * The -9 dB at Nyquist is weaker than session 9's -25 dB but the resulting
 * aliasing is energy outside 12 kHz folding back — and there's no P25
 * signal out there at baseband since the carrier is centered at DC, so the
 * fold-back is just background noise, not co-channel interference.
 *
 * CPU: 31 taps × 24 kHz = 0.74 Mops/sec (actually CHEAPER than session 9's
 * 61 × 24k = 1.46 Mops/sec, and still much cheaper than session 6's
 * 31 × 48k = 1.49 Mops/sec).
 */
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

    s->mode = DEMOD_C4FM;          /* Default: FM discriminator */
    s->demod_gain = 9000.0f;      /* Positive = correct polarity for FSK4_TRACKING.
                                   * (If switching to DEMOD_C4FM mode, press R to
                                   * flip sign since C4FM needs opposite polarity.) */

    /* CQPSK Gardner timing recovery */
    s->g_period    = (float)DSP_SPS;
    s->g_mu        = 0.5f;
    s->g_gain_mu   = 0.025f;
    s->g_gain_omega = 0.025f * 0.025f * 0.25f;
    s->g_omega_rel = 0.005f;
    s->g_half = DSP_SPS / 2;

    /* CQPSK Costas loop */
    s->c_alpha = 0.04f;
    s->c_beta  = 0.04f * 0.04f * 0.25f;

    s->diff_prev_i = 1.0f;
    s->diff_prev_q = 0.0f;
    s->cqpsk_polarity = 0;

    /* DC blocker: enabled by default. Set to 0 to disable. Applies to all
     * modes that consume post-LPF IQ before demodulation. */
    s->dc_avg_i = 0.0f;
    s->dc_avg_q = 0.0f;
    s->dc_alpha = 0.001f;   /* ~1 second TC at 24 kHz */

    /* RMS AGC — feedforward, OP25-style.
     *
     * Previous default was alpha=0 (frozen gain=1.0), which meant weak
     * signals went into atan2 with their full original amplitude. When
     * |IQ| is small, the phase increment per sample is dominated by IQ
     * quantization + LO leakage rather than actual modulation, so the
     * discriminator output is mostly noise and the sync correlator
     * never sees a clean dibit pattern. That matched the README's
     * "sync isn't reliably hitting on real P25 signals at low input
     * levels" symptom exactly.
     *
     * alpha = 0.01 at 24 kHz post-LPF rate → time constant ~4 ms ≈
     * 20 P25 symbols. Fast enough that the AGC has converged within
     * the ~3-symbol head of an HDU before sync correlation matters.
     * Slow enough that per-symbol envelope variation (which carries
     * no information in C4FM/FM mode) doesn't get cancelled.
     *
     * Reference 0.85 puts post-AGC |IQ| close to but inside the
     * float [-1, 1] range — gives the discriminator a strong vector
     * to compute phase from, without risking saturation in the
     * subsequent RRC / discriminator math.
     *
     * Disable by zeroing alpha at runtime if needed. */
    s->agc_gain  = 1.0f;
    s->agc_alpha = 0.01f;
    s->agc_ref   = 0.85f;
    s->agc_mag_in_avg  = 0.0f;
    s->agc_mag_out_avg = 0.0f;
    s->agc_mag_in_peak = 0.0f;

    /* DIFF_4FSK equalizer (SDRTrunk defaults) */
    s->eq_pll  = 0.0f;
    s->eq_gain = 1.219f;     /* SDRTrunk's empirical gain for pulse-shaped C4FM */

    /*
     * Output scale for DIFF_4FSK mode:
     * After equalizer, ideal ±3 symbols are at ±3π/4 * eq_gain ≈ ±2.87 rad
     * and ±1 symbols are at ±π/4 * eq_gain ≈ ±0.957 rad.
     * We want these to map to DSD's expected range where the slicer works.
     * Scale = 10186 maps 3π/4 → 24000 (strong outer), π/4 → 8000 (inner).
     * DSD's 50% boundary at center=0 with umid=12000 will cleanly separate all 4 levels.
     */
    s->diff_output_scale = 10186.0f;

    /* Differential demod ring buffer */
    s->diff_ring_idx = 0;
    s->diff_ring_filled = 0;

    /* FSK4_TRACKING: initialize 3-loop tracker state.
     *
     * Session 9: now runs at 24 kHz input rate, 5 samples per symbol.
     * ft_symbol_time = 4800/24000 = 0.2, so ft_symbol_clock advances 0.2
     * per sample and crosses 1.0 every 5 samples — emitting one symbol
     * per 5 input samples. MMSE interpolator table is indexed by mu, so
     * the sub-sample precision still works the same.
     *
     * Nominal spread = 2.0 means ideal symbols sit at ±1.0 (inner) and
     * ±3.0 (outer) after the `output = 2.0 * interp / spread` scaling.
     *
     * Session 10: raised from 3.0 → 7.0 based on physics.
     *
     * P25 C4FM deviation is ±600 Hz (inner) and ±1800 Hz (outer). At
     * 24 kHz sample rate, post-discriminator phase-per-sample is
     * 2π*f/fs, so ±1 symbol → ±0.157 rad, ±3 symbol → ±0.471 rad.
     *
     * For outer symbols to present to the tracker at the expected ±3
     * (so the spread loop can converge to 2.0 instead of collapsing to
     * SPREAD_MIN), we need scale ≈ 3 / 0.471 ≈ 6.4. Plus the RRC has
     * a peak-retention factor of ~0.82 on alternating symbols (verified
     * by tap simulation), so effective scale needed ≈ 6.4 / 0.82 ≈ 7.8.
     *
     * 7.0 rounded down, user can push to 8+ with E key if needed.
     * Previous value 3.0 was ~2.3x undergained — the root cause of the
     * chronic spread collapse and inner=±0.5 / outer=±1.5 cramping we
     * saw in all session-9 logs.
     */
    s->fsk4_input_scale = 7.0f;   /* session 10: was 3.0, physics-correct */
    s->ft_symbol_clock  = 0.0;
    s->ft_symbol_time   = (double)DSP_BAUD / (double)DSP_AUDIO_RATE;  /* 0.2 */
    s->ft_symbol_spread = 2.0;
    s->ft_fine_freq     = 0.0;
    s->ft_coarse_freq   = 0.0;
    for (int k = 0; k < 8; k++) s->ft_history[k] = 0.0f;
    s->ft_history_idx   = 0;
    s->ft_prev_i        = 1.0f;
    s->ft_prev_q        = 0.0f;

    /* Pre-demod NCO — zero-init. DC-tracking AFC starts from "no shift". */
    s->nco_phase    = 0.0;
    s->nco_step_rad = 0.0;
    s->nco_dc_avg   = 0.0;

    s->c4fm_dc_avg  = 0.0f;
}

void dsp_set_mode(dsp_state_t *s, demod_mode_t mode)
{
    s->mode = mode;
    /* The 21-tap RRC matched filter (rrc_fsk4_buf) is shared between
     * DEMOD_C4FM and DEMOD_FSK4_TRACKING. Reset it on entry to either
     * so stale history from the previous mode (or last call) doesn't
     * leak into the first ~21 output samples of this mode. */
    if (mode == DEMOD_C4FM || mode == DEMOD_FSK4_TRACKING) {
        for (int k = 0; k < RRC_FSK4_TAPS; k++) s->rrc_fsk4_buf[k] = 0.0f;
        s->rrc_fsk4_idx = 0;
    }
    /* On entry to FSK4_TRACKING, reset tracker state so the 3 loops start
     * clean and the diagnostic logger fires fresh. Also reset the static
     * diag counter inside the tracker. */
    if (mode == DEMOD_FSK4_TRACKING) {
        s->ft_symbol_clock  = 0.0;
        s->ft_symbol_spread = 2.0;
        s->ft_fine_freq     = 0.0;
        s->ft_coarse_freq   = 0.0;
        for (int k = 0; k < 8; k++) s->ft_history[k] = 0.0f;
        s->ft_history_idx   = 0;
        s->ft_prev_i        = 1.0f;
        s->ft_prev_q        = 0.0f;
        /* Reset pre-demod NCO on entry */
        s->nco_phase    = 0.0;
        s->nco_step_rad = 0.0;
        s->nco_dc_avg   = 0.0;
        /* Reset diagnostic counter via flag — see diag block in fsk4_track_sample */
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

/* ── LPF + decimate ── */
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

/* ── RMS AGC (OP25-style feedforward) ── */
static void agc_apply(dsp_state_t *s, float *si, float *sq)
{
    float mag2 = (*si) * (*si) + (*sq) * (*sq);
    float mag = sqrtf(mag2 + 1e-12f);

    /* Instrumentation: track input magnitude before gain.
     * α=0.0005 → ~2 s TC at 24 kHz, gives a stable read at 1 Hz emit. */
    s->agc_mag_in_avg += 0.0005f * (mag - s->agc_mag_in_avg);
    if (mag > s->agc_mag_in_peak) s->agc_mag_in_peak = mag;

    float target_gain = (mag > 1e-6f) ? (s->agc_ref / mag) : s->agc_gain;
    s->agc_gain += s->agc_alpha * (target_gain - s->agc_gain);
    if (s->agc_gain > 100.0f) s->agc_gain = 100.0f;
    if (s->agc_gain < 0.001f) s->agc_gain = 0.001f;
    *si *= s->agc_gain;
    *sq *= s->agc_gain;

    /* Instrumentation: track output magnitude after gain.
     * Should converge near s->agc_ref (0.85) if AGC is working. */
    float out_mag = sqrtf((*si) * (*si) + (*sq) * (*sq) + 1e-12f);
    s->agc_mag_out_avg += 0.0005f * (out_mag - s->agc_mag_out_avg);
}

/* ── C4FM RRC symbol filter (matches OP25 symbol_filter) ── */
/* RRC, excess_bw=0.2, 10 sps, 51 taps — for 48kHz/4800 baud symbol shaping */
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

/* ── FSK4 matched filter (21-tap RRC at 5 samples/symbol) ──
 *
 * Operates on post-discriminator phase values, scalar input. Designed
 * specifically for 24 kHz / 4800 baud = 5 sps. Beta=0.2 matches
 * the OP25 C4FM filter design but at our native rate (the existing
 * rrc_sym_taps are designed for 10 sps, which doesn't match our
 * post-CIC/LPF rate of 24 kHz).
 *
 * Unity DC gain, so a constant-DC input comes out unchanged — the
 * NCO's offset correction still sees its own DC measurement correctly.
 *
 * Generated in Python with the standard RRC impulse response formula,
 * normalized to sum(taps) = 1.
 */
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

/* ── RRC filter for I/Q channels (DIFF_4FSK mode) ── */
/* Same taps as above, but separate buffers for I and Q */
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

/* ── C4FM FM discriminator ──
 *
 * Session 7: atan2-based phase-difference discriminator (replaces the old
 * cross-product / |s|² formula). Bounded to [-π, +π] by construction, so
 * it can never produce spikes larger than |demod_gain|·π.
 *
 * Old formula: (prev_q*si - prev_i*sq) / |s|² ≈ -dφ/sample when |s| is
 * reasonable, but blows up to ±28000+ in envelope nulls because dividing
 * phase noise by a near-zero |s|² amplifies it into π-rad range.
 *
 * New formula: atan2(-Im{conj(prev)*cur}, Re{conj(prev)*cur}) — exact per-
 * sample phase step in radians, negated to preserve old sign convention so
 * demod_gain=-9000 still gives correct output polarity.
 */
static int16_t fm_demod(dsp_state_t *s, float si, float sq)
{
    /* conj(prev) * cur = (prev_i - j*prev_q) * (si + j*sq)
     *                  = (prev_i*si + prev_q*sq) + j*(prev_i*sq - prev_q*si) */
    float re = s->prev_i * si + s->prev_q * sq;
    float im = s->prev_i * sq - s->prev_q * si;
    /* Negate im to match old formula's sign convention: old = -Im/|s|². */
    float phase = atan2f(-im, re);
    s->prev_i = si; s->prev_q = sq;

    float v = phase * s->demod_gain;
    if (v >  32767.0f) v =  32767.0f;
    if (v < -32767.0f) v = -32767.0f;
    return (int16_t)v;
}

/* ── CQPSK: Costas + Gardner + diff + atan2 rescale ── */
static int cqpsk_sample(dsp_state_t *s, float si, float sq,
                         int16_t *out, int maxn)
{
    /* Costas loop */
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

    /* Gardner timing */
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

    /* Differential phasor */
    float di2 = mi * s->diff_prev_i + mq * s->diff_prev_q;
    float dq2 = mq * s->diff_prev_i - mi * s->diff_prev_q;
    s->diff_prev_i = mi; s->diff_prev_q = mq;

    /* atan2 -> rescale by 1/(pi/4) */
    float angle = atan2f(dq2, di2);
    angle += s->cqpsk_polarity * (M_PI / 4.0f);
    while (angle > M_PI) angle -= 2.0f * M_PI;
    while (angle < -M_PI) angle += 2.0f * M_PI;
    float rescaled = angle / (M_PI / 4.0f);

    /* Scale to DSD range */
    float v = rescaled * s->demod_gain;
    if (v > 32767.0f) v = 32767.0f;
    if (v < -32767.0f) v = -32767.0f;
    int16_t sym = (int16_t)v;

    /* Output repeated for DSD's 10 sps */
    int n = 0;
    for (int r = 0; r < DSP_SPS && n < maxn; r++)
        out[n++] = sym;
    return n;
}

/*
 * ══════════════════════════════════════════════════════════════════════
 *  DEMOD_DIFF_4FSK — SDRTrunk-inspired differential demodulation
 * ══════════════════════════════════════════════════════════════════════
 *
 * Architecture (matches SDRTrunk's P25P1DecoderC4FM):
 *
 *   IQ samples at 48 kHz (after LPF+decim)
 *     → RRC pulse shaping filter on I and Q separately
 *     → Differential demodulation:
 *         phase[n] = atan2( Q[n]*I[n-D] - I[n]*Q[n-D],
 *                           I[n]*I[n-D] + Q[n]*Q[n-D] )
 *       where D = samples_per_symbol (10 at 48 kHz)
 *     → Equalizer: output = (phase + eq_pll) * eq_gain
 *     → Scale to int16 for DSD consumption
 *
 * Why this works better than FM discriminator:
 *   - FM demod computes instantaneous frequency (phase derivative between
 *     ADJACENT samples), requiring very clean signals. Noise in any single
 *     sample corrupts the output.
 *   - Differential demod compares samples spaced 1 SYMBOL apart, effectively
 *     integrating over the full symbol period. This is inherently more noise-
 *     tolerant and produces the actual differential phase which maps directly
 *     to the transmitted dibit.
 *   - The output is in radians with natural quadrant boundaries at ±π/2,
 *     eliminating the need for DSD's adaptive min/max/center tracking.
 */
static int16_t diff_4fsk_sample(dsp_state_t *s, float si, float sq)
{
    /*
     * Try two variants:
     * - With RRC: better pulse shaping but might over-filter at low sample rates
     * - Without RRC: raw differential demod, let DSD handle the symbol averaging
     *
     * For now, skip RRC to get basic signal through, enable later.
     */
    float fi = si, fq = sq;
    /* RRC disabled for initial testing — uncomment to enable:
     * rrc_filter_iq(s, si, sq, &fi, &fq);
     */

    /* Store current sample in ring, get sample from 1 symbol ago */
    int d_idx = s->diff_ring_idx;
    float prev_i = s->diff_ring_i[d_idx];
    float prev_q = s->diff_ring_q[d_idx];
    s->diff_ring_i[d_idx] = fi;
    s->diff_ring_q[d_idx] = fq;
    s->diff_ring_idx = (d_idx + 1) % DIFF_DELAY;

    /* Don't output until ring has valid data */
    if (!s->diff_ring_filled) {
        if (s->diff_ring_idx == 0)
            s->diff_ring_filled = 1;
        return 0;
    }

    /* Differential demodulation: cur * conj(prev) */
    float diff_i = fi * prev_i + fq * prev_q;
    float diff_q = fq * prev_i - fi * prev_q;

    float phase = atan2f(diff_q, diff_i);

    /* Equalizer */
    float equalized = (phase + s->eq_pll) * s->eq_gain;

    /* Wrap to ±π */
    if (equalized > M_PI)  equalized -= 2.0f * M_PI;
    if (equalized < -M_PI) equalized += 2.0f * M_PI;

    /* Scale to int16 for DSD */
    float v = equalized * s->diff_output_scale;
    if (v > 32767.0f) v = 32767.0f;
    if (v < -32767.0f) v = -32767.0f;

    /* Diagnostic: log first 50 output values when enabled */
    {
        static int diag_count = 0;
        static int diag_enabled = 0;
        extern void sys_log(unsigned char color, const char *fmt, ...);
        /* Reset counter on mode switch (detected by checking if we just started) */
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

/*
 * ══════════════════════════════════════════════════════════════════════
 *  DEMOD_FSK4_TRACKING — OP25 Frank/Radio-Rausch 3-loop tracker
 * ══════════════════════════════════════════════════════════════════════
 *
 * Port of op25/gr-op25/lib/fsk4_demod_ff_impl.cc (GPL-3, Frank/Radio Rausch
 * 2006, Steve Glass 2011). Algorithm from U.S. Patent 5,553,101 (1996).
 *
 * Architecture:
 *
 *   IQ at 48 kHz -> linear atan2 FM discriminator -> phase sample
 *        |
 *        v
 *   ft_history[8] sample ring
 *        |
 *        v
 *   ft_symbol_clock advances by ft_symbol_time (= 0.1) per input sample
 *        |
 *        v  when clock crosses 1.0:
 *   MMSE fractional-sample interpolator at mu = ft_symbol_clock / ft_symbol_time
 *        |
 *        v
 *   subtract ft_fine_freq (DC offset correction)
 *        |
 *        v
 *   scale: output = 2.0 * interp / ft_symbol_spread
 *        |                                       (nominal output at ±1, ±3)
 *        v
 *   hard symbol decision + error computation vs expected ±0.5*spread or ±1.5*spread
 *        |
 *        v
 *   update 3 tracking loops:
 *     - ft_symbol_spread += err * K_SPREAD  (with direction per symbol region)
 *     - ft_symbol_clock  ± err * K_TIMING   (sign from interp_p1 vs interp slope)
 *     - ft_fine_freq     += err * K_FINE_FREQ
 *
 * Table: 128 fractional sample positions × 8 taps each. Index 0 means
 * "sample exactly at history[4]"; index 128 means "sample exactly at
 * history[3]". Used to interpolate the correct symbol sample given the
 * fractional clock position. Copied verbatim from the OP25 source.
 */

#define FSK4_NSTEPS  128
#define FSK4_NTAPS   8
#define K_SPREAD      0.0100
#define K_TIMING      0.025
#define K_FINE_FREQ   0.125
#define K_COARSE_FREQ 0.00125
#define SPREAD_MIN    1.6
#define SPREAD_MAX    3.0   /* session 10: was 2.4, widened because real
                             * signal on RTL V4/R820T2 shows asymmetric
                             * constellation (neg ratio 2.6, pos 3.5,
                             * avg 3.0) that drives spread above 2.4.
                             * Letting the loop find its own equilibrium
                             * around 2.3-2.5 is healthier than pegging
                             * at an arbitrary ceiling. */
#define SPREAD_NOM    2.0

/* MMSE interpolator taps: [FSK4_NSTEPS+1][FSK4_NTAPS]. Values come from
 * OP25 (GPL-3), generated by their gen_interpolator_taps script. */
static const float FSK4_TAPS[FSK4_NSTEPS + 1][FSK4_NTAPS] = {
 { 0.00000e+00f, 0.00000e+00f, 0.00000e+00f, 0.00000e+00f, 1.00000e+00f, 0.00000e+00f, 0.00000e+00f, 0.00000e+00f }, /*   0 */
 {-1.54700e-04f, 8.53777e-04f,-2.76968e-03f, 7.89295e-03f, 9.98534e-01f,-5.41054e-03f, 1.24642e-03f,-1.98993e-04f }, /*   1 */
 {-3.09412e-04f, 1.70888e-03f,-5.55134e-03f, 1.58840e-02f, 9.96891e-01f,-1.07209e-02f, 2.47942e-03f,-3.96391e-04f }, /*   2 */
 {-4.64053e-04f, 2.56486e-03f,-8.34364e-03f, 2.39714e-02f, 9.95074e-01f,-1.59305e-02f, 3.69852e-03f,-5.92100e-04f }, /*   3 */
 {-6.18544e-04f, 3.42130e-03f,-1.11453e-02f, 3.21531e-02f, 9.93082e-01f,-2.10389e-02f, 4.90322e-03f,-7.86031e-04f }, /*   4 */
 {-7.72802e-04f, 4.27773e-03f,-1.39548e-02f, 4.04274e-02f, 9.90917e-01f,-2.60456e-02f, 6.09305e-03f,-9.78093e-04f }, /*   5 */
 {-9.26747e-04f, 5.13372e-03f,-1.67710e-02f, 4.87921e-02f, 9.88580e-01f,-3.09503e-02f, 7.26755e-03f,-1.16820e-03f }, /*   6 */
 {-1.08030e-03f, 5.98883e-03f,-1.95925e-02f, 5.72454e-02f, 9.86071e-01f,-3.57525e-02f, 8.42626e-03f,-1.35627e-03f }, /*   7 */
 {-1.23337e-03f, 6.84261e-03f,-2.24178e-02f, 6.57852e-02f, 9.83392e-01f,-4.04519e-02f, 9.56876e-03f,-1.54221e-03f }, /*   8 */
 {-1.38589e-03f, 7.69462e-03f,-2.52457e-02f, 7.44095e-02f, 9.80543e-01f,-4.50483e-02f, 1.06946e-02f,-1.72594e-03f }, /*   9 */
 {-1.53777e-03f, 8.54441e-03f,-2.80746e-02f, 8.31162e-02f, 9.77526e-01f,-4.95412e-02f, 1.18034e-02f,-1.90738e-03f }, /*  10 */
 {-1.68894e-03f, 9.39154e-03f,-3.09033e-02f, 9.19033e-02f, 9.74342e-01f,-5.39305e-02f, 1.28947e-02f,-2.08645e-03f }, /*  11 */
 {-1.83931e-03f, 1.02356e-02f,-3.37303e-02f, 1.00769e-01f, 9.70992e-01f,-5.82159e-02f, 1.39681e-02f,-2.26307e-03f }, /*  12 */
 {-1.98880e-03f, 1.10760e-02f,-3.65541e-02f, 1.09710e-01f, 9.67477e-01f,-6.23972e-02f, 1.50233e-02f,-2.43718e-03f }, /*  13 */
 {-2.13733e-03f, 1.19125e-02f,-3.93735e-02f, 1.18725e-01f, 9.63798e-01f,-6.64743e-02f, 1.60599e-02f,-2.60868e-03f }, /*  14 */
 {-2.28483e-03f, 1.27445e-02f,-4.21869e-02f, 1.27812e-01f, 9.59958e-01f,-7.04471e-02f, 1.70776e-02f,-2.77751e-03f }, /*  15 */
 {-2.43121e-03f, 1.35716e-02f,-4.49929e-02f, 1.36968e-01f, 9.55956e-01f,-7.43154e-02f, 1.80759e-02f,-2.94361e-03f }, /*  16 */
 {-2.57640e-03f, 1.43934e-02f,-4.77900e-02f, 1.46192e-01f, 9.51795e-01f,-7.80792e-02f, 1.90545e-02f,-3.10689e-03f }, /*  17 */
 {-2.72032e-03f, 1.52095e-02f,-5.05770e-02f, 1.55480e-01f, 9.47477e-01f,-8.17385e-02f, 2.00132e-02f,-3.26730e-03f }, /*  18 */
 {-2.86289e-03f, 1.60193e-02f,-5.33522e-02f, 1.64831e-01f, 9.43001e-01f,-8.52933e-02f, 2.09516e-02f,-3.42477e-03f }, /*  19 */
 {-3.00403e-03f, 1.68225e-02f,-5.61142e-02f, 1.74242e-01f, 9.38371e-01f,-8.87435e-02f, 2.18695e-02f,-3.57923e-03f }, /*  20 */
 {-3.14367e-03f, 1.76185e-02f,-5.88617e-02f, 1.83711e-01f, 9.33586e-01f,-9.20893e-02f, 2.27664e-02f,-3.73062e-03f }, /*  21 */
 {-3.28174e-03f, 1.84071e-02f,-6.15931e-02f, 1.93236e-01f, 9.28650e-01f,-9.53307e-02f, 2.36423e-02f,-3.87888e-03f }, /*  22 */
 {-3.41815e-03f, 1.91877e-02f,-6.43069e-02f, 2.02814e-01f, 9.23564e-01f,-9.84679e-02f, 2.44967e-02f,-4.02397e-03f }, /*  23 */
 {-3.55283e-03f, 1.99599e-02f,-6.70018e-02f, 2.12443e-01f, 9.18329e-01f,-1.01501e-01f, 2.53295e-02f,-4.16581e-03f }, /*  24 */
 {-3.68570e-03f, 2.07233e-02f,-6.96762e-02f, 2.22120e-01f, 9.12947e-01f,-1.04430e-01f, 2.61404e-02f,-4.30435e-03f }, /*  25 */
 {-3.81671e-03f, 2.14774e-02f,-7.23286e-02f, 2.31843e-01f, 9.07420e-01f,-1.07256e-01f, 2.69293e-02f,-4.43955e-03f }, /*  26 */
 {-3.94576e-03f, 2.22218e-02f,-7.49577e-02f, 2.41609e-01f, 9.01749e-01f,-1.09978e-01f, 2.76957e-02f,-4.57135e-03f }, /*  27 */
 {-4.07279e-03f, 2.29562e-02f,-7.75620e-02f, 2.51417e-01f, 8.95936e-01f,-1.12597e-01f, 2.84397e-02f,-4.69970e-03f }, /*  28 */
 {-4.19774e-03f, 2.36801e-02f,-8.01399e-02f, 2.61263e-01f, 8.89984e-01f,-1.15113e-01f, 2.91609e-02f,-4.82456e-03f }, /*  29 */
 {-4.32052e-03f, 2.43930e-02f,-8.26900e-02f, 2.71144e-01f, 8.83893e-01f,-1.17526e-01f, 2.98593e-02f,-4.94589e-03f }, /*  30 */
 {-4.44107e-03f, 2.50946e-02f,-8.52109e-02f, 2.81060e-01f, 8.77666e-01f,-1.19837e-01f, 3.05345e-02f,-5.06363e-03f }, /*  31 */
 {-4.55932e-03f, 2.57844e-02f,-8.77011e-02f, 2.91006e-01f, 8.71305e-01f,-1.22047e-01f, 3.11866e-02f,-5.17776e-03f }, /*  32 */
 {-4.67520e-03f, 2.64621e-02f,-9.01591e-02f, 3.00980e-01f, 8.64812e-01f,-1.24154e-01f, 3.18153e-02f,-5.28823e-03f }, /*  33 */
 {-4.78866e-03f, 2.71272e-02f,-9.25834e-02f, 3.10980e-01f, 8.58189e-01f,-1.26161e-01f, 3.24205e-02f,-5.39500e-03f }, /*  34 */
 {-4.89961e-03f, 2.77794e-02f,-9.49727e-02f, 3.21004e-01f, 8.51437e-01f,-1.28068e-01f, 3.30021e-02f,-5.49804e-03f }, /*  35 */
 {-5.00800e-03f, 2.84182e-02f,-9.73254e-02f, 3.31048e-01f, 8.44559e-01f,-1.29874e-01f, 3.35600e-02f,-5.59731e-03f }, /*  36 */
 {-5.11376e-03f, 2.90433e-02f,-9.96402e-02f, 3.41109e-01f, 8.37557e-01f,-1.31581e-01f, 3.40940e-02f,-5.69280e-03f }, /*  37 */
 {-5.21683e-03f, 2.96543e-02f,-1.01915e-01f, 3.51186e-01f, 8.30432e-01f,-1.33189e-01f, 3.46042e-02f,-5.78446e-03f }, /*  38 */
 {-5.31716e-03f, 3.02507e-02f,-1.04150e-01f, 3.61276e-01f, 8.23188e-01f,-1.34699e-01f, 3.50903e-02f,-5.87227e-03f }, /*  39 */
 {-5.41467e-03f, 3.08323e-02f,-1.06342e-01f, 3.71376e-01f, 8.15826e-01f,-1.36111e-01f, 3.55525e-02f,-5.95620e-03f }, /*  40 */
 {-5.50931e-03f, 3.13987e-02f,-1.08490e-01f, 3.81484e-01f, 8.08348e-01f,-1.37426e-01f, 3.59905e-02f,-6.03624e-03f }, /*  41 */
 {-5.60103e-03f, 3.19495e-02f,-1.10593e-01f, 3.91596e-01f, 8.00757e-01f,-1.38644e-01f, 3.64044e-02f,-6.11236e-03f }, /*  42 */
 {-5.68976e-03f, 3.24843e-02f,-1.12650e-01f, 4.01710e-01f, 7.93055e-01f,-1.39767e-01f, 3.67941e-02f,-6.18454e-03f }, /*  43 */
 {-5.77544e-03f, 3.30027e-02f,-1.14659e-01f, 4.11823e-01f, 7.85244e-01f,-1.40794e-01f, 3.71596e-02f,-6.25277e-03f }, /*  44 */
 {-5.85804e-03f, 3.35046e-02f,-1.16618e-01f, 4.21934e-01f, 7.77327e-01f,-1.41727e-01f, 3.75010e-02f,-6.31703e-03f }, /*  45 */
 {-5.93749e-03f, 3.39894e-02f,-1.18526e-01f, 4.32038e-01f, 7.69305e-01f,-1.42566e-01f, 3.78182e-02f,-6.37730e-03f }, /*  46 */
 {-6.01374e-03f, 3.44568e-02f,-1.20382e-01f, 4.42134e-01f, 7.61181e-01f,-1.43313e-01f, 3.81111e-02f,-6.43358e-03f }, /*  47 */
 {-6.08674e-03f, 3.49066e-02f,-1.22185e-01f, 4.52218e-01f, 7.52958e-01f,-1.43968e-01f, 3.83800e-02f,-6.48585e-03f }, /*  48 */
 {-6.15644e-03f, 3.53384e-02f,-1.23933e-01f, 4.62289e-01f, 7.44637e-01f,-1.44531e-01f, 3.86247e-02f,-6.53412e-03f }, /*  49 */
 {-6.22280e-03f, 3.57519e-02f,-1.25624e-01f, 4.72342e-01f, 7.36222e-01f,-1.45004e-01f, 3.88454e-02f,-6.57836e-03f }, /*  50 */
 {-6.28577e-03f, 3.61468e-02f,-1.27258e-01f, 4.82377e-01f, 7.27714e-01f,-1.45387e-01f, 3.90420e-02f,-6.61859e-03f }, /*  51 */
 {-6.34530e-03f, 3.65227e-02f,-1.28832e-01f, 4.92389e-01f, 7.19116e-01f,-1.45682e-01f, 3.92147e-02f,-6.65479e-03f }, /*  52 */
 {-6.40135e-03f, 3.68795e-02f,-1.30347e-01f, 5.02377e-01f, 7.10431e-01f,-1.45889e-01f, 3.93636e-02f,-6.68698e-03f }, /*  53 */
 {-6.45388e-03f, 3.72167e-02f,-1.31800e-01f, 5.12337e-01f, 7.01661e-01f,-1.46009e-01f, 3.94886e-02f,-6.71514e-03f }, /*  54 */
 {-6.50285e-03f, 3.75341e-02f,-1.33190e-01f, 5.22267e-01f, 6.92808e-01f,-1.46043e-01f, 3.95900e-02f,-6.73929e-03f }, /*  55 */
 {-6.54823e-03f, 3.78315e-02f,-1.34515e-01f, 5.32164e-01f, 6.83875e-01f,-1.45993e-01f, 3.96678e-02f,-6.75943e-03f }, /*  56 */
 {-6.58996e-03f, 3.81085e-02f,-1.35775e-01f, 5.42025e-01f, 6.74865e-01f,-1.45859e-01f, 3.97222e-02f,-6.77557e-03f }, /*  57 */
 {-6.62802e-03f, 3.83650e-02f,-1.36969e-01f, 5.51849e-01f, 6.65779e-01f,-1.45641e-01f, 3.97532e-02f,-6.78771e-03f }, /*  58 */
 {-6.66238e-03f, 3.86006e-02f,-1.38094e-01f, 5.61631e-01f, 6.56621e-01f,-1.45343e-01f, 3.97610e-02f,-6.79588e-03f }, /*  59 */
 {-6.69300e-03f, 3.88151e-02f,-1.39150e-01f, 5.71370e-01f, 6.47394e-01f,-1.44963e-01f, 3.97458e-02f,-6.80007e-03f }, /*  60 */
 {-6.71985e-03f, 3.90083e-02f,-1.40136e-01f, 5.81063e-01f, 6.38099e-01f,-1.44503e-01f, 3.97077e-02f,-6.80032e-03f }, /*  61 */
 {-6.74291e-03f, 3.91800e-02f,-1.41050e-01f, 5.90706e-01f, 6.28739e-01f,-1.43965e-01f, 3.96469e-02f,-6.79662e-03f }, /*  62 */
 {-6.76214e-03f, 3.93299e-02f,-1.41891e-01f, 6.00298e-01f, 6.19318e-01f,-1.43350e-01f, 3.95635e-02f,-6.78902e-03f }, /*  63 */
 {-6.77751e-03f, 3.94578e-02f,-1.42658e-01f, 6.09836e-01f, 6.09836e-01f,-1.42658e-01f, 3.94578e-02f,-6.77751e-03f }, /*  64 */
 {-6.78902e-03f, 3.95635e-02f,-1.43350e-01f, 6.19318e-01f, 6.00298e-01f,-1.41891e-01f, 3.93299e-02f,-6.76214e-03f }, /*  65 */
 {-6.79662e-03f, 3.96469e-02f,-1.43965e-01f, 6.28739e-01f, 5.90706e-01f,-1.41050e-01f, 3.91800e-02f,-6.74291e-03f }, /*  66 */
 {-6.80032e-03f, 3.97077e-02f,-1.44503e-01f, 6.38099e-01f, 5.81063e-01f,-1.40136e-01f, 3.90083e-02f,-6.71985e-03f }, /*  67 */
 {-6.80007e-03f, 3.97458e-02f,-1.44963e-01f, 6.47394e-01f, 5.71370e-01f,-1.39150e-01f, 3.88151e-02f,-6.69300e-03f }, /*  68 */
 {-6.79588e-03f, 3.97610e-02f,-1.45343e-01f, 6.56621e-01f, 5.61631e-01f,-1.38094e-01f, 3.86006e-02f,-6.66238e-03f }, /*  69 */
 {-6.78771e-03f, 3.97532e-02f,-1.45641e-01f, 6.65779e-01f, 5.51849e-01f,-1.36969e-01f, 3.83650e-02f,-6.62802e-03f }, /*  70 */
 {-6.77557e-03f, 3.97222e-02f,-1.45859e-01f, 6.74865e-01f, 5.42025e-01f,-1.35775e-01f, 3.81085e-02f,-6.58996e-03f }, /*  71 */
 {-6.75943e-03f, 3.96678e-02f,-1.45993e-01f, 6.83875e-01f, 5.32164e-01f,-1.34515e-01f, 3.78315e-02f,-6.54823e-03f }, /*  72 */
 {-6.73929e-03f, 3.95900e-02f,-1.46043e-01f, 6.92808e-01f, 5.22267e-01f,-1.33190e-01f, 3.75341e-02f,-6.50285e-03f }, /*  73 */
 {-6.71514e-03f, 3.94886e-02f,-1.46009e-01f, 7.01661e-01f, 5.12337e-01f,-1.31800e-01f, 3.72167e-02f,-6.45388e-03f }, /*  74 */
 {-6.68698e-03f, 3.93636e-02f,-1.45889e-01f, 7.10431e-01f, 5.02377e-01f,-1.30347e-01f, 3.68795e-02f,-6.40135e-03f }, /*  75 */
 {-6.65479e-03f, 3.92147e-02f,-1.45682e-01f, 7.19116e-01f, 4.92389e-01f,-1.28832e-01f, 3.65227e-02f,-6.34530e-03f }, /*  76 */
 {-6.61859e-03f, 3.90420e-02f,-1.45387e-01f, 7.27714e-01f, 4.82377e-01f,-1.27258e-01f, 3.61468e-02f,-6.28577e-03f }, /*  77 */
 {-6.57836e-03f, 3.88454e-02f,-1.45004e-01f, 7.36222e-01f, 4.72342e-01f,-1.25624e-01f, 3.57519e-02f,-6.22280e-03f }, /*  78 */
 {-6.53412e-03f, 3.86247e-02f,-1.44531e-01f, 7.44637e-01f, 4.62289e-01f,-1.23933e-01f, 3.53384e-02f,-6.15644e-03f }, /*  79 */
 {-6.48585e-03f, 3.83800e-02f,-1.43968e-01f, 7.52958e-01f, 4.52218e-01f,-1.22185e-01f, 3.49066e-02f,-6.08674e-03f }, /*  80 */
 {-6.43358e-03f, 3.81111e-02f,-1.43313e-01f, 7.61181e-01f, 4.42134e-01f,-1.20382e-01f, 3.44568e-02f,-6.01374e-03f }, /*  81 */
 {-6.37730e-03f, 3.78182e-02f,-1.42566e-01f, 7.69305e-01f, 4.32038e-01f,-1.18526e-01f, 3.39894e-02f,-5.93749e-03f }, /*  82 */
 {-6.31703e-03f, 3.75010e-02f,-1.41727e-01f, 7.77327e-01f, 4.21934e-01f,-1.16618e-01f, 3.35046e-02f,-5.85804e-03f }, /*  83 */
 {-6.25277e-03f, 3.71596e-02f,-1.40794e-01f, 7.85244e-01f, 4.11823e-01f,-1.14659e-01f, 3.30027e-02f,-5.77544e-03f }, /*  84 */
 {-6.18454e-03f, 3.67941e-02f,-1.39767e-01f, 7.93055e-01f, 4.01710e-01f,-1.12650e-01f, 3.24843e-02f,-5.68976e-03f }, /*  85 */
 {-6.11236e-03f, 3.64044e-02f,-1.38644e-01f, 8.00757e-01f, 3.91596e-01f,-1.10593e-01f, 3.19495e-02f,-5.60103e-03f }, /*  86 */
 {-6.03624e-03f, 3.59905e-02f,-1.37426e-01f, 8.08348e-01f, 3.81484e-01f,-1.08490e-01f, 3.13987e-02f,-5.50931e-03f }, /*  87 */
 {-5.95620e-03f, 3.55525e-02f,-1.36111e-01f, 8.15826e-01f, 3.71376e-01f,-1.06342e-01f, 3.08323e-02f,-5.41467e-03f }, /*  88 */
 {-5.87227e-03f, 3.50903e-02f,-1.34699e-01f, 8.23188e-01f, 3.61276e-01f,-1.04150e-01f, 3.02507e-02f,-5.31716e-03f }, /*  89 */
 {-5.78446e-03f, 3.46042e-02f,-1.33189e-01f, 8.30432e-01f, 3.51186e-01f,-1.01915e-01f, 2.96543e-02f,-5.21683e-03f }, /*  90 */
 {-5.69280e-03f, 3.40940e-02f,-1.31581e-01f, 8.37557e-01f, 3.41109e-01f,-9.96402e-02f, 2.90433e-02f,-5.11376e-03f }, /*  91 */
 {-5.59731e-03f, 3.35600e-02f,-1.29874e-01f, 8.44559e-01f, 3.31048e-01f,-9.73254e-02f, 2.84182e-02f,-5.00800e-03f }, /*  92 */
 {-5.49804e-03f, 3.30021e-02f,-1.28068e-01f, 8.51437e-01f, 3.21004e-01f,-9.49727e-02f, 2.77794e-02f,-4.89961e-03f }, /*  93 */
 {-5.39500e-03f, 3.24205e-02f,-1.26161e-01f, 8.58189e-01f, 3.10980e-01f,-9.25834e-02f, 2.71272e-02f,-4.78866e-03f }, /*  94 */
 {-5.28823e-03f, 3.18153e-02f,-1.24154e-01f, 8.64812e-01f, 3.00980e-01f,-9.01591e-02f, 2.64621e-02f,-4.67520e-03f }, /*  95 */
 {-5.17776e-03f, 3.11866e-02f,-1.22047e-01f, 8.71305e-01f, 2.91006e-01f,-8.77011e-02f, 2.57844e-02f,-4.55932e-03f }, /*  96 */
 {-5.06363e-03f, 3.05345e-02f,-1.19837e-01f, 8.77666e-01f, 2.81060e-01f,-8.52109e-02f, 2.50946e-02f,-4.44107e-03f }, /*  97 */
 {-4.94589e-03f, 2.98593e-02f,-1.17526e-01f, 8.83893e-01f, 2.71144e-01f,-8.26900e-02f, 2.43930e-02f,-4.32052e-03f }, /*  98 */
 {-4.82456e-03f, 2.91609e-02f,-1.15113e-01f, 8.89984e-01f, 2.61263e-01f,-8.01399e-02f, 2.36801e-02f,-4.19774e-03f }, /*  99 */
 {-4.69970e-03f, 2.84397e-02f,-1.12597e-01f, 8.95936e-01f, 2.51417e-01f,-7.75620e-02f, 2.29562e-02f,-4.07279e-03f }, /* 100 */
 {-4.57135e-03f, 2.76957e-02f,-1.09978e-01f, 9.01749e-01f, 2.41609e-01f,-7.49577e-02f, 2.22218e-02f,-3.94576e-03f }, /* 101 */
 {-4.43955e-03f, 2.69293e-02f,-1.07256e-01f, 9.07420e-01f, 2.31843e-01f,-7.23286e-02f, 2.14774e-02f,-3.81671e-03f }, /* 102 */
 {-4.30435e-03f, 2.61404e-02f,-1.04430e-01f, 9.12947e-01f, 2.22120e-01f,-6.96762e-02f, 2.07233e-02f,-3.68570e-03f }, /* 103 */
 {-4.16581e-03f, 2.53295e-02f,-1.01501e-01f, 9.18329e-01f, 2.12443e-01f,-6.70018e-02f, 1.99599e-02f,-3.55283e-03f }, /* 104 */
 {-4.02397e-03f, 2.44967e-02f,-9.84679e-02f, 9.23564e-01f, 2.02814e-01f,-6.43069e-02f, 1.91877e-02f,-3.41815e-03f }, /* 105 */
 {-3.87888e-03f, 2.36423e-02f,-9.53307e-02f, 9.28650e-01f, 1.93236e-01f,-6.15931e-02f, 1.84071e-02f,-3.28174e-03f }, /* 106 */
 {-3.73062e-03f, 2.27664e-02f,-9.20893e-02f, 9.33586e-01f, 1.83711e-01f,-5.88617e-02f, 1.76185e-02f,-3.14367e-03f }, /* 107 */
 {-3.57923e-03f, 2.18695e-02f,-8.87435e-02f, 9.38371e-01f, 1.74242e-01f,-5.61142e-02f, 1.68225e-02f,-3.00403e-03f }, /* 108 */
 {-3.42477e-03f, 2.09516e-02f,-8.52933e-02f, 9.43001e-01f, 1.64831e-01f,-5.33522e-02f, 1.60193e-02f,-2.86289e-03f }, /* 109 */
 {-3.26730e-03f, 2.00132e-02f,-8.17385e-02f, 9.47477e-01f, 1.55480e-01f,-5.05770e-02f, 1.52095e-02f,-2.72032e-03f }, /* 110 */
 {-3.10689e-03f, 1.90545e-02f,-7.80792e-02f, 9.51795e-01f, 1.46192e-01f,-4.77900e-02f, 1.43934e-02f,-2.57640e-03f }, /* 111 */
 {-2.94361e-03f, 1.80759e-02f,-7.43154e-02f, 9.55956e-01f, 1.36968e-01f,-4.49929e-02f, 1.35716e-02f,-2.43121e-03f }, /* 112 */
 {-2.77751e-03f, 1.70776e-02f,-7.04471e-02f, 9.59958e-01f, 1.27812e-01f,-4.21869e-02f, 1.27445e-02f,-2.28483e-03f }, /* 113 */
 {-2.60868e-03f, 1.60599e-02f,-6.64743e-02f, 9.63798e-01f, 1.18725e-01f,-3.93735e-02f, 1.19125e-02f,-2.13733e-03f }, /* 114 */
 {-2.43718e-03f, 1.50233e-02f,-6.23972e-02f, 9.67477e-01f, 1.09710e-01f,-3.65541e-02f, 1.10760e-02f,-1.98880e-03f }, /* 115 */
 {-2.26307e-03f, 1.39681e-02f,-5.82159e-02f, 9.70992e-01f, 1.00769e-01f,-3.37303e-02f, 1.02356e-02f,-1.83931e-03f }, /* 116 */
 {-2.08645e-03f, 1.28947e-02f,-5.39305e-02f, 9.74342e-01f, 9.19033e-02f,-3.09033e-02f, 9.39154e-03f,-1.68894e-03f }, /* 117 */
 {-1.90738e-03f, 1.18034e-02f,-4.95412e-02f, 9.77526e-01f, 8.31162e-02f,-2.80746e-02f, 8.54441e-03f,-1.53777e-03f }, /* 118 */
 {-1.72594e-03f, 1.06946e-02f,-4.50483e-02f, 9.80543e-01f, 7.44095e-02f,-2.52457e-02f, 7.69462e-03f,-1.38589e-03f }, /* 119 */
 {-1.54221e-03f, 9.56876e-03f,-4.04519e-02f, 9.83392e-01f, 6.57852e-02f,-2.24178e-02f, 6.84261e-03f,-1.23337e-03f }, /* 120 */
 {-1.35627e-03f, 8.42626e-03f,-3.57525e-02f, 9.86071e-01f, 5.72454e-02f,-1.95925e-02f, 5.98883e-03f,-1.08030e-03f }, /* 121 */
 {-1.16820e-03f, 7.26755e-03f,-3.09503e-02f, 9.88580e-01f, 4.87921e-02f,-1.67710e-02f, 5.13372e-03f,-9.26747e-04f }, /* 122 */
 {-9.78093e-04f, 6.09305e-03f,-2.60456e-02f, 9.90917e-01f, 4.04274e-02f,-1.39548e-02f, 4.27773e-03f,-7.72802e-04f }, /* 123 */
 {-7.86031e-04f, 4.90322e-03f,-2.10389e-02f, 9.93082e-01f, 3.21531e-02f,-1.11453e-02f, 3.42130e-03f,-6.18544e-04f }, /* 124 */
 {-5.92100e-04f, 3.69852e-03f,-1.59305e-02f, 9.95074e-01f, 2.39714e-02f,-8.34364e-03f, 2.56486e-03f,-4.64053e-04f }, /* 125 */
 {-3.96391e-04f, 2.47942e-03f,-1.07209e-02f, 9.96891e-01f, 1.58840e-02f,-5.55134e-03f, 1.70888e-03f,-3.09412e-04f }, /* 126 */
 {-1.98993e-04f, 1.24642e-03f,-5.41054e-03f, 9.98534e-01f, 7.89295e-03f,-2.76968e-03f, 8.53777e-04f,-1.54700e-04f }, /* 127 */
 { 0.00000e+00f, 0.00000e+00f, 0.00000e+00f, 1.00000e+00f, 0.00000e+00f, 0.00000e+00f, 0.00000e+00f, 0.00000e+00f }, /* 128 */
};

/*
 * Linear FM discriminator for FSK4_TRACKING mode.
 *
 * Unlike fm_demod() which computes (prev_q*di - prev_i*dq) / |cur|², this
 * uses atan2(Im(conj(prev)*cur), Re(conj(prev)*cur)) — the actual phase
 * difference in radians. No magnitude normalization, so outer symbols stay
 * proportionally larger than inner symbols (the compression bug in fm_demod
 * is what caused our inner/outer ratio to collapse to 0.8 instead of 0.33).
 */
static float fm_discriminate_linear(dsp_state_t *s, float si, float sq)
{
    /* conj(prev) * cur = (prev_i - j*prev_q) * (si + j*sq)
     *                  = (prev_i*si + prev_q*sq) + j*(prev_i*sq - prev_q*si) */
    float re = s->ft_prev_i * si + s->ft_prev_q * sq;
    float im = s->ft_prev_i * sq - s->ft_prev_q * si;
    s->ft_prev_i = si;
    s->ft_prev_q = sq;
    return atan2f(im, re);
}

/*
 * Core tracking function. Called once per input sample (48kHz).
 * Returns 1 and writes to *sym_out when a symbol is ready (every 10 samples).
 * Returns 0 otherwise.
 *
 * sym_out is the tracker's normalized symbol in the [-3, +3] range (centered,
 * amplitude-normalized). Caller scales to int16 for DSD.
 */
static int fsk4_track_sample(dsp_state_t *s, float phase, float *sym_out)
{
    /* Push sample into history ring. */
    s->ft_history[s->ft_history_idx] = phase;
    s->ft_history_idx = (s->ft_history_idx + 1) & 7;  /* mod 8 */

    /* Advance symbol clock. Each input sample adds ft_symbol_time (=0.1). */
    s->ft_symbol_clock += s->ft_symbol_time;
    if (s->ft_symbol_clock < 1.0) return 0;
    s->ft_symbol_clock -= 1.0;

    /* Fractional sample position within current symbol interval.
     * mu ∈ [0, 1): 0 means sample is at "now", 1 means sample is at "now + 1 tick". */
    double mu = s->ft_symbol_clock / s->ft_symbol_time;

    /* Map fractional mu to MMSE table index. floor(0.5 + N*mu) rounds to nearest. */
    int imu = (int)(0.5 + (double)FSK4_NSTEPS * mu);
    if (imu < 0) imu = 0;
    if (imu > FSK4_NSTEPS) imu = FSK4_NSTEPS;
    int imu_p1 = imu + 1;
    if (imu_p1 > FSK4_NSTEPS) imu_p1 = FSK4_NSTEPS;

    /* Convolve history with taps[imu] and taps[imu_p1] simultaneously.
     * ft_history_idx points to next-write slot = oldest sample. */
    double interp = 0.0, interp_p1 = 0.0;
    int j = s->ft_history_idx;
    for (int i = 0; i < FSK4_NTAPS; i++) {
        double h = (double)s->ft_history[j];
        interp    += (double)FSK4_TAPS[imu   ][i] * h;
        interp_p1 += (double)FSK4_TAPS[imu_p1][i] * h;
        j = (j + 1) & 7;
    }

    /* Subtract DC offset (fine frequency correction). */
    interp    -= s->ft_fine_freq;
    interp_p1 -= s->ft_fine_freq;

    /* Normalize to nominal ±1, ±3 using tracked spread. */
    double output = 2.0 * interp / s->ft_symbol_spread;

    /* Hard symbol decision + error vs expected position.
     *
     * Spread-update gating: the inner regions are structurally biased
     * downward for zero-mean noise. At interp≈0 (where most noise mass
     * sits), both inner branches push spread DOWN by ~0.5×spread×K_SPREAD
     * per sample. With K_SPREAD=0.01 and spread=2.0, that's a constant
     * -0.01/symbol drift on noise. The spring at 5e-3 pulls back at
     * ~+0.002/symbol when spread=1.6 — the loop loses 5:1 against noise.
     *
     * Fix: only let inner-region updates affect spread when the sample
     * is at least somewhere meaningful (|interp| > 0.25*spread). Outer
     * regions always update — they require enough energy that noise
     * almost never reaches them, so they're inherently self-gated.
     *
     * The error term `err` is still computed in all four branches so
     * the timing loop (K_TIMING) downstream still sees it. */
    double err;
    if (interp < -s->ft_symbol_spread) {
        /* Region: outer negative (expect -1.5 * spread). */
        err = interp + 1.5 * s->ft_symbol_spread;
        s->ft_symbol_spread -= err * 0.5 * K_SPREAD;
    } else if (interp < 0.0) {
        /* Region: inner negative (expect -0.5 * spread). */
        err = interp + 0.5 * s->ft_symbol_spread;
        if (interp < -0.25 * s->ft_symbol_spread)
            s->ft_symbol_spread -= err * K_SPREAD;
    } else if (interp < s->ft_symbol_spread) {
        /* Region: inner positive (expect +0.5 * spread). */
        err = interp - 0.5 * s->ft_symbol_spread;
        if (interp >  0.25 * s->ft_symbol_spread)
            s->ft_symbol_spread += err * K_SPREAD;
    } else {
        /* Region: outer positive (expect +1.5 * spread). */
        err = interp - 1.5 * s->ft_symbol_spread;
        s->ft_symbol_spread += err * 0.5 * K_SPREAD;
    }

    /* Symbol timing: compare current interp to the sample one tap ahead.
     * If interp_p1 < interp, the signal is decreasing through our sample
     * point — we're sampling on the down-slope. Push the clock forward. */
    if (interp_p1 < interp) {
        s->ft_symbol_clock += err * K_TIMING;
    } else {
        s->ft_symbol_clock -= err * K_TIMING;
    }

    /* Clamp spread to ±20% of nominal. */
    if (s->ft_symbol_spread < SPREAD_MIN) s->ft_symbol_spread = SPREAD_MIN;
    if (s->ft_symbol_spread > SPREAD_MAX) s->ft_symbol_spread = SPREAD_MAX;

    /* Continuous restoring spring toward nominal spread = 2.0.
     *
     * Session 10: replaces the old "only-when-no-signal" bleed with a
     * weaker continuous pull. Rationale:
     *
     * The four-region spread update is biased against symmetric noise.
     * For any zero-mean distribution where most mass is within |interp| <
     * 0.5*spread (which is true for band-limited noise and also for
     * RRC-filtered signal between symbols), the net update is negative
     * — spread drifts down until it hits SPREAD_MIN and sticks.
     *
     * The old gate `if (!dsp_has_signal_lock)` meant the spring only
     * pushed back during confirmed-no-signal periods. But the spread
     * collapses DURING signal acquisition too, because the tracker
     * sees noise-dominated samples between symbol pulses. By the time
     * dsd_decoder_task flips dsp_has_signal_lock to 1, spread is
     * already pinned at MIN and the rest of the frame is garbage.
     *
     * Session 10-b: coefficient bumped 2e-4 → 5e-4 after measuring
     * real signal dynamics. On actual P25 traffic, the spread loop
     * has an inherent UPWARD drift of ~3.2/sec when the NID section
     * has more inner than outer symbols (common — NACs 0x527 etc. are
     * inner-heavy). The old spring pulled at 1.9/sec when spread=2.4,
     * so net growth was +1.3/sec and spread slammed against SPREAD_MAX.
     *
     * 5e-4 pulls at 4.8/sec when spread=2.4, enough to balance the
     * upward drift and let spread oscillate around its real equilibrium
     * (which seems to be ~2.3-2.5 for our signal chain). Still weak
     * enough (time-constant ~80ms) that a burst of outer symbols can
     * legitimately lift it without the spring stomping it back.
     *
     * During strong signal the spring is 10× weaker than K_SPREAD, so
     * real updates win. During weak signal it prevents both SPREAD_MIN
     * lockup (collapse) and SPREAD_MAX lockup (overshoot). During
     * noise it gently restores to nominal.
     */
    {
        /* Spring coefficient is signal-lock dependent.
         *
         * 5e-4 (the prior value) was tuned to balance the upward drift
         * during NID-heavy real signal — see Session 10-b notes above —
         * but is too weak to fight pure-noise dead-air drift. With
         * intermittent traffic, the tracker spends most of its time
         * in NOI, where the noise distribution drives the four-region
         * update negative; spread collapses to SPREAD_MIN within a few
         * hundred ms and stays there until real signal arrives, by
         * which time it can't acquire fast enough.
         *
         * During NOI: 5e-3 (10× stronger) easily overpowers noise drift
         *   and pulls spread back to nominal 2.0 within ~80ms.
         * During lock: 5e-4 (unchanged) preserves the careful balance
         *   from session 10-b that lets real signal find equilibrium
         *   around 2.3-2.5 without being stomped back. */
        extern int dsp_has_signal_lock;
        const double target = 2.0;
        const double k_spring = dsp_has_signal_lock ? 5.0e-4 : 5.0e-3;
        s->ft_symbol_spread += (target - s->ft_symbol_spread) * k_spring;
    }

    /* Fine frequency loop — RE-ENABLED at slow rate (session 10).
     *
     * Role separation with the pre-demod NCO:
     *
     *   NCO (dsp_process_iq) handles SLOW carrier drift caused by tuner
     *   crystal error. TC ~80ms during no-signal for initial convergence,
     *   then ~800ms during signal lock so it doesn't chase per-call DC.
     *
     *   ft_fine_freq handles PER-CALL residual DC that leaks past the NCO.
     *   This comes from two sources: (1) the natural DC of a non-balanced
     *   symbol burst (an LDU with more outer-positive than outer-negative
     *   has positive DC, and the NCO is deliberately slow enough not to
     *   react to that), and (2) any RTL V4 vs V3 tuner-specific response
     *   asymmetry that differs from whatever the NCO learned during idle.
     *
     *   The two loops don't fight if their timescales are well-separated:
     *   NCO TC = 800ms during signal, fine_freq TC = ~100ms. Fine_freq
     *   converges a decade faster, so during a call it catches whatever
     *   residual DC shows up and nulls it. The NCO, being much slower,
     *   barely moves during the same window.
     *
     *   Clamped to ±0.5 rad/sample. At fsk4_input_scale=7.0 that's a tracker
     *   DC correction of 3.5 pre-normalized, enough to catch observed
     *   session 10 V4 biases of ~2.5. Old unclamped range was ±2.0 which
     *   let it wander into runaway during bad-sync noise periods.
     *
     *   K_FINE_FREQ = 0.125 gives convergence TC ~50ms on strong signal.
     *   That's fast enough to settle within the first LDU of a call but
     *   slow enough not to track the symbol stream itself (symbols modulate
     *   at 4800 Hz, fine_freq cutoff is ~20 Hz).
     *
     *   Gated on dsp_has_signal_lock: during NOI (noise-only) periods the
     *   error term is dominated by random samples in the mid-region, not
     *   real symbol decisions, so integrating it just walks fine_freq to
     *   ±0.5 (its clamp) over a few seconds. With the gate, dead-air time
     *   doesn't corrupt the fine_freq estimate; when sync arrives the
     *   loop starts from wherever it last had a real lock (or 0 after a
     *   watchdog reset).
     */
    {
        extern int dsp_has_signal_lock;
        if (dsp_has_signal_lock) {
            s->ft_fine_freq += err * K_FINE_FREQ;
            if (s->ft_fine_freq >  0.5) s->ft_fine_freq =  0.5;
            if (s->ft_fine_freq < -0.5) s->ft_fine_freq = -0.5;
        }
    }

    /* Coarse frequency loop — diagnostic only (we don't retune the front end). */
    s->ft_coarse_freq += (s->ft_fine_freq - s->ft_coarse_freq) * K_COARSE_FREQ;

    *sym_out = (float)output;

    /* ── Diagnostic: log tracker state during real calls.
     * Faster during sync (every 2400 symbols = ~500ms), much slower during
     * noise (every 24000 symbols = ~5s) so a long quiet period doesn't
     * flood the event log with tracker-on-noise garbage. */
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

/* Global diag-reset flag set by dsp_set_mode() on entry to FSK4_TRACKING. */
int fsk4_diag_reset_flag = 0;

/*
 * Signal-lock flag set by class_driver.c when DSD has found a P25 frame
 * sync. Used by fsk4_track_sample to:
 *   (a) rate the diagnostic logger (fast during signal, slow during noise)
 *   (b) let class_driver.c gate the auto-retune logic
 *
 * Lives in dsp_pipeline.c to be near where it's read; extern-declared by
 * any task that needs to write it.
 */
int dsp_has_signal_lock = 0;

/*
 * Wall-clock deadline (microseconds since boot, esp_timer_get_time())
 * past which the C4FM DC tracker is allowed to update. While now() is
 * less than this value, the DC tracker is frozen — its value at the
 * start of the call carries through.
 *
 * Why: the DC tracker is a slow IIR running off the demod output.
 * During an active P25 voice call, the symbol stream is biased by
 * whatever IMBE happens to send (think long runs of +3, +1 dibits).
 * A tracker that updates during this gets pulled by symbol bias and
 * shifts the slicer thresholds mid-call, corrupting BCH on later NIDs.
 *
 * App_p25 sets this to `now + 2 seconds` on every successful sync.
 * As long as syncs keep hitting at >0.5 Hz, the tracker stays frozen
 * for the duration of the call. Between calls, the deadline expires
 * and the tracker resumes (at hunt-mode α=0.0005) so it converges
 * on the new noise/signal mean before the next call.
 *
 * 0 = never frozen (default at boot — let the tracker run during cold
 * startup so it converges on first signal).
 */
int64_t dsp_dc_freeze_until_us = 0;

/*
 * 256-entry sin lookup table for the NCO.
 * nco_sin_lut[i] = sin(i * 2π / 256) for i ∈ [0, 256).
 * cos is obtained by indexing at i + 64 (quarter-wave offset).
 *
 * Generated with:
 *   python -c "import math; print(', '.join(f'{math.sin(i*2*math.pi/256):+.6f}f' for i in range(256)))"
 */
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

/* ── Main entry ── */
int dsp_process_iq(dsp_state_t *s, const uint8_t *iq_data, int iq_len,
                   int16_t *audio_out, int audio_max)
{
    int n_out = 0, n_iq = iq_len / 2;
    for (int k = 0; k < n_iq; k++) {
        float fi = ((float)iq_data[k * 2] - 127.5f) / 127.5f;
        float fq = ((float)iq_data[k * 2 + 1] - 127.5f) / 127.5f;

        /*
         * CIC pre-decimation: average every DSP_PRE_DECIM (4) IQ samples.
         * 960 kHz → 240 kHz. Nearly free (just additions).
         * This gives +6 dB SNR from oversampling gain.
         */
        s->cic_acc_i += fi;
        s->cic_acc_q += fq;
        s->cic_count++;
        if (s->cic_count < DSP_PRE_DECIM) continue;

        /* Output averaged sample at 240 kHz */
        float ai = s->cic_acc_i * (1.0f / DSP_PRE_DECIM);
        float aq = s->cic_acc_q * (1.0f / DSP_PRE_DECIM);
        s->cic_acc_i = 0;
        s->cic_acc_q = 0;
        s->cic_count = 0;

        /* ── Pre-demod NCO: rotate IQ by -nco_phase to cancel any fixed
         *    carrier frequency offset before the FM discriminator sees it.
         *
         * Without this correction, a 4 kHz carrier offset produces a
         * constant phase-per-sample bias of ~1 rad/sample at 24 kHz. That
         * bias is LARGER than the actual P25 symbol modulation (±0.47
         * rad for outer symbols), so the symbols all appear one-sided and
         * the FSK4 tracker can't find its constellation.
         *
         * Uses a 256-entry sin LUT for speed — runtime cos/sin at 240 kHz
         * rate would cost 5-10% CPU; LUT is near-free.
         *
         * NCO feedback is updated in the FSK4_TRACKING branch below based
         * on the running DC of discriminator phase output. */
        {
            /* Map nco_phase ∈ [-2π, +2π] to LUT index [0, 256). */
            static const int NCO_LUT_SIZE = 256;
            extern const float nco_sin_lut[];   /* defined below */
            double p = s->nco_phase;
            if (p < 0) p += 6.283185307;
            int idx = (int)(p * (NCO_LUT_SIZE / 6.283185307));
            idx &= (NCO_LUT_SIZE - 1);
            int idx_cos = (idx + NCO_LUT_SIZE / 4) & (NCO_LUT_SIZE - 1);
            float nc = nco_sin_lut[idx_cos];  /* cos(p) = sin(p + π/2) */
            float ns = nco_sin_lut[idx];
            float ai2 = ai * nc + aq * ns;
            float aq2 = aq * nc - ai * ns;
            ai = ai2; aq = aq2;
            s->nco_phase += s->nco_step_rad;
            /* Wrap to keep phase bounded. */
            if (s->nco_phase >  6.283185307) s->nco_phase -= 6.283185307;
            if (s->nco_phase < -6.283185307) s->nco_phase += 6.283185307;
        }

        /* From here, everything runs at 240 kHz exactly as before */
        float si, sq;
        if (!lpf_decimate(s, ai, aq, &si, &sq)) continue;

        /* DC BLOCKER: subtract running mean of I and Q before demod/AGC.
         * The R828D's LO leakage creates a DC spur at tune center that,
         * if left in place, makes atan2 unstable near the IQ origin and
         * produces random ±π output that looks like pure noise. IIR with
         * α=0.001 at 24 kHz post-LPF rate → ~1 s TC, well above P25 symbol
         * period so real modulation is untouched. Disable by setting
         * s->dc_alpha = 0. */
        if (s->dc_alpha > 0.0f) {
            s->dc_avg_i += s->dc_alpha * (si - s->dc_avg_i);
            s->dc_avg_q += s->dc_alpha * (sq - s->dc_avg_q);
            si -= s->dc_avg_i;
            sq -= s->dc_avg_q;
        }

        /* AGC normalizes signal level before demod */
        agc_apply(s, &si, &sq);

        if (s->mode == DEMOD_C4FM) {
            if (n_out < audio_max) {
                /* Discriminator → matched filter → DC removal → ring.
                 *
                 * The 21-tap RRC matched filter (rrc_fsk4_filter, beta
                 * 0.2, designed for 24 kHz / 4800 baud = 5 sps) is the
                 * matched-filter pair to a P25-shaped transmitter. With
                 * it, symbol centers become well-defined peaks and
                 * symbol edges become nulls — exactly what median-of-5
                 * in dsd_symbol.c needs to find consistent plateaus.
                 *
                 * Same buf as DEMOD_FSK4_TRACKING. Modes are mutually
                 * exclusive at runtime so no collision.
                 *
                 * AFC: subtract running mean of post-RRC signal. RTL-SDR
                 * crystal PPM error puts the carrier ±100-1000 Hz from
                 * tune freq, which through fm_demod becomes a constant
                 * DC offset proportional to the offset frequency. Without
                 * compensation, DSD's slicer sees an asymmetric symbol
                 * stream (e.g. center=-2200, umid=+500, lmid=-5000) and
                 * misclassifies outer symbols as inner. Time constant
                 * gates on dsp_has_signal_lock: 80 ms TC during hunt for
                 * fast initial convergence, 800 ms TC once locked so
                 * short-term symbol asymmetry within an LDU doesn't
                 * pull the AFC. Same scheme as the FSK4_TRACKING NCO
                 * AFC above. RRC has unity DC gain so subtracting
                 * post-RRC mean is equivalent to subtracting a constant
                 * pre-RRC, with better noise immunity. */
                int16_t raw = fm_demod(s, si, sq);
                float filt = rrc_fsk4_filter(s, (float)raw);

                {
                    extern int dsp_has_signal_lock;
                    extern int64_t dsp_dc_freeze_until_us;
                    /*
                     * Three-state DC tracker:
                     *
                     *   - FROZEN: in active call (now < freeze_until). Don't
                     *     touch the tracker. Symbol-stream bias during voice
                     *     would otherwise pull it 200+ units off true zero
                     *     and corrupt the slicer for subsequent NIDs.
                     *
                     *   - LOCK (slow, α=0.00005): had recent sync but freeze
                     *     window expired. Tiny crystal drift over minutes
                     *     can still nudge DC; let the slow tracker handle
                     *     that.
                     *
                     *   - HUNT (fast, α=0.0005): no recent sync. Converge
                     *     fast on initial signal so we're pre-compensated
                     *     when the next call begins.
                     */
                    int64_t now = esp_timer_get_time();
                    if (now < dsp_dc_freeze_until_us) {
                        /* frozen — skip update */
                    } else {
                        float c4fm_alpha = dsp_has_signal_lock ? 0.00005f : 0.0005f;
                        s->c4fm_dc_avg += c4fm_alpha * (filt - s->c4fm_dc_avg);
                    }
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

            /* NCO AFC: update the NCO step toward cancelling the DC of
             * discriminator phase. Learn from RAW (pre-filter) phase so
             * our DC estimate isn't biased by the filter's group delay.
             * nco_dc_avg is an IIR of phase; once it settles, it IS the
             * residual carrier offset per 24 kHz sample, which we feed
             * back as nco_step_rad.
             *
             * CRITICAL: the NCO runs at 240 kHz (post-CIC) but we learn
             * the phase bias from the 24 kHz discriminator output (post-
             * LPF). Phase-per-sample scales linearly with sample rate,
             * so nco_step at 240 kHz must be nco_dc_avg / 10.
             *
             * Session 10: coefficient is gated on dsp_has_signal_lock.
             *
             * During NO-signal: 0.0005 (TC ~80ms) → fast convergence at
             * call start so we're pre-compensated by the time sync fires.
             *
             * During signal lock: 0.00005 (TC ~800ms) → near-frozen. A
             * real P25 transmission has short-term DC asymmetry that's
             * NOT residual carrier — it's just the symbol stream happening
             * to have more +3s than -1s over 100ms. If the fast NCO chases
             * that, it rotates the IQ during the call, which causes the
             * tracker output to drift, which shifts state->min/state->max
             * in DSD, which moves state->center away from 0, which makes
             * the dibit slicer misread NID symbols. The result we saw in
             * session-9 logs: sync correlator catches 0x527 (pattern match
             * is robust), but the dibit-by-dibit read gets 0x888/0xA9A/0x126
             * because by then the slicer baseline has drifted.
             *
             * 800ms TC is still fast enough to track actual crystal drift
             * (which happens on thermal timescales of minutes), and lets
             * the NCO finish converging during a long silent period
             * between calls. It's slow enough that a single 1-2 second
             * call can't perturb it significantly from whatever zero it
             * learned beforehand.
             *
             * The initial 80ms convergence only costs us the first call
             * after boot — after that, nco_step_rad persists across
             * tracker resets (see dsp_fsk4_reset_tracker notes) so every
             * subsequent call starts pre-compensated. */
            {
                extern int dsp_has_signal_lock;
                double nco_alpha = dsp_has_signal_lock ? 0.00005 : 0.0005;
                s->nco_dc_avg += nco_alpha * ((double)phase_raw - s->nco_dc_avg);
            }
            double target_step = s->nco_dc_avg / (double)DSP_DECIMATION;
            s->nco_step_rad += 0.02 * (target_step - s->nco_step_rad);

            /* Matched filter: the 21-tap RRC at 5 sps cleans up the
             * post-discriminator phase so symbol centers are well-
             * defined peaks and symbol edges are nulls. Without this,
             * the tracker sees raw noisy phase where each sample has
             * significant noise + ISI contamination. With it, the
             * tracker's sliced decisions and spread learning are
             * operating on shaped pulses matching the transmitted
             * pulse shape — what the P25 standard mandates and what
             * OP25/SDRTrunk do. */
            float phase = rrc_fsk4_filter(s, phase_raw);

            float tracker_in = phase * s->fsk4_input_scale;
            float sym;
            if (fsk4_track_sample(s, tracker_in, &sym)) {
                float polarity = (s->demod_gain < 0) ? -1.0f : 1.0f;
                /* Scale: ideal |sym|=1 (inner) -> ±4000; |sym|=3 (outer) -> ±12000.
                 *
                 * Session 10: reduced from 5000 → 4000. Reason: when the
                 * tracker spread pegs at SPREAD_MAX=2.4 during strong signal,
                 * `output = 2.0 * interp / 2.4` can push sym to ±4 or beyond
                 * on transients (Gibbs-like overshoots in the normalized
                 * output). At scale 5000 those peaks hit ±20000+ and
                 * occasionally clip at ±32767. The clipped samples then
                 * corrupt DSD's state->min/max tracker, which in turn pulls
                 * state->center off-zero by 2000-3000 counts, causing the
                 * dibit slicer to misread the NID+BCH region *after* sync
                 * has already matched. Net effect: NAC matches via the
                 * sync correlator, but DIB reads back garbage and BCH
                 * fails uncorrectable.
                 *
                 * At scale 4000, peak tracker excursion of ±4.5 produces
                 * ±18000 — still inside the ±20000 soft-clip warning
                 * threshold, and far from the ±32767 hard clip. DSD's
                 * slicer also stays far from clipping on min/max, so
                 * center stays near zero and BCH has a real chance.
                 *
                 * 3:1 inner/outer ratio is what matters to DSD, not the
                 * absolute magnitude. */
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

/* ──────────────────────────────────────────────────────────────────────────
 * Auto-retune helpers — called from p25_rx_task.
 *
 * The tracker runs on the RF task (core 1), the retune decision happens on
 * the same task between IQ reads. Since there's no cross-core access here,
 * no locking is needed. Core 1 owns the tracker state entirely.
 *
 * If we ever move the retune decision elsewhere, these become the atomic
 * access points and will need proper sync.
 * ────────────────────────────────────────────────────────────────────────── */

float dsp_fsk4_get_fine_freq_hz(const dsp_state_t *s)
{
    /* Convert radians-per-sample to Hz:
     *   freq_hz = freq_rad_per_sample * sample_rate / (2π)
     * sample_rate here is the tracker input rate = DSP_AUDIO_RATE = 24000. */
    return (float)(s->ft_fine_freq * (double)DSP_AUDIO_RATE / (2.0 * 3.14159265358979323846));
}

void dsp_fsk4_clear_fine_freq(dsp_state_t *s)
{
    s->ft_fine_freq   = 0.0;
    s->ft_coarse_freq = 0.0;
}

/*
 * Full tracker reset. Called when we lose signal lock for an extended
 * period, so the tracker doesn't carry forward garbage state learned
 * from noise (in particular, ft_symbol_spread gets pushed to SPREAD_MIN
 * by noise samples near the region boundaries, and stays there).
 *
 * NOTE: we deliberately do NOT reset the NCO state. The NCO compensates
 * for tuner crystal error, which is a slow property of the hardware
 * (changes on thermal timescales, minutes), not a per-call property. If
 * we reset it every time a call ends, each new call starts from a full-
 * bias discriminator output and has to re-learn the carrier offset from
 * scratch — costing us the first ~80ms of every call to convergence.
 * Keeping nco_step_rad across calls means we're pre-compensated.
 */
void dsp_fsk4_reset_tracker(dsp_state_t *s)
{
    s->ft_fine_freq     = 0.0;
    s->ft_coarse_freq   = 0.0;
    s->ft_symbol_spread = 2.0;
    s->ft_symbol_clock  = 0.0;
    for (int k = 0; k < 8; k++) s->ft_history[k] = 0.0f;
    s->ft_history_idx   = 0;
    /* NCO state deliberately preserved across calls — see note above. */
}
