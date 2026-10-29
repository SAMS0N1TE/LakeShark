/* See rx_tape.h. Moved from screens/main/scr_fm.c. */
#include "rx_tape.h"
#include "../ls_i18n.h"
#include <stdio.h>
#include <string.h>

enum { TAPE_IDLE = 0, TAPE_ADDR, TAPE_MSG, TAPE_ERR };

static void tape_push(rx_tape_t *t, uint8_t mark, int n)
{
    /* A burst is capped: a counter that jumped by thousands because the
       screen was away means the tape cannot show what happened in between,
       and filling it entirely with one mark would claim that it can. */
    if (n > RX_TAPE_MAX / 4) n = RX_TAPE_MAX / 4;
    for (int i = 0; i < n; i++) {
        if (t->n < RX_TAPE_MAX) {
            t->mark[t->n++] = mark;
        } else {
            memmove(t->mark, t->mark + 1, RX_TAPE_MAX - 1);
            t->mark[RX_TAPE_MAX - 1] = mark;
        }
    }
}

void rx_tape_sample(rx_tape_t *t, const rx_pager_counts_t *c)
{
    /* Only a decoder that counts codewords fills the tape. Anywhere else the
       tape holds nothing, and the next time it does it starts from that count. */
    if (!c->tape) { t->n = 0; t->valid = false; return; }

    const uint32_t f = c->frames, a = c->addr, m = c->msg, e = c->errs;

    /* Counters only go up, and a decoder restart takes them back to zero.
       Treating that as a huge negative delta would push nothing; treating it
       as a huge positive one would fill the tape. Re-baseline instead. */
    if (!t->valid || f < t->frames || a < t->addr || m < t->msg || e < t->err) {
        if (t->valid) t->n = 0;
        t->valid = true;
        t->frames = f; t->addr = a; t->msg = m; t->err = e;
        return;
    }

    const uint32_t d_addr = a - t->addr;
    const uint32_t d_msg  = m - t->msg;
    const uint32_t d_err  = e - t->err;
    uint32_t d_frame = f - t->frames;

    /* The interesting ones first, then whatever frames are left over as
       idle - so a batch that carried one address among fifteen idles reads
       as one address among fifteen idles. */
    tape_push(t, TAPE_ERR,  (int)d_err);
    tape_push(t, TAPE_ADDR, (int)d_addr);
    tape_push(t, TAPE_MSG,  (int)d_msg);

    const uint32_t named = d_addr + d_msg + d_err;
    if (d_frame > named) d_frame -= named; else d_frame = 0;
    /* A frame is sixteen codewords; the tape is codewords. */
    tape_push(t, TAPE_IDLE, (int)(d_frame * 16));

    t->frames = f; t->addr = a; t->msg = m; t->err = e;
}

void rx_tape_draw(const rx_tape_t *t, const rx_pager_counts_t *pc, tui_surface *sf, tui_rect a)
{
    const uint8_t dim = LS_ATTR_DIM;
    const uint8_t yel  = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t wht  = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    const uint8_t red  = TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK);
    const uint8_t good = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    char buf[48];

    ls_panel_box(sf, a, "STREAM", TUI_CYAN);
    if (a.h < 5 || a.w < 24) return;

    /* The counters go on the right, against the frame, so the tape gets the
       width. Sync first: it is the one that decides whether any of the rest
       means anything. */
    const int cw = 13;
    const int tw = a.w - 2 - cw;
    if (tw < 8) return;

    const int cx = a.x + tw + 2;
    int row = 1;
    if (pc->chip) {
        snprintf(buf, sizeof(buf), "%lu rx", (unsigned long)pc->heard);
        tui_put_str(sf, a, cx, a.y + row++, buf, pc->heard ? good : dim);
    } else {
        tui_put_str(sf, a, cx, a.y + row++, ls_tr(pc->sync ? "SYNC" : "no sync"),
                    pc->sync ? good : dim);
    }
    if (pc->baud) {
        snprintf(buf, sizeof(buf), "%d bd", pc->baud);
        tui_put_str(sf, a, cx, a.y + row++, buf, dim);
    }
    snprintf(buf, sizeof(buf), "%lu pg", (unsigned long)pc->pages);
    tui_put_str(sf, a, cx, a.y + row++, buf, pc->pages ? yel : dim);
    if (!pc->chip && row < a.h - 2) {
        snprintf(buf, sizeof(buf), "%lu err", (unsigned long)pc->errs);
        tui_put_str(sf, a, cx, a.y + row++, buf, pc->errs ? red : dim);
    }

    if (!pc->tape) {
        snprintf(buf, sizeof(buf), "%.*s", tw - 1, "tape: POCSAG on an SDR");
        tui_put_str(sf, a, a.x + 2, a.y + 1, buf, dim);
        return;
    }

    const int rows = a.h - 2;
    const int cap  = rows * tw;
    int start = t->n - cap;
    if (start < 0) start = 0;

    for (int i = start; i < t->n; i++) {
        const int at = i - start;
        const int x = a.x + 1 + (at % tw);
        const int y = a.y + 1 + (at / tw);
        if (y >= a.y + a.h - 1) break;

        char ch; uint8_t attr;
        switch (t->mark[i]) {
        case TAPE_ADDR: ch = 'A'; attr = yel;  break;
        case TAPE_MSG:  ch = '#'; attr = wht;  break;
        case TAPE_ERR:  ch = 'X'; attr = red;  break;
        default:        ch = '.'; attr = dim;  break;
        }
        tui_put_char(sf, a, x, y, ch, attr);
    }

    if (!t->n)
        tui_put_str(sf, a, a.x + 2, a.y + 1, ls_tr("no codewords yet"), dim);
}
