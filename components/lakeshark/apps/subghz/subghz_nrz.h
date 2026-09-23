/* Run-length OOK: the other half of what a sub-GHz capture turns out to be.
 *
 * subghz_pwm reads the family where each bit is its own mark-and-space pair,
 * which is EV1527 and the remotes built on it. A lot of hardware does not do
 * that. It holds the carrier on for a run of identical ones and off for a run
 * of zeros, so a mark of four units is four ones - plain NRZ at a fixed bit
 * period, with a preamble and a 1010 training run in front of it.
 *
 * Measured on 101 Flipper captures of a 433.42 MHz device: 415 us a bit,
 * runs of one to three units through the payload, and a frame that repeats
 * several times per press. subghz_pwm reads none of them, because it looks
 * for a long preamble space that this encoding never sends.
 */
#ifndef LS_SUBGHZ_NRZ_H
#define LS_SUBGHZ_NRZ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SUBGHZ_NRZ_MAX_BITS 256

typedef struct {
    uint8_t  bits[SUBGHZ_NRZ_MAX_BITS / 8];  /* payload, MSB first */
    uint16_t n_bits;      /* payload length, preamble and training removed */
    uint16_t unit_us;     /* the bit period the run lengths are counted in */
    uint8_t  repeats;     /* identical payloads in the capture */
    uint16_t training;    /* 1010 bits seen before the payload */
} subghz_nrz_t;

/* Decode the repeated frame in `pulse`. Returns false when no payload
   repeats, which is the same rule the rest of this module applies: one
   unrepeated frame is noise more often than a transmitter. */
bool subghz_nrz_decode(const int32_t *pulse, int edges, subghz_nrz_t *out);

/* "78 bits 53912E8... x5 @415us" - one line for a screen or a log. */
size_t subghz_nrz_format(const subghz_nrz_t *in, char *out, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* LS_SUBGHZ_NRZ_H */
