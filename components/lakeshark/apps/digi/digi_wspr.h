/* WSPR: weak-signal propagation beacons, decode only (plus an encoder and
   synthesizer for the bench).

   Protocol (K1JT; coding described by G4JNT, "The WSPR Coding Process"):
   a 50-bit message (28-bit callsign, 15-bit locator, 7-bit power) is
   convolutionally coded K=32 r=1/2 (polynomials 0xF2D05351, 0xE4613C47) with
   a 31-bit zero tail, bit-reverse interleaved to 162 bits, and sent as 4-FSK
   symbols sync + 2*data, 1.4648 Hz apart, 8192 samples at 12 kHz each
   (110.6 s), starting one second after an even UTC minute, within 1400-1600
   Hz of audio on the standard dial frequencies.

   The decoder mixes the 1500 Hz audio window to complex baseband at 375 Hz as
   samples arrive (360 kB of PSRAM for two minutes), then at the end of the
   cycle: quarter-symbol spectra on a half-tone grid, a sync search over
   time, frequency and drift, soft symbols, deinterleave, and a sequential
   (Fano) decoder written from the textbook algorithm. Type 1 messages print
   in full; the compound-call types 2 and 3 are reported as such. */
#ifndef DIGI_WSPR_H
#define DIGI_WSPR_H
#include <stdbool.h>
#include <stdint.h>
#include "digi_fft.h"

#define DIGI_WSPR_RATE    12000
#define DIGI_WSPR_SYMS    162
#define DIGI_WSPR_SPS     8192
#define DIGI_WSPR_TONE_HZ (12000.0f / 8192.0f)
#define DIGI_WSPR_BB_RATE 375
#define DIGI_WSPR_CENTER  1500.0f

bool digi_wspr_pack(const char *call, const char *grid4, int dbm, uint8_t data[7]);
/* False for a message that is not type 1; call/grid then say what it is. */
bool digi_wspr_unpack(const uint8_t data[7], char call[12], char grid[7], int *dbm);
/* 50 bits -> 162 channel symbols 0..3. */
void digi_wspr_encode(const uint8_t data[7], uint8_t sym[162]);
/* Sequential decode of 162 soft bits in channel (interleaved) order,
   0 = surely 0, 255 = surely 1. True with the 50 bits in data on success. */
bool digi_wspr_fano(const uint8_t soft[162], uint8_t data[7], long max_cycles, long *cycles);
/* Add a continuous-phase 4-FSK signal with tone 0 at f_hz to out[]. */
void digi_wspr_synth(const uint8_t sym[162], float f_hz, float amp, float *out, long n, long start);

typedef struct {
    float snr_db;       /* in 2500 Hz                               */
    float dt_s;         /* start relative to the even minute + 1 s  */
    float freq_hz;      /* audio frequency of the signal's centre   */
    float drift_hz;     /* over the transmission                    */
    char call[12], grid[7];
    int dbm;
    bool type1;
    int agree;          /* data bits matching the re-encoded message, of 162 */
    char text[32];
} digi_wspr_result_t;

typedef struct {
    digi_cpx *bb;       /* baseband, 375 Hz                         */
    long nbb, bb_max;
    float hist[512];    /* the last input samples for the FIR       */
    int hpos, phase;    /* input count modulo the decimation        */
    digi_cpx g[512];    /* complex band-pass taps                   */
    digi_fft_t fft;
    uint32_t last_us;
    int last_candidates;
} digi_wspr_dec_t;

bool digi_wspr_dec_init(digi_wspr_dec_t *d);
void digi_wspr_dec_free(digi_wspr_dec_t *d);
/* Call at the even minute. */
void digi_wspr_dec_reset(digi_wspr_dec_t *d);
void digi_wspr_dec_feed(digi_wspr_dec_t *d, const float *x, int n);
void digi_wspr_dec_feed_i16(digi_wspr_dec_t *d, const int16_t *x, int n);
int digi_wspr_dec_run(digi_wspr_dec_t *d, digi_wspr_result_t *out, int max);

#endif
