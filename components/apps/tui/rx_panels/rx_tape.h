/* The pager's codeword tape: a panel of the RX HEAD, moved here from the FM
   screen. Every codeword the decoder took since the last look, one character
   each (A address, # message, X error, . idle), filling the box left to
   right, newest last; the sync, baud, page and error counts down its right.
   It moves only when the decoder counts something. */
#ifndef RX_TAPE_H
#define RX_TAPE_H

#include "../ls_tui_ui.h"

/* What the running pager decoder has counted. POCSAG on an SDR (or the
   LR2021's I/Q) counts codewords; the chip counts packets and pages; FLEX
   has no codeword counts. */
typedef struct {
    bool     on;
    bool     chip;      /* packets heard and pages, no lock or error count */
    bool     tape;      /* counts codewords, which is what the tape draws  */
    bool     sync;
    int      baud;      /* 0 where the decoder reports no rate             */
    uint32_t pages, errs, heard;
    uint32_t frames, addr, msg;   /* codeword counts, when tape           */
} rx_pager_counts_t;

#define RX_TAPE_MAX 256

typedef struct {
    uint8_t  mark[RX_TAPE_MAX];
    int      n;
    uint32_t frames, addr, msg, err;
    bool     valid;
} rx_tape_t;

void rx_tape_sample(rx_tape_t *t, const rx_pager_counts_t *c);
void rx_tape_draw(const rx_tape_t *t, const rx_pager_counts_t *c, tui_surface *sf, tui_rect a);

#endif
