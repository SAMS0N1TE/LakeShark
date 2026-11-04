/* DOPPLER RADAR's arithmetic: the LR2021's 125 kHz IQ16 in, Doppler rows out.

   The SX1262 board sends a plain carrier; the LR2021 hears it straight from
   the other board and again off everything in the room. Whatever moves
   shifts its echo by f = 2 v / lambda (6.1 Hz per m/s at 915 MHz), so the
   picture is the spectrum right around the carrier:

     acquire  125 kHz -> 25 kHz, one 4096-point FFT: the carrier is the
              strongest line more than 300 Hz from the LR's own DC.
     track    an oscillator moves it to 0 Hz, 125 kHz -> 5 kHz -> 500 Hz,
              and a slow loop follows the two boards' crystals drifting
              apart (seconds, far slower than anything that moves).
     doppler  512-point FFTs at 500 Hz (0.98 Hz bins, +-250 Hz) every 128
              samples: four rows a second.

   Pure: no radio, no clock, no locks. One owner. */

#ifndef RADAR_DSP_H
#define RADAR_DSP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RADAR_IN_RATE    125000
#define RADAR_ACQ_DECIM  5            /* 125k -> 25k for the search        */
#define RADAR_ACQ_N      4096
#define RADAR_D1         25           /* 125k -> 5k                        */
#define RADAR_D2         10           /* 5k -> 500 Hz                      */
#define RADAR_FIR_TAPS   48
#define RADAR_RATE       (RADAR_IN_RATE / RADAR_D1 / RADAR_D2)   /* 500 */
#define RADAR_N          512
#define RADAR_HOP        128
#define RADAR_DC_GUARD_HZ 300.0f
#define RADAR_MIN_HZ     5.0f         /* echoes from here out: the window's main lobe */
#define RADAR_BG_ROWS    48           /* the still room, learned over ~12 s   */
#define RADAR_WARM_ROWS  8            /* 2 s of learning before anything counts */
#define RADAR_SHOW_DB    11.0f        /* drawn, and counted as motion, from here */
#define RADAR_ECHO_DB    12.0f        /* a bin this far over the room is an echo */

typedef struct {
    float db[RADAR_N];        /* power per bin, dB relative to the carrier;
                                 bin k is (k < N/2 ? k : k - N) * RATE / N Hz */
    float ex[RADAR_N];        /* dB over the still room (or over the floor
                                 with the background off): what is drawn   */
    bool learning;            /* the first 2 s of a room: nothing counted yet */
    float carrier_dbfs;       /* the direct path's level                       */
    float floor_db;           /* median of the bins away from the carrier      */
    float motion_db;          /* new power off the carrier (5..245 Hz), dB under it */
    float peak_hz;            /* strongest echo 5..245 Hz either side; 0: none */
    float peak_db;            /* its level over the room (ex)                  */
    float offset_hz;          /* where the carrier sits from the LR's tune     */
} radar_row_t;

typedef struct {
    bool locked;
    double nco_hz;                       /* the carrier's offset being removed */
    float rot[2], step[2]; int rot_n;    /* the oscillator, a phasor rotating */
    /* acquisition */
    float acq_acc[2]; int acq_n, acq_fill;
    /* stage 1 */
    float s1[2]; int s1_n;
    /* stage 2 FIR decimator */
    float fir[RADAR_FIR_TAPS];
    float hist[RADAR_FIR_TAPS][2]; int hist_at, d2_n;
    /* Doppler window */
    float win[RADAR_N][2]; int win_at, since_row, filled;
    float prev[2]; bool have_prev;
    double fll_re, fll_im;               /* sum of y[n] * conj(y[n-1]) this row */
    double drift_hz;                     /* learned drift, Hz per row */
    int lost_rows;                       /* rows with no carrier, for a re-search */
    float hann[RADAR_N];                 /* the window (Blackman-Harris) */
    float re[RADAR_N], im[RADAR_N];      /* FFT scratch */
    float acq_re[RADAR_ACQ_N], acq_im[RADAR_ACQ_N];
    float bg[RADAR_N];                   /* the still room, dB against the carrier */
    int bg_rows;
    bool subtract;                       /* show change against it (default) */
} radar_dsp_t;

/* About 50 KB: keep it static, in PSRAM on the board. Background on. */
void radar_dsp_init(radar_dsp_t *s);
/* Background subtraction on (only what changes) or off (everything). The
   room is learned again either way. */
void radar_dsp_set_background(radar_dsp_t *s, bool on);
/* A gap in the stream: the search starts again. */
void radar_dsp_reset(radar_dsp_t *s);
/* Feed IQ16LE pairs. Returns how many rows it wrote to `rows` (at most `max`). */
int radar_dsp_feed(radar_dsp_t *s, const uint8_t *iq16, size_t pairs,
                   radar_row_t *rows, int max);

/* Doppler of bin k in Hz. */
float radar_bin_hz(int k);
/* Speed for a Doppler at a carrier: v = f * c / (2 * carrier), m/s. */
float radar_speed_mps(float doppler_hz, uint32_t carrier_hz);

/* One row as text: `cols` characters (odd; the middle is the carrier, '|'),
   each the strongest bin of its slice of +-span_hz, as ' .:-=+*#%@' over
   the floor. Returns cols. `out` holds cols + 1. */
int radar_row_text(const radar_row_t *r, float span_hz, int cols, char *out);

#ifdef __cplusplus
}
#endif

#endif
