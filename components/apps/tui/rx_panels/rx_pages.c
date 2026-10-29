/* See rx_pages.h. Moved from screens/main/scr_fm.c, which draws it through
   this file now; the RADIO head's pager panels draw the same one. */
#include "rx_pages.h"
#include "../ls_i18n.h"
#include "../ls_text.h"
#include <stdio.h>
#include <string.h>

static void field(tui_surface *sf, tui_rect a, int row, const char *l,
                  const char *v, uint8_t la, uint8_t va)
{
    tui_put_str(sf, a, a.x + 2, a.y + row, l, la);
    tui_put_str(sf, a, a.x + 12, a.y + row, v, va);
}

/* HOLDING THE SELECTION ON ITS MESSAGE.

   The selection is an index into a newest-first list, so each page that
   arrives moves every message one place down and the index then names the
   next newer one: a page being read turned into another under the reader.
   While anything but the newest is selected, or a page is open, the message's
   own time and address are kept as well, and found again before the next
   draw, key or tap. Messages only move down the list, so the search starts
   where the selection was; an open message that has left the list shows
   an expired state until explicit navigation. */
void rx_pages_refind(rx_pages_t *v, const rx_pages_src_t *src)
{
    if (!v->held) return;
    v->missing = true;
    const int count = src->count();
    for (int i = v->sel; i < count; i++) {
        const fm_page_t *p = src->at(i);
        if (p && p->ts_us == v->sel_ts && p->address == v->sel_addr) {
            v->top += i - v->sel;
            v->sel = i;
            v->missing = false;
            return;
        }
    }
}

void rx_pages_remember(rx_pages_t *v, const rx_pages_src_t *src)
{
    if (v->open && v->missing) return;
    const fm_page_t *p = (v->sel > 0 || v->open) ? src->at(v->sel) : NULL;
    v->missing = false;
    v->held = p != NULL;
    if (p) { v->sel_ts = p->ts_us; v->sel_addr = p->address; }
}

void rx_pages_move(rx_pages_t *v, const rx_pages_src_t *src, int delta)
{
    if (delta) v->missing = false;
    const int count = src->count();
    v->sel += delta;
    if (v->sel >= count) v->sel = count - 1;
    if (v->sel < 0) v->sel = 0;
}

int rx_pages_controls(rx_pages_t *v, const rx_pages_src_t *src, tui_surface *sf, tui_rect area)
{
    memset(v->hit, 0, sizeof(v->hit));
    const int height = ls_tui_is_wide() ? 3 : 5;
    if (area.w < 24 || area.h < height + 4) return 0;
    rx_pages_move(v, src, 0);
    const int count = src->count();
    char center[32];
    snprintf(center, sizeof(center), "%s %d/%d", v->open ? "LIST" : "OPEN",
             count ? v->sel + 1 : 0, count);
    const char *labels[] = {"UP", center, "DOWN"};
    const bool enabled[] = {v->sel > 0, count > 0, v->sel + 1 < count};
    for (int i = 0; i < 3; ++i) {
        int x0 = area.x + area.w * i / 3;
        int x1 = area.x + area.w * (i + 1) / 3;
        tui_rect box = tui_rect_make(x0, area.y + area.h - height, x1 - x0, height);
        v->hit[i] = box;
        uint8_t attr = enabled[i] ? TUI_ATTR(TUI_BLACK, TUI_CYAN)
                                  : TUI_ATTR(TUI_WHITE, TUI_BLACK);
        tui_box(sf, box, NULL, TUI_ATTR(enabled[i] ? TUI_CYAN : TUI_WHITE, TUI_BLACK));
        for (int y = box.y + 1; y < box.y + box.h - 1; ++y)
            for (int x = box.x + 1; x < box.x + box.w - 1; ++x)
                tui_put_char(sf, box, x, y, ' ', attr);
        tui_put_str(sf, box, box.x + (box.w - (int)strlen(labels[i])) / 2,
                    box.y + box.h / 2, labels[i], attr);
    }
    return height + 1;
}

/* One row per page in landscape, three in portrait: the same measurement the
   mesh node list and the mesh settings use, and for the same reason - one row
   of a 10x17 cell is 1.3 mm and not a target. */
int rx_pages_row_h(void) { return ls_tui_is_wide() ? 1 : 3; }

/* Which page a row belongs to. Draw and hit test both go through this so
   they cannot disagree about where a row is. */
int rx_pages_at_row(const rx_pages_t *v, tui_rect r, int row)
{
    const int i = (row - r.y - 1) / rx_pages_row_h();
    return i >= 0 ? v->top + i : -1;
}

static const char *type_word(char t)
{
    switch (t) {
    case 'A': return "ALPHANUMERIC";
    case 'N': return "NUMERIC";
    case 'T': return "TONE ONLY";
    default:  return "NOT SURE - see below";
    }
}

/* The page, whole.

   Wrapped at a word where there is one, because a message broken mid-word
   every forty characters is harder to read than the truncation it replaced.
   Everything the decoder recorded is on here: a page that looks wrong is
   either the wrong baud or the wrong classification, and those are the two
   fields that say which. */
void rx_pages_detail(rx_pages_t *v, const rx_pages_src_t *src, tui_surface *sf, tui_rect pane)
{
    const uint8_t lab   = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t val   = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t txt   = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    const uint8_t dim   = TUI_ATTR(TUI_WHITE, TUI_BLACK);
    const uint8_t warn  = TUI_ATTR(TUI_YELLOW, TUI_BLACK);

    v->back = tui_rect_make(0, -1, 0, 0);

    const fm_page_t *pg = v->missing ? NULL : src->at(v->sel);
    if (!pg) {
        ls_panel_box(sf, pane, "PAGE", TUI_CYAN);
        tui_put_str(sf, pane, pane.x + 2, pane.y + 2, ls_tr("that page is gone"), dim);
        v->back = tui_rect_make(pane.x + 1, pane.y + 4, pane.w - 2, 3);
        tui_box(sf, v->back, ls_tr("BACK"), lab);
        return;
    }

    /* Wrapped FIRST, so the box can be the size of what is in it.

       A box stretched over the whole pane with a line of text near the top
       is the fault recorded on the P25 spectrum notice: forty-five
       rows of empty rectangle whose bottom edge lands on the page buttons.
       A page is six fields and a couple of lines; it should look like it. */
#define DETAIL_LINES 8
    char wrap[DETAIL_LINES][96];
    int nlines = ls_wrap_text(pg->text, pane.w - 4, (char *)wrap, sizeof(wrap[0]), DETAIL_LINES);
    if (!nlines) snprintf(wrap[nlines++], sizeof(wrap[0]), "%s", "(no text)");

    const bool unsure = (pg->type == '?');
    const int bh = ls_tui_is_wide() ? 1 : 3;
    /* border, five fields, blank, the unsure line, the message, blank, the
       way back, border. */
    int want = 1 + 5 + 1 + (unsure ? 2 : 0) + nlines + 1 + bh + 1;
    if (want > pane.h) want = pane.h;

    tui_rect area = tui_rect_make(pane.x, pane.y, pane.w, want);
    ls_panel_box(sf, area, "PAGE", TUI_CYAN);

    int y = area.y + 1;
    char buf[64];

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)pg->address);
    field(sf, area, y - area.y, ls_tr("RIC"), buf, lab, val);
    y++;

    field(sf, area, y - area.y, ls_tr("TYPE"), type_word(pg->type), lab,
          unsure ? warn : val);
    y++;

    snprintf(buf, sizeof(buf), "%u", (unsigned)pg->function);
    field(sf, area, y - area.y, ls_tr("FUNCTION"), buf, lab, val);
    y++;

    /* The baud is on the screen for the first time. The receiver
       runs three decoders at once and they all write this one list, so
       "which decoder produced this" was unanswerable from the panel - and
       it is the first question to ask about a page that reads as rubbish. */
    snprintf(buf, sizeof(buf), "%u", (unsigned)pg->baud);
    field(sf, area, y - area.y, ls_tr("BAUD"), pg->baud ? buf : "-", lab, val);
    y++;

    field(sf, area, y - area.y, ls_tr("PROTOCOL"),
          pg->protocol == FM_PAGE_PROTOCOL_FLEX ? "FLEX" : "POCSAG", lab, val);
    y += 2;

    if (unsure && y < area.y + area.h - 2) {
        tui_put_str(sf, area, area.x + 2, y,
                    ls_tr("not sure this is readable - see BAUD"), warn);
        y += 2;
    }

    for (int i = 0; i < nlines && y < area.y + area.h - 1 - bh; i++, y++)
        tui_put_str(sf, area, area.x + 2, y, wrap[i], txt);

    /* A way back that is a target, not a key. Portrait has no
       keyboard, so ESC is not available and a page you cannot leave is worse
       than no page. */
    if (area.h > bh + 4) {
        v->back = tui_rect_make(area.x + 2, area.y + area.h - 1 - bh, area.w - 4, bh);
        ls_fill_dither(sf, v->back, LS_DITHER_LIGHT, TUI_CYAN);
        ls_dither_label(sf, v->back, (bh - 1) / 2, "BACK TO THE LIST",
                        TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    }
#undef DETAIL_LINES
}

void rx_pages_list(rx_pages_t *v, const rx_pages_src_t *src, tui_surface *sf, tui_rect area,
                   const char *title, const char *empty)
{
    const uint8_t frame = TUI_ATTR(TUI_CYAN, TUI_BLACK);
    const uint8_t sel   = TUI_ATTR(TUI_BLACK, TUI_CYAN | TUI_BRIGHT);
    const uint8_t addr  = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t body  = TUI_ATTR(TUI_WHITE, TUI_BLACK);
    const uint8_t dim = LS_ATTR_DIM;

    tui_box(sf, area, title, frame);
    v->list = area;

    const int count = src->count();
    if (!count) {
        tui_put_str(sf, area, area.x + 2, area.y + 2, ls_tr("listening..."), dim);
        if (empty) tui_put_str(sf, area, area.x + 2, area.y + 3, empty, dim);
        return;
    }

    const int rh = rx_pages_row_h();
    /* The last interior row carries the hint, so it is not a list row. */
    const int rows = (area.h - 3) / rh;
    if (rows < 1) return;

    /* The window follows the selection instead of the selection
       running off the end of the window. Without this the list showed the
       newest `rows` pages and nothing else could ever be reached - which is
       the "scrolling messages as they get cut" half of the report. */
    if (v->sel < 0) v->sel = 0;
    if (v->sel > count - 1) v->sel = count - 1;
    if (v->sel < v->top) v->top = v->sel;
    if (v->sel >= v->top + rows) v->top = v->sel - rows + 1;
    if (v->top > count - rows) v->top = count - rows;
    if (v->top < 0) v->top = 0;

    for (int r = 0; r < rows; r++) {
        const int i = v->top + r;
        const fm_page_t *pg = src->at(i);
        if (!pg) break;

        const int top = area.y + 1 + r * rh;
        const int y = top + (rh - 1) / 2;
        const bool on = (i == v->sel);

        if (on) {
            tui_fill(sf, tui_rect_make(area.x + 1, top, area.w - 2, rh), ' ', sel);
        } else if (rh > 1) {
            ls_fill_dither(sf, tui_rect_make(area.x + 1, top, area.w - 2, rh),
                           LS_DITHER_LIGHT, TUI_CYAN);
        }

        char line[160];
        snprintf(line, sizeof(line), "%-9lu %c %.*s", (unsigned long)pg->address,
                 pg->type ? pg->type : '?', area.w - 16, pg->text);
        tui_put_str(sf, area, area.x + 2, y, line, on ? sel : body);
        if (!on) {
            char id[12];
            snprintf(id, sizeof(id), "%-9lu", (unsigned long)pg->address);
            tui_put_str(sf, area, area.x + 2, y, id, addr);
        }
    }

    /* Where you are in the list, and how to open one. A count that only
       appears when something is off screen: a list that fits has nothing to
       say about its own position. */
    char hint[64];
    if (count > rows)
        snprintf(hint, sizeof(hint), "%d-%d of %d   %s", v->top + 1,
                 v->top + rows, count,
                 ls_tui_is_wide() ? "ENTER opens it" : "tap twice to open");
    else
        snprintf(hint, sizeof(hint), "%s",
                 ls_tui_is_wide() ? "ENTER opens it" : "tap a page twice to open it");
    tui_put_str(sf, area, area.x + 2, area.y + area.h - 2, hint, dim);
}

/* The open page owns its keys while it is up: UP/DOWN walk to the next
   message without going back to the list first, which is what reading a
   run of pages actually looks like, and ESC goes back. */
bool rx_pages_key(rx_pages_t *v, const rx_pages_src_t *src, ls_tk_t k)
{
    if (v->open) {
        if (k == LS_TK_ESC) v->open = false;
        else if (k == LS_TK_UP || k == LS_TK_DOWN) rx_pages_move(v, src, k == LS_TK_UP ? -1 : 1);
        return true;
    }
    if (k == LS_TK_UP || k == LS_TK_DOWN) { rx_pages_move(v, src, k == LS_TK_UP ? -1 : 1); return true; }
    if (k == LS_TK_ENTER && src->count() > 0) { v->open = true; return true; }
    return false;
}

bool rx_pages_touch(rx_pages_t *v, const rx_pages_src_t *src, int col, int row)
{
    for (int i = 0; i < 3; ++i) {
        if (!tui_rect_contains(v->hit[i], col, row)) continue;
        if (i == 0) rx_pages_move(v, src, -1);
        else if (i == 2) rx_pages_move(v, src, 1);
        else if (src->count() > 0) v->open = !v->open;
        return true;
    }

    /* The open page: its BACK target, and nothing else - a stray tap
       must not lose the message you opened. */
    if (v->open) {
        if (tui_rect_contains(v->back, col, row)) v->open = false;
        return true;
    }

    /* A page in the list: the first tap picks it and a second tap on
       the same one opens it. The rule the settings fields and the mesh node
       list already use - one tap must never fire an action, because a tap
       that both moves the cursor and does something is one you cannot take
       back. */
    if (v->list.h > 0 && row > v->list.y && row < v->list.y + v->list.h - 1 &&
        col >= v->list.x && col < v->list.x + v->list.w) {
        const int i = rx_pages_at_row(v, v->list, row);
        if (i >= 0 && i < src->count()) {
            if (i == v->sel) v->open = true;
            else             v->sel = i;
        }
        return true;
    }
    return false;
}
