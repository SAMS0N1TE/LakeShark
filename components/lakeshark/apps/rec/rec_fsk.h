/* 2-FSK slicing for the IQ recorder.
 *
 * REC finds a burst by its amplitude, which is all an OOK remote has. A
 * frequency-keyed transmitter holds its amplitude constant and moves its
 * frequency instead, so an amplitude slicer sees one long carrier and a
 * .sub saved from it replays as nothing (LS-1241: a 433.42 MHz trap
 * transmitter, 2400 baud, ~19 kHz deviation, recorded as 4-20 pulses).
 *
 * This takes the bit from the instantaneous frequency while the amplitude
 * gate says a carrier is present: above the tracked centre is a mark, below
 * is a space. The centre is tracked, not assumed, because the tuner is
 * rarely exactly on the transmitter. No hardware here, so the bench drives
 * it with synthetic IQ.
 */

#ifndef REC_FSK_H
#define REC_FSK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float   fs;          /* sample rate, Hz */
    int     pi, pq;      /* previous sample, zero-centred */
    bool    have_prev;
    bool    fresh;       /* next reading starts the filter */
    float   f_lp;        /* smoothed instantaneous frequency, Hz */
    float   tone_hi;     /* running estimate of each tone, Hz */
    float   tone_lo;
    bool    have_hi, have_lo;
    bool    bit;
    uint32_t off_n;      /* samples since the carrier dropped */
    uint32_t on_n;       /* samples since it came back */
    uint32_t cand_n;     /* samples a new tone has held so far */
    int8_t   cand_side;  /* +1 above the known tone, -1 below */
    uint32_t hi_seen, lo_seen;  /* sample index each tone was last seen */
    /* Deviation: the mean of each tone, measured separately, so neither
       the centre estimate nor the transition samples bias it. */
    double  hi_sum, lo_sum;
    uint32_t hi_n, lo_n;
} rec_fsk_t;

void rec_fsk_init(rec_fsk_t *s, float fs);

/* Start of a capture: forget the tone statistics, keep the centre. */
void rec_fsk_clear_stats(rec_fsk_t *s);

/* One IQ sample, zero-centred, and whether the amplitude gate sees a
   carrier. Returns the data level: true for the upper tone. Always false
   without a carrier, so silence reads as a space, which is how the Flipper
   treats a long negative duration. */
bool rec_fsk_step(rec_fsk_t *s, int i, int q, bool carrier);

/* Half the distance between the two tones, in Hz, from the samples seen
   since rec_fsk_clear_stats. 0 until both tones have been seen. */
uint32_t rec_fsk_deviation(const rec_fsk_t *s);

/* Bit rate from a capture's edge list: the shortest run is one bit. Runs
   shorter than min_us are ignored as slicer chatter. 0 if nothing usable. */
uint32_t rec_fsk_bitrate(const int32_t *edges, int n, uint32_t min_us);

/* What a .sub's preset says about modulation, so a file loaded back reports
   what it will replay as. `preset` is the text after "Preset:", `custom` the
   text after "Custom_preset_data:" (or NULL). Returns true for 2-FSK and
   sets *dev_hz from DEVIATN when the preset carries it. */
bool rec_fsk_preset_is_fsk(const char *preset, const char *custom,
                           uint32_t *dev_hz);

#ifdef __cplusplus
}
#endif

#endif
