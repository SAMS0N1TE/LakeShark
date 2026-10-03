/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#ifndef LS_HACKRF_RADIO_H
#define LS_HACKRF_RADIO_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "radio_endpoint.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS_HACKRF_CAPABILITIES LS_RADIO_RX_IQ_U8
#define LS_HACKRF_DUPLEX LS_RADIO_DUPLEX_HALF
#define LS_HACKRF_MIN_STREAM_BYTES_PER_SEC UINT32_C(4000000)

extern const ls_radio_range_t ls_hackrf_frequency_ranges[1];
extern const ls_radio_range_t ls_hackrf_sample_rate_ranges[1];

void ls_hackrf_iq_s8_to_u8(uint8_t *samples, size_t bytes);
void ls_hackrf_encode_frequency(uint64_t frequency_hz, uint8_t out[8]);
void ls_hackrf_encode_sample_rate(uint32_t sample_rate_hz, uint8_t out[8]);
uint32_t ls_hackrf_filter_bandwidth(uint32_t requested_hz,
                                    uint32_t sample_rate_hz);
bool ls_hackrf_decode_m0_state(const uint8_t *wire, size_t bytes,
                               ls_radio_iq_health_t *out);

/* The radio's own sample rates start at 2 MSPS. A slower rate is served by
   running the radio at the smallest whole multiple of it that the radio can
   do and decimating on the way out, so an app asking for 240 or 256 kSPS gets
   exactly that and never knows. */
#define LS_HACKRF_HW_MIN_RATE UINT32_C(2000000)
#define LS_HACKRF_HW_MAX_RATE UINT32_C(20000000)
#define LS_HACKRF_DECIM_MAX   20

/* The decimation for `rate_hz`: 1 when the radio can run at it, else the
   smallest factor that brings it into range, or 0 when nothing does. */
unsigned ls_hackrf_decimation(uint32_t rate_hz);

/* The radio is zero-IF: its own carrier leaks through as a spike at the
   tuned frequency, on top of whatever is there. So a decimated stream is
   tuned `shift` output-rates off to one side and mixed back, which leaves the
   spike on one of the CIC's nulls. 0 when the factor is too small to fit it. */
unsigned ls_hackrf_shift(unsigned factor);

/* A third-order CIC per rail: three integrators at the radio's rate, three
   combs at the output rate. Its nulls sit on every multiple of the output
   rate, where the aliases would land, for a few adds a sample. Input is the
   radio's signed 8-bit I/Q, output RTL-style offset binary at factor/2 times
   the input level, clipped. State runs across
   calls, so a read may end anywhere, even between I and Q. */
typedef struct {
    uint32_t integ[2][3]; /* wrap freely: only differences are used */
    uint32_t comb[2][3];
    unsigned factor, phase;
    int64_t inv;          /* the CIC gain and the mixer's 256 out, factor/2 in */
    /* The mixer, one entry per input sample of an output period, x256. */
    int16_t cos_q8[LS_HACKRF_DECIM_MAX], sin_q8[LS_HACKRF_DECIM_MAX];
    int8_t  held;         /* an I waiting for its Q */
    bool    has_held;
    /* A slow digital AGC on the output, off unless set: the radio's eight
       bits after decimation leave a weak carrier a step or two tall, and the
       FM and FSK decoders behind it need it filling the byte. It keeps the
       peaks between a quarter and three quarters of full scale, changing
       by an eighth at a time every 256 output samples. */
    bool    agc;
    int32_t agc_q8;       /* 256 is x1 */
    int32_t agc_env;      /* the output's recent peak */
    unsigned agc_count;
} ls_hackrf_cic_t;

void ls_hackrf_cic_init(ls_hackrf_cic_t *c, unsigned factor, unsigned shift);
/* Takes `bytes` of signed I/Q from `in`, writes offset-binary I/Q to `out`
   and answers how many bytes it wrote: a whole number of pairs, at most
   bytes / factor rounded up to a pair. `out` may be `in`. */
size_t ls_hackrf_cic_run(ls_hackrf_cic_t *c, const uint8_t *in, size_t bytes,
                         uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif
