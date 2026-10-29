/* FT8 and FT4: channel coding, a GFSK synthesizer and a slot decoder.

   Protocol (Franke, Somerville and Taylor, QEX 2020):
     FT8  8-GFSK BT 2.0, 6.25 Hz, 0.16 s symbols, 79 symbols, 15 s slots,
          Costas 7x7 at symbols 0, 36, 72.
     FT4  4-GFSK BT 1.0, 20.833 Hz, 0.048 s symbols, 105 symbols (two ramp
          symbols), 7.5 s slots, four 4x4 Costas groups.
     Both: 77 message bits + CRC-14 (poly 0x2757 over the message
          zero-extended to 82 bits) + LDPC(174,91), Gray-mapped tones.

   The decoder is fed 12 kHz mono samples from the slot boundary on. It turns
   them into a waterfall as they arrive (four frames per symbol in time, two
   bins per tone in frequency, kept as 0.5 dB codes in PSRAM), so the work
   left at the end of the slot is the sync search and the LDPC decodes. The
   approach - log-power waterfall, Costas sync score against neighbouring
   tones and symbols, log-max bit metrics, belief propagation - follows the
   ideas in ft8_lib by Karlis Goba (MIT); the code here is our own.

   Not thread safe: one decoder per worker. */
#ifndef DIGI_FTX_H
#define DIGI_FTX_H
#include <stdbool.h>
#include <stdint.h>
#include "digi_fft.h"
#include "digi_msg.h"

#define DIGI_FTX_RATE 12000
/* Waterfall frames per symbol. */
#ifndef DIGI_FTX_TOSR
#define DIGI_FTX_TOSR 4
#endif

typedef enum { DIGI_FT8 = 0, DIGI_FT4 = 1 } digi_ftx_mode_t;

typedef struct {
    int sym;            /* samples per symbol at 12 kHz: 1920 / 576    */
    int nn;             /* channel symbols: 79 / 105                   */
    int ntones;         /* 8 / 4                                       */
    int slot_ms;        /* 15000 / 7500                                */
    float tone_hz;      /* 6.25 / 20.833                               */
    float bt;           /* GFSK bandwidth-time product                 */
} digi_ftx_spec_t;

const digi_ftx_spec_t *digi_ftx_spec(digi_ftx_mode_t m);

/* ---- channel coding ----------------------------------------------------- */
uint16_t digi_crc14(const uint8_t *bytes, int nbits);
/* 77-bit payload -> 91 bits with CRC (12 bytes, MSB first). */
void digi_ftx_add_crc(const uint8_t payload[10], uint8_t a91[12]);
bool digi_ftx_crc_ok(const uint8_t a91[12]);
/* 91 bits -> 174-bit codeword (22 bytes). */
void digi_ldpc_encode(const uint8_t a91[12], uint8_t cw[22]);
/* llr > 0 favours a 1. Writes hard bits; returns the parity checks still
   failing (0 = a codeword). */
int digi_ldpc_decode(const float llr[174], int iters, uint8_t bits[174]);
/* Payload to channel tones; returns the count written (79 or 105). */
int digi_ftx_tones(digi_ftx_mode_t m, const uint8_t payload[10], uint8_t *tones);
/* Add a GFSK signal with base tone f0 and peak amplitude `amp` to out[],
   starting at sample `start` (may be negative; out of range is skipped). */
void digi_ftx_synth(digi_ftx_mode_t m, const uint8_t *tones, float f0_hz,
                    float amp, float *out, int n, int start);

/* The same signal produced a block at a time, for a live test source. */
typedef struct {
    digi_ftx_mode_t mode;
    uint8_t tones[105];
    float f0, amp;
    long start, t;      /* first sample index; next sample to produce */
    double phase;
} digi_ftx_tx_t;
bool digi_ftx_tx_begin(digi_ftx_tx_t *tx, digi_ftx_mode_t m, const uint8_t payload[10],
                       float f0_hz, float amp, long start);
void digi_ftx_tx_add(digi_ftx_tx_t *tx, float *out, int n, long at);

/* ---- decoder ------------------------------------------------------------ */
typedef struct {
    float snr_db;       /* in 2500 Hz, as WSJT-X reports it            */
    float dt_s;         /* start relative to slot + 0.5 s              */
    float freq_hz;      /* audio frequency of tone 0                   */
    int   score;        /* sync score                                  */
    uint8_t payload[10];
    char text[DIGI_TEXT_MAX];
    digi_msg_fields_t f;
} digi_ftx_result_t;

typedef struct {
    digi_ftx_mode_t mode;
    const digi_ftx_spec_t *spec;
    int nfft, step, tosr, bins, bin_lo, bin_hi;
    int frames, frames_max;
    uint8_t *wf;            /* frames_max x bins, 0.5 dB codes          */
    float ref_db;           /* dB mapped to code 96                    */
    bool ref_set;
    digi_fft_t fft;
    digi_cpx *fin, *fout;
    float *buf;             /* samples not yet consumed by frames       */
    int nbuf;
    long samples;           /* fed since reset                         */
    /* Statistics of the last run, for the screen and the cost log. */
    int last_candidates, last_ldpc, last_decodes;
    uint32_t last_us;
} digi_ftx_dec_t;

/* fmin..fmax: the audio band searched (Hz). */
bool digi_ftx_dec_init(digi_ftx_dec_t *d, digi_ftx_mode_t m, float fmin, float fmax);
void digi_ftx_dec_free(digi_ftx_dec_t *d);
/* A new slot: forget the waterfall, keep the level reference. */
void digi_ftx_dec_reset(digi_ftx_dec_t *d);
/* Samples from the slot boundary on, -1..1 full scale. */
void digi_ftx_dec_feed(digi_ftx_dec_t *d, const float *x, int n);
void digi_ftx_dec_feed_i16(digi_ftx_dec_t *d, const int16_t *x, int n);
/* Decode what has been fed. Returns the number of results (<= max). */
int digi_ftx_dec_run(digi_ftx_dec_t *d, digi_ftx_result_t *out, int max, digi_hash_t *h);
/* Latest waterfall line as codes, for a band-activity strip; NULL if none. */
const uint8_t *digi_ftx_dec_line(const digi_ftx_dec_t *d, int back);
float digi_ftx_dec_bin_hz(const digi_ftx_dec_t *d);

#endif
