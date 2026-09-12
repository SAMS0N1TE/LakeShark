

#ifndef LS_ACARS_GEN_H
#define LS_ACARS_GEN_H

#include <stdint.h>
#include <stddef.h>

/* Audio-band sample rate at the decoder boundary.  19200 = 8 * 2400 gives
   exactly 8 samples per bit and 16/8 samples per cycle of mark/space, so
   the tone correlators land on clean phases without a resampler. */
#define ACARS_SAMP_RATE   19200
#define ACARS_BAUD         2400

/* MSK tones.  Continuous phase: mark advances the phase by pi per bit,
   space by 2pi. */
#define ACARS_TONE_MARK    1200      /* bit '1' */
#define ACARS_TONE_SPACE   2400      /* bit '0' */

/* Text field cap.  ARINC 618 allows up to ~220 bytes; the fixed header
   between SOH and STX is 13 bytes (mode + 7 reg + tak + 2 label + block id),
   plus SOH, STX, ETX, 2 BCS, DEL frame it. */
#define ACARS_TEXT_MAX      220

/* Frame control bytes - 7-bit ASCII, parity added at wire time. */
#define ACARS_SOH  0x01
#define ACARS_STX  0x02
#define ACARS_ETX  0x03
#define ACARS_SYN  0x16
#define ACARS_DEL  0x7F

typedef struct {
    char        mode;
    char        reg[8];       /* aircraft registration, 7 chars + NUL */
    char        tak;          /* technical ack (NAK/ACK)              */
    char        label[3];     /* 2-char message label + NUL           */
    char        block_id;     /* single-char block sequence id        */
    const char *text;         /* NULL for header-only messages        */
} acars_msg_t;

typedef struct {
    float       level;            /* audio amplitude, 1.0 nominal          */
    float       noise;            /* sd of additive noise, 0 = clean       */
    float       dc;               /* DC offset on the audio (mistune)      */
    float       baud_err_ppm;     /* transmitter clock error the rx must eat */
    int         invert;           /* flip audio polarity                   */
    int         preamble_bits;    /* alternating 1010... bit sync, 128 nom */
    /* Deliberate corruption knobs, for the parity / CRC test cases.  A
       corrupt fixture must still emit a bit stream the framer can walk. */
    int         corrupt_parity_at; /* char index whose parity is flipped, -1 for none */
    int         corrupt_text_bit;  /* nonzero: flip one bit of the text after CRC */
    uint64_t    seed;
} acars_tx_cfg_t;

void acars_tx_defaults(acars_tx_cfg_t *cfg);

/* Odd parity bit for the low 7 bits of `c`.  ACARS wire format is 7-bit
   ASCII with bit 7 chosen so that the full byte has an odd 1-count. */
uint8_t acars_odd_parity_bit(uint8_t c);
uint8_t acars_apply_parity  (uint8_t c);    /* returns c | (parity << 7) */
int     acars_byte_parity_ok(uint8_t byte); /* 1 if byte has odd parity  */

/* CRC-16-CCITT: polynomial 0x1021, initial 0xFFFF, no final XOR.
   Applied to every byte from SOH through ETX inclusive, using the 8-bit
   parity-encoded byte values.  Transmitted MSB byte first, and each byte
   sent LSB bit first like the character stream (see acars_tx_bytes). */
uint16_t acars_crc16(const uint8_t *data, size_t n);

/* Build the byte frame a real transmission carries, minus the preamble:
     SYN SYN SOH mode reg[7] tak label[2] block_id STX text[] ETX bcs_hi bcs_lo DEL
   Every content byte carries odd parity in bit 7; the two BCS bytes do
   not (they are the full 16-bit CRC value).  Returns bytes written. */
size_t acars_build_frame(const acars_msg_t *m, uint8_t *bytes, size_t cap);

/* Render one message as audio at ACARS_SAMP_RATE.  Returns samples
   written, or samples required if `out` is NULL. */
size_t acars_tx_msg(const acars_tx_cfg_t *cfg, const acars_msg_t *m,
                    float *out, size_t out_cap);

/* Lower-level: render an explicit byte frame (already assembled).  Each
   byte is emitted LSB bit first, 8 bits per byte, with the preamble
   prepended. */
size_t acars_tx_bytes(const acars_tx_cfg_t *cfg, const uint8_t *bytes,
                      size_t n_bytes, float *out, size_t out_cap);

/* Render an explicit bit array as MSK audio.  Bit 1 = mark tone, 0 =
   space tone.  Phase-continuous. */
size_t acars_tx_bits(const acars_tx_cfg_t *cfg, const uint8_t *bits,
                     size_t n_bits, float *out, size_t out_cap);

#endif
