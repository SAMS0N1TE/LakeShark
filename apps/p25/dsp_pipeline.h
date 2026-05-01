#ifndef DSP_PIPELINE_H
#define DSP_PIPELINE_H

#include <stdint.h>

#define DSP_SAMPLE_RATE     960000
#define DSP_PRE_DECIM       4        /* CIC pre-decimation: 960k → 240k */
#define DSP_DECIMATION      10       /* FIR decimation: 240k → 24k (session 9: was 5) */
#define DSP_AUDIO_RATE      (DSP_SAMPLE_RATE / DSP_PRE_DECIM / DSP_DECIMATION)  /* 24000 */
#define DSP_FIR_TAPS        31       /* Session 9b: reduced from 61 back to 31
                                      * for shorter group delay (62µs = 30% of
                                      * symbol) so 5-sps median has plateaus
                                      * to work with. 61 taps at fc=6 kHz had
                                      * 125 µs group delay = 60% of symbol,
                                      * smearing transitions across the next
                                      * symbol's window. */
#define DSP_SPS             5        /* samples per symbol at 24kHz / 4800 baud */
#define DSP_BAUD            4800     /* P25 Phase 1 symbol rate */

typedef enum {
    DEMOD_C4FM          = 0,   /* FM discriminator for C4FM systems (original) */
    DEMOD_CQPSK         = 1,   /* Gardner/Costas + diff QPSK for CQPSK/LSM systems */
    DEMOD_DIFF_4FSK     = 2,   /* SDRTrunk-style: diff demod + RRC + fixed quadrant slicer */
    DEMOD_FSK4_TRACKING = 3,   /* OP25 Frank/Radio-Rausch: 3-loop tracking + MMSE interp */
} demod_mode_t;

typedef struct {
    /* Anti-alias LPF + decimation */
    float fir_coeffs[DSP_FIR_TAPS];
    float fir_buf_i[DSP_FIR_TAPS];
    float fir_buf_q[DSP_FIR_TAPS];
    int   fir_idx;
    int   decim_count;

    /* Mode selection */
    demod_mode_t mode;
    float demod_gain;       /* FM demod: output scale. Negative = flip polarity */

    /* C4FM FM discriminator state */
    float prev_i;
    float prev_q;

    /* DC blocker for pre-demod IQ — running estimate of baseband DC offset.
     * R828D has significant LO leakage producing a DC spur at tune freq;
     * when signal amplitude is weak, the DC spur dominates and atan2 of
     * (cur × conj(prev)) becomes unstable near IQ origin, producing random
     * ±π output that breaks sync correlation. IIR averaging with α≈0.001
     * at 24 kHz post-LPF rate gives ~1 second time constant — slow enough
     * that P25 symbol modulation (~12 ms/symbol) is untouched. */
    float dc_avg_i;
    float dc_avg_q;
    float dc_alpha;            /* 0 = disabled, 0.001 = enabled (1s TC) */

    /* CQPSK: Gardner symbol timing recovery (operates on complex IQ at 48kHz) */
    float  g_clock;
    float  g_period;
    float  g_mu;
    float  g_gain_mu;
    float  g_gain_omega;
    float  g_omega_rel;
    float  g_di[3];
    float  g_dq[3];
    int    g_sample_idx;
    int    g_half;

    /* CQPSK: Costas loop carrier recovery */
    float  c_phase;
    float  c_freq;
    float  c_alpha;
    float  c_beta;

    /* CQPSK: differential decode */
    float  diff_prev_i;
    float  diff_prev_q;

    /* CQPSK: polarity */
    int    cqpsk_polarity;

    /* RMS AGC */
    float  agc_gain;
    float  agc_alpha;
    float  agc_ref;

    /* AGC instrumentation: running averages of input and output |IQ|
     * magnitude, plus peak input over the current diag-emit window.
     * Read by the periodic emit (1 Hz). Peak is reset to 0 by the
     * reader after each read.
     *
     * If agc_mag_in is varying significantly while agc_gain stays
     * pinned at 100, AGC is broken. If agc_mag_in is consistently
     * tiny (<0.01), the input signal is genuinely below what 100x
     * gain can lift to ref level — RF / antenna problem, not AGC.
     */
    float  agc_mag_in_avg;
    float  agc_mag_out_avg;
    float  agc_mag_in_peak;

    /* C4FM RRC symbol filter (after FM demod, before DSD) */
#define RRC_SYM_TAPS 51
    float  rrc_buf[51];
    int    rrc_idx;

    /* ── DEMOD_DIFF_4FSK state (SDRTrunk-inspired) ── */

    /* RRC pulse shaping on I and Q BEFORE differential demod */
    float  rrc_i_buf[RRC_SYM_TAPS];
    float  rrc_q_buf[RRC_SYM_TAPS];
    int    rrc_iq_idx;

    /*
     * Differential demod delay line — we need the sample from 1 symbol period
     * ago (DSP_SPS samples back) to compute the differential phase.
     * SDRTrunk uses interpolation between adjacent samples at the symbol-period
     * offset; we use a simple ring buffer of DSP_SPS samples.
     */
#define DIFF_DELAY DSP_SPS
    float  diff_ring_i[DIFF_DELAY];
    float  diff_ring_q[DIFF_DELAY];
    int    diff_ring_idx;
    int    diff_ring_filled;

    /*
     * Equalizer (SDRTrunk-style):
     *   eq_pll  = DC offset in radians (corrects freq error → phase bias)
     *   eq_gain = constellation expansion (SDRTrunk default 1.219)
     * Updated at each sync detect via external callback.
     */
    float  eq_pll;
    float  eq_gain;

    /*
     * Output scaling: converts radians to DSD's int16 range.
     * With eq_gain=1.219, ideal symbols are at ±3π/4*1.219 ≈ ±2.87 and ±π/4*1.219 ≈ ±0.957
     * We scale so that ±3π/4 maps to ±24000 (outer) and ±π/4 maps to ±8000 (inner)
     * giving DSD clean 4-level separation. Scale factor = 24000 / (3π/4) ≈ 10186
     */
    float  diff_output_scale;

    /* CIC pre-decimation: averages DSP_PRE_DECIM IQ samples (960k→240k) */
    float  cic_acc_i;
    float  cic_acc_q;
    int    cic_count;

    /*
     * FSK4_TRACKING: input scale applied to phase samples before tracker.
     *
     * Raw per-sample phase values from atan2 discriminator cover [-π, +π].
     * The tracker's decision regions expect input around ±1 (inner) and ±3
     * (outer). Ideal scale depends on actual carrier offset and modulation
     * index, which vary per signal, so we make this user-tunable from the
     * keyboard (E key in FSK4_TRACKING mode: E +0.2, Shift-E -0.2).
     *
     * Default 1.5 is empirically-chosen from observed phase range ~±2 rad.
     */
    float fsk4_input_scale;
    double ft_symbol_clock;       /* [0, 1) — fires symbol when it crosses 1 */
    double ft_symbol_time;        /* symbol_rate / sample_rate = 4800/48000 = 0.1 */
    double ft_symbol_spread;      /* constellation scale, nominal 2.0 */
    double ft_fine_freq;          /* running DC offset, radians */
    double ft_coarse_freq;        /* slow-loop aggregate, diagnostic only */
    float  ft_history[8];         /* sample history for MMSE interpolation */
    int    ft_history_idx;        /* next write slot in ft_history */
    /* ft_prev_i / ft_prev_q are the last complex sample for linear atan2
     * discriminator — deliberately separate from the legacy prev_i/prev_q
     * used by fm_demod() so the two modes don't interfere. */
    float  ft_prev_i;
    float  ft_prev_q;

    /* Pre-demod NCO — removes carrier frequency offset from IQ BEFORE
     * FM discrimination, so the discriminator output has zero mean
     * regardless of the tuning error. This is the classic software-radio
     * coarse AFC loop.
     *
     * nco_phase advances by nco_step_rad each sample. The rotation
     * cos(nco_phase) + j*sin(nco_phase) gets complex-multiplied with
     * (ai, aq) to produce DC-shifted (si, sq).
     *
     * nco_step_rad gets updated slowly based on the DC of the
     * discriminator output — if phase-per-sample averages +0.5 rad,
     * we increment nco_step_rad by a fraction of that to nudge the
     * NCO toward cancelling it.
     */
    double nco_phase;
    double nco_step_rad;
    double nco_dc_avg;

    /*
     * FSK4 matched filter: a 21-tap root-raised-cosine operating on the
     * post-discriminator phase at 5 samples/symbol (24 kHz / 4800 baud).
     * Applied BEFORE fsk4_track_sample so the tracker sees pulse-shaped
     * peaks at symbol centers with nulls at symbol edges, instead of
     * raw noisy phase. This dramatically improves symbol quality for
     * weak signals where inter-symbol interference from channel noise
     * dominates.
     */
#define RRC_FSK4_TAPS 21
    float  rrc_fsk4_buf[RRC_FSK4_TAPS];
    int    rrc_fsk4_idx;

    /* DEMOD_C4FM post-discriminator DC tracker. Removes the constant
     * phase-per-sample bias caused by RTL-SDR crystal PPM error from
     * the post-RRC signal before it enters the DSD slicer. Without
     * this, a 200-1000 Hz tuning error produces center=-2000 to -3000
     * in DSD's symbol stream, asymmetrizing umid/lmid and corrupting
     * NID dibits. The FSK4_TRACKING mode has its own NCO-based AFC
     * (nco_dc_avg/nco_step_rad above); this is the equivalent for
     * DEMOD_C4FM. Time constant gated on dsp_has_signal_lock: fast
     * (80 ms) when hunting, slow (800 ms) when locked. */
    float  c4fm_dc_avg;

} dsp_state_t;

void dsp_init(dsp_state_t *s);
void dsp_set_mode(dsp_state_t *s, demod_mode_t mode);
void dsp_set_gain(dsp_state_t *s, float gain);
void dsp_set_costas_alpha(dsp_state_t *s, float alpha);
void dsp_flip_polarity(dsp_state_t *s);
int  dsp_process_iq(dsp_state_t *s, const uint8_t *iq_data, int iq_len,
                    int16_t *audio_out, int audio_max);

/*
 * Auto-retune support for FSK4_TRACKING mode.
 *
 * dsp_fsk4_get_fine_freq_hz:
 *   Returns the tracker's learned fine frequency offset, converted to Hz.
 *   Called by p25_rx_task every ~100ms to decide whether to retune.
 *
 * dsp_fsk4_clear_fine_freq:
 *   Zero the tracker's fine_freq accumulator. Called after a retune so the
 *   tracker starts fresh at the new carrier.
 */
float dsp_fsk4_get_fine_freq_hz(const dsp_state_t *s);
void  dsp_fsk4_clear_fine_freq(dsp_state_t *s);
void  dsp_fsk4_reset_tracker(dsp_state_t *s);

#endif
