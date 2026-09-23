/* What a sub-GHz capture actually is, rather than that one arrived.
 *
 * The recorder already produces the only input this needs: signed edge
 * durations in microseconds, mark positive and space negative, which is the
 * Flipper .sub RAW convention and what rec_watch keeps per slot. Everything
 * here is integer work on that array - no radio, no sample rate, no
 * dependency on which receiver captured it - so a decoder can be developed
 * and regression-tested on the host against recorded edges.
 *
 * The family decoded here is the pulse-width OOK used by EV1527, PT2262,
 * HT12E and the remotes, door contacts and PIR sensors built on them: a long
 * low preamble, then each bit as a short mark and long space or the reverse.
 * rec_decode_ook24() reads exactly 24 bits of it; this reads whatever length
 * the frame carries and repeats, which is what separates one device family
 * from another when the encoding is shared.
 */
#ifndef LS_SUBGHZ_PWM_H
#define LS_SUBGHZ_PWM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Frames longer than this are not this family; 64 is also as much as a
   uint64_t payload can carry without a second word. */
#define SUBGHZ_PWM_MAX_BITS 64
#define SUBGHZ_PWM_MIN_BITS 12

typedef struct {
    uint64_t value;       /* payload, first bit received in the high position */
    uint8_t  bits;        /* how many were carried */
    uint8_t  repeats;     /* identical frames seen back to back */
    uint16_t unit_us;     /* the short element, which sets the whole timing */
    uint16_t preamble_units; /* low preamble, in units - 31 for EV1527 */
    bool     inverted;    /* a one is the LONG mark (EV1527), or the short one */

    /* Set only where the bit count identifies a known layout. An
       interpretation that is not certain is not offered: a wrong device name
       is worse than a payload the operator reads themselves. */
    const char *family;   /* "EV1527" for 24 bits, else NULL */
    uint32_t    id;       /* EV1527: the 20-bit address */
    uint8_t     button;   /* EV1527: the 4-bit button / channel nibble */
} subghz_pwm_t;

/* Decode the strongest repeated frame in `pulse`. Returns false when no
   frame repeats - a single unrepeated match is noise more often than it is a
   device, which is the rule rec_decode_ook24() already applies. */
bool subghz_pwm_decode(const int32_t *pulse, int edges, subghz_pwm_t *out);

/* One line for a screen: "EV1527 id 0A3B4C btn 5" or "36 bits 0123456789". */
size_t subghz_pwm_format(const subghz_pwm_t *in, char *out, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* LS_SUBGHZ_PWM_H */
