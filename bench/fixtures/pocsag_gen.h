/* POCSAG transmitter, for tests.

   Builds the same bitstream a pager transmitter does - preamble, frame sync,
   16 codewords per batch, BCH(31,21) with 0x769 and an even parity bit - and
   renders it as an FM discriminator output at FM_DEMOD_RATE, which is exactly
   what pocsag_process() is handed on the device.

   The point of having this is that "does POCSAG work" stops being a question
   you answer by standing next to a transmitter. */
#ifndef LS_POCSAG_GEN_H
#define LS_POCSAG_GEN_H

#include <stdint.h>
#include <stddef.h>

#define MSG_GEN_MAX   640    /* message bits a page can carry */
#define BITS_GEN_MAX 4096    /* preamble + two batches, comfortably */

typedef struct {
    int      baud;          /* 512 / 1200 / 2400                              */
    float    level;         /* deviation amplitude, 1.0 nominal               */
    float    noise;         /* sd of additive noise, 0 for clean              */
    float    dc;            /* discriminator DC offset (mistune)              */
    float    baud_err_ppm;  /* transmitter clock error the timing loop must eat */
    int      invert;        /* flip polarity, as a mistuned/inverted rx does  */
    int      preamble_bits; /* alternating 1010..., 576 on air                */
    uint64_t seed;          /* noise seed; same seed gives the same waveform  */
} pocsag_tx_cfg_t;

void pocsag_tx_defaults(pocsag_tx_cfg_t *cfg);

/* One codeword, already BCH-encoded and parity-stuffed. */
uint32_t pocsag_encode_address(uint32_t ric, int func);
uint32_t pocsag_encode_message(uint32_t data20);
uint32_t pocsag_encode_raw(int flag, uint32_t data20);

/* A whole transmission: preamble, then one batch carrying `ric` with `text`,
   padded with idle codewords, then a trailing sync so the batch closes.
   `text` NULL or empty produces a tone-only page.
   Returns the number of samples written to `out`, or the number required if
   `out` is NULL. */
size_t pocsag_tx_page(const pocsag_tx_cfg_t *cfg, uint32_t ric, int func,
                      const char *text, float *out, size_t out_cap);

size_t pocsag_tx_page_raw(const pocsag_tx_cfg_t *cfg, uint32_t ric, int func,
                          const uint8_t *msg_bits, size_t n_msg_bits,
                          float *out, size_t out_cap);

/* Lower level: render an explicit bit array. */
size_t pocsag_tx_bits(const pocsag_tx_cfg_t *cfg, const uint8_t *bits,
                      size_t n_bits, float *out, size_t out_cap);

/* Build the bit array for a page without rendering it. Returns bit count. */
size_t pocsag_build_bits(uint32_t ric, int func, const char *text,
                         int preamble_bits, uint8_t *bits, size_t bits_cap);

#endif
