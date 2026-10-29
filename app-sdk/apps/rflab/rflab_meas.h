/* RF LAB's measurement: bench-instrument readings from a block of I/Q - peak
   frequency and power, noise floor, SNR, channel power, 99% occupied
   bandwidth, frequency error against the tuning, clipping, DC offset and
   I/Q gain/phase imbalance. Pure arithmetic on a captured block, no radio
   and no allocation: the caller owns the scratch and the samples.

   Method, from the textbook rather than any SDR program's source:
   - Spectrum: Hann-windowed 512-point FFTs, averaged, scaled so the bins
     sum to the block's mean power: a full-scale complex tone reads 0 dBFS
     whatever bin it falls in.
   - Noise floor: the median bin, corrected to a mean for a chi-square of 2K
     degrees of freedom (K frames averaged; Wilson-Hilferty).
   - Peak: the strongest bin outside a DC guard, refined by a log-parabola
     through its neighbours, then by the mean phase step of the block mixed
     down to it and boxcar-filtered (a frequency discriminator), which
     resolves a clean carrier to a few hertz where a bin is kilohertz.
   - Peak power: the main lobe (seven bins) less the noise in it.
   - Occupied bandwidth: where the cumulative spectrum crosses 0.5% and
     99.5% of the total (ITU-R SM.328, beta = 1%), on the bins less the
     floor when a signal stands above it, else on the raw bins.
   - Imbalance: blind second-order statistics of the DC-removed block,
     gain = var(I)/var(Q), phase = asin(cov(I,Q)/sqrt(var I var Q)); right
     for noise or a carrier off DC, which is what an antenna gives.

   The board's firmware carried this as dsp/rflab.c until RF LAB became an
   app; the app SDK's I/Q is signed 16-bit from every radio, so only that
   format is kept, and the maths uses the float library the board exports. */
#ifndef RFLAB_MEAS_H
#define RFLAB_MEAS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RFLAB_N 512

typedef struct {
    uint32_t rate_hz;           /* sample rate of the block                */
    uint64_t center_hz;         /* RF at 0 Hz, for ppm; 0 when unknown     */
    uint32_t chan_bw_hz;        /* channel for channel power and SNR; 0: 12.5 kHz */
    int32_t  chan_offset_hz;    /* the channel's centre from 0 Hz          */
    int32_t  expect_offset_hz;  /* where the carrier should be, from 0 Hz  */
    uint8_t  dc_guard_bins;     /* bins each side of 0 Hz the peak skips   */
} rflab_cfg_t;

typedef struct {
    bool  valid;                /* enough samples for one FFT              */
    bool  carrier;              /* the peak stands 10 dB over the floor    */
    bool  fine;                 /* frequency refined by the discriminator  */
    int   frames;               /* FFTs averaged                           */
    uint32_t pairs;             /* samples measured                        */
    float rbw_hz;               /* bin spacing                             */
    float total_dbfs;           /* mean power of the block                 */
    float peak_offset_hz;       /* carrier frequency from 0 Hz             */
    float peak_dbfs;            /* carrier power                           */
    float noise_dbfs_bin;       /* floor, per bin                          */
    float noise_dbfs_hz;        /* floor, per hertz                        */
    float snr_db;               /* carrier over the noise in chan_bw       */
    float chan_dbfs;            /* everything in the channel               */
    float obw_hz, obw_lo_hz, obw_hi_hz;  /* 99% occupied bandwidth          */
    bool  obw_net;              /* of the signal over the floor; false: of everything */
    float freq_err_hz;          /* peak - expected                         */
    float freq_err_ppm;         /* of the RF it was received at; 0 unknown */
    float clip_pct;             /* pairs with I or Q at a rail             */
    float dc_i, dc_q;           /* mean, fraction of full scale            */
    float dc_dbfs;              /* DC power                                */
    float iq_gain_db;           /* 10 log var(I)/var(Q)                    */
    float iq_phase_deg;         /* quadrature error                        */
    float image_db;             /* image rejection the two imply           */
} rflab_result_t;

/* Scratch: the FFT, and the spectrum the last measure left for drawing
   (rflab_spectrum_db). About 14 KB. */
typedef struct {
    float re[RFLAB_N], im[RFLAB_N];
    float tw_re[RFLAB_N / 2], tw_im[RFLAB_N / 2];
    float win[RFLAB_N], acc[RFLAB_N];
    float bin[RFLAB_N];         /* calibrated power per bin, fftshifted    */
    float tmp[RFLAB_N];         /* the median's working copy              */
    int frames;
    bool ready;
} rflab_t;

void rflab_init(rflab_t *w);

/* Measure `pairs` interleaved int16 I/Q pairs (full scale 32768). False
   when there are fewer than RFLAB_N. */
bool rflab_measure(rflab_t *w, const rflab_cfg_t *cfg, const int16_t *iq,
                   size_t pairs, rflab_result_t *out);

/* The last measure's spectrum folded to n columns, dBFS per bin, the
   strongest bin of each column (0 Hz in the middle). False before one. */
bool rflab_spectrum_db(const rflab_t *w, float *out, int n);

#endif
