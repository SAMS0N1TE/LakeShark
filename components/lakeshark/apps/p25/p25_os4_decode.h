#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* P25 symbols from the LR2021's 2-FSK detector run at four bits a symbol
 * (19200 bps), as measured on live C4FM: a symbol's level shows in the middle
 * three of the five bits from its start to the next symbol's first bit (three
 * ones +3, two +1, one -1, none -3).
 *
 * Which bit a symbol starts on is decided by the frame syncs: all four bit
 * phases are decoded side by side, and the phase that reproduces a frame
 * sync (24 outer symbols) best is the one used until another sync says
 * otherwise. On a change the new phase's sync is sent again, so the frame
 * that follows is found. Before any sync, the phase where bit transitions
 * cluster is used. */
typedef struct {
    uint32_t pos;          /* bits seen since the reset                         */
    uint16_t trans[4];     /* transitions per bit phase, halved every 4096       */
    uint32_t hist[4];      /* each phase's last 16 symbol signs, newest in bit 0 */
    uint32_t outer[4];     /* and whether each was an outer (+-3) level          */
    uint32_t hist2[4];     /* the 8 symbols before those: signs                  */
    uint32_t outer2[4];
    uint8_t  last24[4][24];/* each phase's last 24 dibits, a ring               */
    uint8_t  ring_at[4];
    uint8_t  win;          /* the last five bits, newest in bit 0                */
    uint8_t  last;
    int8_t   phase;        /* the phase in use, -1 before the first sync         */
    uint8_t  score[4];     /* each phase's match to the sync over its last 24    */
} p25_os4_decoder_t;

void p25_os4_reset(p25_os4_decoder_t *d);

/* Bytes are MSB first, a 1 bit is the higher frequency. Writes the decoded
 * dibits (decoder order: 1 +3, 0 +1, 2 -1, 3 -3) and returns how many; on a
 * phase change that includes the 24 sync dibits sent again. */
size_t p25_os4_decode(p25_os4_decoder_t *d, const uint8_t *bytes, size_t n,
                      uint8_t *dibits, size_t max);
#ifdef __cplusplus
}
#endif
