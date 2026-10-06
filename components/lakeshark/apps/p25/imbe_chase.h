#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Soft-decision correction of an IMBE 7200x4400 frame's FEC words, for a
 * symbol source that says how much it doubts each bit (0 sure .. 3).
 *
 * Chase-II: besides the word as received, the decoder tries it with each
 * subset of its most doubted bits (imbe_chase_npos of them) flipped, and keeps
 * the codeword whose disagreement with what was received weighs least, by
 * imbe_chase_weight per doubt. A bit never doubted is never
 * flipped, so with no doubts this is the hard decode mbelib does. Words are
 * integers with bit i = mbelib's in[i]: Golay(23,12) data in bits 22..11 and
 * parity in 10..0, Hamming(15,11) data in 14..4. */

uint32_t imbe_golay_encode(uint32_t data12);
uint32_t imbe_golay_correct(uint32_t word, const uint8_t doubt[23]);
uint32_t imbe_hamming_correct(uint32_t word, const uint8_t doubt[15]);

/* On a frame as process_IMBE reads it: c0, before mbe_eccImbe7200x4400C0;
 * then c1..c6, after mbe_demodulateImbe7200x4400Data has taken the PN off.
 * Each word is left a codeword, so mbelib's own pass finds nothing to fix and
 * only extracts the data. Return the bits changed, counted as mbelib counts
 * them: data bits for a Golay word, every bit for a Hamming word. c0 also
 * says how many of its changed bits were sure ones (doubt 0): the decode had
 * to overrule the receiver, and the frame is most likely wrong. */
extern int imbe_chase_npos;   /* doubted positions tried per word, 1..5 (5) */
extern uint8_t imbe_chase_weight[4];   /* what a changed bit weighs, by its doubt (13 7 4 1) */
int imbe_chase_c0(char fr[8][23], const uint8_t doubt[8][23], int *sure_changed);
int imbe_chase_data(char fr[8][23], const uint8_t doubt[8][23]);

#ifdef __cplusplus
}
#endif
