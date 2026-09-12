

#ifndef LS_FLEX_GEN_H
#define LS_FLEX_GEN_H

#include <stdint.h>
#include <stddef.h>

/* Rate/level modes.  The B word in every FLEX frame identifies which of these
   the payload uses, and a receiver that finds sync therefore also learns the
   rate before it has to demodulate the payload. */
typedef enum {
    FLEX_MODE_1600_2 = 0,    /* 1600 bps, 2-level FSK */
    FLEX_MODE_3200_2 = 1,    /* 3200 bps, 2-level FSK */
    FLEX_MODE_3200_4 = 2,    /* 3200 bps, 4-level FSK (1600 sym/s) */
    FLEX_MODE_6400_4 = 3,    /* 6400 bps, 4-level FSK (3200 sym/s) */
    FLEX_MODE_COUNT
} flex_mode_t;

/* 32-bit A word (frame sync).  Fixed across all modes: a receiver correlates
   the incoming bit stream against this, then reads the mode word that follows
   to learn the rate/level of the payload. */
extern const uint32_t FLEX_SYNC_A;

/* 32-bit B word - one per mode.  Structured as B16 | ~B16 so a random 32-bit
   word has near-zero probability of matching any of them. */
extern const uint32_t FLEX_MODE_WORDS[FLEX_MODE_COUNT];

/* Best-match classifier: which mode does `word` look most like, accepting up
   to `tol` bit errors?  Returns -1 if nothing is close enough. */
int flex_mode_of(uint32_t word, int tol);

int flex_mode_bit_rate(flex_mode_t m);   /* bits/second */
int flex_mode_sym_rate(flex_mode_t m);   /* symbols/second */
int flex_mode_levels (flex_mode_t m);    /* 2 or 4 */

typedef enum {
    FLEX_MSG_ALPHA   = 0,    /* 7-bit ASCII, 3 chars per codeword */
    FLEX_MSG_NUMERIC = 1,    /* 4-bit BCD, 5 digits per codeword */
} flex_msg_type_t;

typedef struct {
    flex_mode_t mode;
    float       level;         /* deviation amplitude, 1.0 nominal */
    float       noise;         /* additive noise sd (0 = clean) */
    float       dc;            /* discriminator DC offset (mistune) */
    float       baud_err_ppm;  /* transmitter clock error the receiver must eat */
    int         invert;        /* flip polarity, as a mistuned rx does */
    int         preamble_bits; /* dotting bits before sync, 64 nominal */
    uint64_t    seed;          /* noise seed - same seed, same waveform */
} flex_tx_cfg_t;

void flex_tx_defaults(flex_tx_cfg_t *cfg);

/* Render one FLEX frame carrying one page.  Returns samples written to `out`,
   or samples required if `out` is NULL. */
size_t flex_tx_page(const flex_tx_cfg_t *cfg, uint32_t address,
                    flex_msg_type_t msg_type, const char *text,
                    float *out, size_t out_cap);

/* A frame with no message payload - sync + mode word + FIW + an idle block.
   The receiver should hit sync, read the mode word, and produce no page. */
size_t flex_tx_idle(const flex_tx_cfg_t *cfg, float *out, size_t out_cap);

/* Building blocks the tests use to construct malformed frames. */
uint32_t flex_encode_cw(uint32_t data21);           /* BCH(31,21) + parity */
void     flex_interleave  (const uint32_t *cw8, uint8_t *bits256);
void     flex_deinterleave(const uint8_t *bits256, uint32_t *cw8);

#endif
