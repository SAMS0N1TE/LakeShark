#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* P25 symbols from the LR2021's 2-FSK detector run at four bits a symbol
 * (19200 bps), as measured on live C4FM: a symbol's level shows in the middle
 * three of the five bits from its start to the next symbol's first bit (three
 * ones +3, two +1, one -1, none -3). Its neighbours' bits say more: with
 * p25_os4_lut_on (the default) the level comes from p25_os4_lut, keyed by the
 * 16 bits from 6 before the symbol's first bit to 9 after it (oldest in bit
 * 15), learned against an RTL's decode of the same calls. Either way a symbol
 * is decided when the ninth bit after its start arrives. With p25_os4_nn_on
 * (the default) the symbols that are sent are decided by p25_os4_nn over 64
 * bits instead, P25_OS4_NN_DELAY bits after their start.
 *
 * Which bit a symbol starts on is decided by the frame syncs: all four bit
 * phases are decoded side by side, and the phase that reproduces a frame
 * sync (24 outer symbols, as decided) best is the one used until another
 * sync says otherwise. Before any sync, the phase where bit transitions
 * cluster is used. The last P25_OS4_Q symbols are kept back, so on a change
 * the new phase's sync takes the place of the old phase's reading of the
 * same air instead of following it: a sync sent twice cost the LDU after it
 * (the frame reader took the copy for the NID), 2.8% of syncs on the
 * 2026-10-04 calls. Nor does any other symbol go out twice: one that starts
 * under 3 bits after the last one sent is that symbol, read at a
 * neighbouring phase. */

#define P25_OS4_LDU     864   /* symbols from one frame sync to the next in a call */
#define P25_OS4_HOLD    1024  /* symbols held per phase while a seam is bridged    */
#define P25_OS4_GAP_MAX (P25_OS4_LDU - 1)   /* a seam's gap is known only mod an LDU */
#define P25_OS4_FILL    4     /* sent for each lost symbol: an erasure (synth)     */
#define P25_OS4_Q       32    /* symbols kept back (6.7 ms), a sync's worth and more */
#define P25_OS4_NO_BIT  (-0x40000000)   /* a kept-back symbol no sync may replace */
#define P25_OS4_LUT_BITS  16
#define P25_OS4_LUT_SIZE  (1u << P25_OS4_LUT_BITS)

/* What the decoder sends for each symbol, a byte: the dibit (0 +1, 1 +3, 2 -1,
   3 -3, or P25_OS4_FILL) in bits 0-2, and how much the lookup doubts each of
   its two bits, 0 sure .. 3: the low (inner/outer) bit's in bits 3-4, the
   high (sign) bit's in bits 5-6. The middle-three rule sends no doubt. */
#define P25_OS4_DIBIT(b)    ((uint8_t)((b) & 7u))
#define P25_OS4_DOUBTS(b)   ((uint8_t)(((b) >> 3) & 15u))   /* low bit's in 0-1, high bit's in 2-3 */

typedef struct {
    uint32_t pos;          /* bits seen since the reset                         */
    uint16_t trans[4];     /* transitions per bit phase, halved every 4096       */
    uint32_t hist[4];      /* each phase's last 16 symbol signs, newest in bit 0 */
    uint32_t outer[4];     /* and whether each was an outer (+-3) level          */
    uint32_t hist2[4];     /* the 8 symbols before those: signs                  */
    uint32_t outer2[4];
    uint8_t  last24[4][24];/* each phase's last 24 dibits, a ring               */
    uint8_t  ring_at[4];
    uint64_t win;          /* the last 64 bits, newest in bit 0                  */
    uint8_t  last;
    uint8_t  use_lut;      /* p25_os4_lut_on as it was at the reset              */
    uint8_t  use_nn;       /* p25_os4_nn_on as it was at the reset               */
    uint8_t  nn_off;       /* set by the reader while it is behind: the lookup decides, at the network's delay */
    int8_t   phase;        /* the phase in use, -1 before the first sync         */
    uint8_t  score[4];     /* each phase's match to the sync over its last 24    */
    int32_t  since_sync;   /* symbols sent since the last sync sent began, -1 unknown */
    int16_t  seam_left;    /* while bridging: symbols the interrupted LDU still had, else -1 */
    int32_t  sent_at;      /* first bit of the symbol sent last, -8 before any   */
    uint8_t  q[P25_OS4_Q]; /* symbols decided but kept back, oldest at q_at      */
    int32_t  q_bit[P25_OS4_Q]; /* the first bit of each, or P25_OS4_NO_BIT      */
    uint8_t  q_at, q_n;
    uint16_t held_n[4];    /* while bridging: symbols held per phase             */
    uint8_t  held[4][P25_OS4_HOLD];
} p25_os4_decoder_t;

extern const uint8_t p25_os4_lut[P25_OS4_LUT_SIZE];
extern volatile int p25_os4_lut_on;
extern volatile int p25_os4_nn_on;

void p25_os4_reset(p25_os4_decoder_t *d);

/* The source lost bits and starts again, as the LR2021 does between packets.
 * Inside an LDU the symbols that follow are held until the next frame sync
 * shows how many were lost; that many P25_OS4_FILL symbols go first, so the
 * reader of the LDU keeps its place and only the voice under the gap is
 * lost. The gaps measured between packets run to 1368 symbols (90% under
 * 423), so any count up to an LDU is filled. With no LDU to keep, or no sync
 * within P25_OS4_HOLD symbols, it is a reset. The symbols kept back from
 * before the seam still go out first. */
void p25_os4_seam(p25_os4_decoder_t *d);

/* Bytes are MSB first, a 1 bit is the higher frequency. Writes the decoded
 * dibits (decoder order: 1 +3, 0 +1, 2 -1, 3 -3) and returns how many, each
 * P25_OS4_Q symbols after it was decided; after a seam up to
 * P25_OS4_GAP_MAX + P25_OS4_HOLD + 24 at once. */
size_t p25_os4_decode(p25_os4_decoder_t *d, const uint8_t *bytes, size_t n,
                      uint8_t *dibits, size_t max);
#ifdef __cplusplus
}
#endif
