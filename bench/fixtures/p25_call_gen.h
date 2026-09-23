/* A complete P25 Phase 1 voice call as C4FM IQ, for host tests of the
 * voice path.
 *
 * Real captures are radio traffic and never enter the repository
 * (bench/captures/ is ignored). This builds the same thing from nothing: an
 * HDU, LDU1/LDU2 pairs carrying the IMBE frames a test supplies, and a TDU,
 * each encoded with the firmware's own FEC encoders and laid out in exactly
 * the order the decoder reads them, then rendered as the 240 kHz u8 IQ the
 * receiver sees - with the impairments that have broken voice on the bench
 * before: noise, carrier offset, clock error, a carrier arriving out of
 * silence, and gain dropping for a burst while a nearby transmitter
 * overloads the tuner.
 */
#ifndef LS_P25_CALL_GEN_H
#define LS_P25_CALL_GEN_H

#include <stdint.h>

#include "ls_test.h"

#define P25_CALL_IMBE_BYTES 11          /* 88 IMBE bits, MSB first */
#define P25_CALL_LDU_SYMBOLS 864
#define P25_CALL_HDU_SYMBOLS 396
#define P25_CALL_TDU_SYMBOLS 72

typedef struct {
    uint16_t nac;
    uint16_t talkgroup;
    uint32_t source;
    uint8_t  algid;         /* 0x80 clear; anything else is encrypted */
    uint16_t kid;
    int      hdu;           /* send an HDU ahead of the first LDU1 */
    int      tdu;           /* close with a TDU */
    int      corrupt_ldu1;  /* 1-based: damage this LDU1's NID so it is lost */
} p25_call_t;

/* Symbols ({-3,-1,+1,+3}) for a call carrying n_imbe frames, which must be a
 * multiple of 18 (an LDU1 and an LDU2 hold nine each). Returns the symbol
 * count, or -1 when sym_max is too small. */
int p25_call_symbols(const p25_call_t *call, const uint8_t (*imbe)[P25_CALL_IMBE_BYTES],
                     int n_imbe, int *sym, int sym_max);

/* Where the given frame (0-based over the call: HDU if present, then LDUs,
   then TDU) starts, in symbols from the start of the call. */
int p25_call_frame_start(const p25_call_t *call, int frame);

typedef struct {
    float start_s;      /* from the start of the rendered IQ */
    float len_s;
    float gain_db;      /* negative: the tuner compressed */
} p25_call_fade_t;

typedef struct {
    float noise_sd;     /* relative to the carrier, as lsm_render_iq */
    float freq_off_hz;
    float clock_ppm;    /* transmitter symbol clock error */
    /* A transmitter's synthesiser settling after PTT: the carrier opens
       keyup_hz off its final frequency and decays to it with time constant
       keyup_tau_s. The 154.7850 capture moved ~550 Hz over ~80 ms. */
    float keyup_hz;
    float keyup_tau_s;
    float lead_s;       /* noise only, before the carrier */
    float tail_s;       /* noise only, after it */
    int   n_fades;
    p25_call_fade_t fades[4];
} p25_call_channel_t;

/* 240 kHz u8 IQ for the symbols. Returns bytes written, 0 if iq_max is too
   small. */
int p25_call_render(const int *sym, int n_sym, const p25_call_channel_t *ch,
                    ls_rng_t *rng, uint8_t *iq, int iq_max);

/* Bytes p25_call_render will need. */
int p25_call_render_bytes(int n_sym, const p25_call_channel_t *ch);

#endif
