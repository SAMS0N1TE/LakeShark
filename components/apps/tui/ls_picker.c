/* See ls_picker.h. The list, the filter, and the rows you can hit. */
#include "ls_picker.h"

#include <stdio.h>
#include <string.h>

#include "ls_tui_ui.h"

#define A(fg, bg) TUI_ATTR((fg), (bg))

#define DIM_FG LS_DIM_FG

static bool s_open;
static char s_title[24];
static char s_why[48];

static char s_label[LS_PICKER_MAX][LS_PICKER_TEXT];
static char s_detail[LS_PICKER_MAX][LS_PICKER_DETAIL];
static int  s_n;

static ls_picker_done_t s_done;

#define FILTER_MAX 16
static char s_filter[FILTER_MAX + 1];
static int  s_flen;
static int  s_order[LS_PICKER_MAX];
static int  s_shown;
static int  s_cur;     /* index into s_order */
static int  s_top;     /* first visible, index into s_order */
/* How many rows the last draw actually fitted. A page is this, and
   it cannot be known before the list has been given its rectangle. */
static int  s_nvis;

#define ROWS_MAX 24
static tui_rect s_hit_row[ROWS_MAX];
static int      s_hit_row_at[ROWS_MAX];   /* which s_order entry it shows */
static int      s_hit_rows;
static tui_rect s_hit_prev, s_hit_next, s_hit_close;

/* ------------------------------------------------------------------ open -- */

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Case-insensitive substring. Not strcasestr: it is not in every libc this
   builds against, and the strings here are short enough that the naive walk
   is not worth an #ifdef. */
static bool contains(const char *hay, const char *needle)
{
    if (!needle[0]) return true;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && lower(*a) == lower(*b)) { a++; b++; }
        if (!*b) return true;
    }
    return false;
}

static void refilter(void)
{
    s_shown = 0;
    for (int i = 0; i < s_n; i++)
        if (contains(s_label[i], s_filter)) s_order[s_shown++] = i;

    if (s_cur >= s_shown) s_cur = s_shown - 1;
    if (s_cur < 0) s_cur = 0;
    s_top = 0;
}

void ls_picker_open(const char *title, ls_picker_done_t on_done)
{
    snprintf(s_title, sizeof(s_title), "%s", title ? title : "CHOOSE");
    s_why[0] = 0;
    s_done = on_done;
    s_n = 0;
    s_flen = 0;
    s_filter[0] = 0;
    s_cur = 0;
    s_top = 0;
    s_shown = 0;
    s_open = true;
}

bool ls_picker_add(const char *label, const char *detail)
{
    if (s_n >= LS_PICKER_MAX) return false;
    snprintf(s_label[s_n], LS_PICKER_TEXT, "%s", label ? label : "");
    snprintf(s_detail[s_n], LS_PICKER_DETAIL, "%s", detail ? detail : "");
    s_n++;
    refilter();
    return true;
}

void ls_picker_empty_reason(const char *why)
{
    snprintf(s_why, sizeof(s_why), "%s", why ? why : "");
}

void ls_picker_close(void) { s_open = false; s_done = NULL; }
bool ls_picker_active(void) { return s_open; }

static void accept(void)
{
    if (s_cur < 0 || s_cur >= s_shown) { ls_picker_close(); return; }
    const int idx = s_order[s_cur];
    const ls_picker_done_t cb = s_done;
    ls_picker_close();
    if (cb) cb(idx);
}

/* ------------------------------------------------------------------ draw -- */

/* A row is five cells tall, which is a decision and not a default. */

#define ROW_H 5

#define ROW_STEP (ROW_H - 1)

static int rows_visible(int body_h)
{
    int n = (body_h - 1) / ROW_STEP;
    if (n > ROWS_MAX) n = ROWS_MAX;
    return n > 0 ? n : 0;
}

/* Text over a dithered field, with a space either side. The field is
   texture, and a word butting straight up against it loses its first and
   last letter to the pattern - the same reason ls_dither_label draws its
   own margins. */
static void put_over(tui_surface *sf, tui_rect r, int x, int y,
                     const char *text, uint8_t attr)
{
    const int n = (int)strlen(text);
    if (n <= 0) return;
    if (x - 1 >= r.x) tui_put_char(sf, r, x - 1, y, ' ', attr);
    if (x + n < r.x + r.w) tui_put_char(sf, r, x + n, y, ' ', attr);
    for (int i = 0; i < n; i++)
        tui_put_char(sf, r, x + i, y, text[i], attr);
}

static void draw_row(tui_surface *sf, tui_rect a, int slot, int oi, bool sel)
{
    const int idx = s_order[oi];
    const uint8_t hue = sel ? (TUI_GREEN | TUI_BRIGHT) : TUI_CYAN;
    const uint8_t text = A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);

    ls_panel_box(sf, a, NULL, hue);
    tui_rect f = tui_rect_make(a.x + 1, a.y + 1, a.w - 2, a.h - 2);
    if (f.w <= 0 || f.h <= 0) return;
    ls_fill_dither(sf, f, sel ? LS_DITHER_MEDIUM : LS_DITHER_LIGHT, hue);

    const int mid = (f.h - 1) / 2;
    put_over(sf, f, f.x + 1, f.y + mid, s_label[idx], text);

    const int dn = (int)strlen(s_detail[idx]);
    if (dn > 0 && dn + 3 < f.w)
        put_over(sf, f, f.x + f.w - 1 - dn, f.y + mid, s_detail[idx],
                 A(TUI_WHITE, TUI_BLACK));

    if (slot < ROWS_MAX) {
        s_hit_row[slot] = a;
        s_hit_row_at[slot] = oi;
        if (slot + 1 > s_hit_rows) s_hit_rows = slot + 1;
    }
}

static void draw_button(tui_surface *sf, tui_rect a, const char *label,
                        bool loud, tui_rect *out)
{
    *out = a;
    if (a.w <= 2 || a.h <= 2) { out->w = 0; return; }
    const uint8_t hue = loud ? (TUI_GREEN | TUI_BRIGHT) : TUI_CYAN;
    ls_panel_box(sf, a, NULL, hue);
    tui_rect f = tui_rect_make(a.x + 1, a.y + 1, a.w - 2, a.h - 2);
    if (f.w <= 0 || f.h <= 0) return;
    ls_fill_dither(sf, f, loud ? LS_DITHER_MEDIUM : LS_DITHER_LIGHT, hue);
    ls_dither_label(sf, f, (f.h - 1) / 2, label,
                    A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
}

void ls_picker_draw(tui_surface *sf, tui_rect area)
{
    if (!s_open || area.w < 16 || area.h < 12) return;

    const uint8_t dim = A(DIM_FG, TUI_BLACK);
    s_hit_rows = 0;
    s_hit_prev.w = s_hit_next.w = s_hit_close.w = 0;

    tui_fill(sf, area, ' ', A(TUI_WHITE, TUI_BLACK));
    ls_panel_box(sf, area, s_title, TUI_CYAN);

    /* --- what is being filtered on, when anything is ------------------- */
    int y = area.y + 1;
    if (s_flen > 0) {
        char line[FILTER_MAX + 8];
        snprintf(line, sizeof(line), "/%s", s_filter);
        tui_put_str(sf, area, area.x + 2, y, line,
                    A(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
        char count[24];
        snprintf(count, sizeof(count), "%d of %d", s_shown, s_n);
        const int cn = (int)strlen(count);
        if (area.w - 2 - cn > (int)strlen(line) + 3)
            tui_put_str(sf, area, area.x + area.w - 2 - cn, y, count, dim);
        y++;
    }

    /* --- the buttons, anchored to the bottom --------------------------- */
    const int btn_h = 4;
    const int btn_y = area.y + area.h - 1 - btn_h;
    tui_rect body = tui_rect_make(area.x + 1, y, area.w - 2, btn_y - y - 1);

    const int nvis = rows_visible(body.h);
    if (nvis <= 0) return;
    s_nvis = nvis;              /* what a page is worth, see page() */

    if (s_cur < s_top) s_top = s_cur;
    if (s_cur >= s_top + nvis) s_top = s_cur - nvis + 1;
    if (s_top > s_shown - nvis) s_top = s_shown - nvis;
    if (s_top < 0) s_top = 0;

    if (s_shown <= 0) {
        ls_panel_notice(sf, body, s_title,
                        s_why[0] ? s_why : "nothing here",
                        s_flen > 0 ? "BKSP clears the filter" : NULL);
    } else {
        for (int i = 0; i < nvis && s_top + i < s_shown; i++)
            draw_row(sf, tui_rect_make(body.x, body.y + i * ROW_STEP,
                                       body.w, ROW_H),
                     i, s_top + i, (s_top + i) == s_cur);
    }

    /* Paging exists for the thumb. The arrow keys are the same gesture for
       whoever has a keyboard, and neither is drawn when everything fits. */
    const bool paged = s_shown > nvis;

    if (paged) {
        char pos[28];
        int last = s_top + nvis;
        if (last > s_shown) last = s_shown;
        snprintf(pos, sizeof(pos), " %d-%d of %d ", s_top + 1, last, s_shown);
        const int pn = (int)strlen(pos);
        /* Right end of the top border, clear of the title on the left. */
        if (area.w - 2 - pn > (int)strlen(s_title) + 4)
            tui_put_str(sf, area, area.x + area.w - 2 - pn, area.y, pos,
                        A(TUI_CYAN, TUI_BLACK));
    }
    const int bw = area.w - 2;
    if (paged) {
        const int third = bw / 3;
        /* Named for what they do. They move a page, and a button
           that says UP has to mean the thing the UP key means. */
        draw_button(sf, tui_rect_make(area.x + 1, btn_y, third - 1, btn_h),
                    "PAGE UP", false, &s_hit_prev);
        draw_button(sf, tui_rect_make(area.x + 1 + third, btn_y, third - 1,
                                      btn_h), "PAGE DN", false, &s_hit_next);
        draw_button(sf, tui_rect_make(area.x + 1 + 2 * third, btn_y,
                                      bw - 2 * third, btn_h),
                    "CLOSE", false, &s_hit_close);
    } else {
        draw_button(sf, tui_rect_make(area.x + 1, btn_y, bw, btn_h),
                    "CLOSE", false, &s_hit_close);
    }
}

/* ----------------------------------------------------------------- input -- */

static void move(int dz)
{
    if (s_shown <= 0) return;
    s_cur += dz;
    if (s_cur < 0) s_cur = 0;
    if (s_cur >= s_shown) s_cur = s_shown - 1;
}

/* A page is a screenful, not four. */

static void page(int dir)
{
    if (s_shown <= 0) return;
    const int n = s_nvis > 1 ? s_nvis : 1;

    /* The WINDOW moves, and the cursor comes with it to the top of the new
       one. Moving the cursor by n instead looks equivalent and is not: the
       draw scrolls the list to the cursor, so a cursor landing one row past
       the bottom of the window scrolls it by exactly one row. That is a
       press of PAGE DN that advances the list by a single entry, which is
       the fault this was written to cure, wearing a different hat. */
    s_top += dir * n;
    if (s_top > s_shown - n) s_top = s_shown - n;
    if (s_top < 0) s_top = 0;

    s_cur = s_top;
    if (s_cur >= s_shown) s_cur = s_shown - 1;
}

bool ls_picker_key(ls_tk_t key, char ch)
{
    if (!s_open) return false;

    if (key == LS_TK_CHAR) {
        /* Typing filters. This is the search a text field would have
           given, on the one input device that can spell - and it costs
           nothing on the days there is no keyboard, because a list nobody
           types at is just a list. */
        if (ch >= ' ' && ch < 127 && s_flen < FILTER_MAX) {
            s_filter[s_flen++] = ch;
            s_filter[s_flen] = 0;
            refilter();
        }
        return true;
    }
    switch (key) {
    case LS_TK_UP:    move(-1); return true;
    case LS_TK_DOWN:  move(1);  return true;
    case LS_TK_LEFT:  page(-1); return true;
    case LS_TK_RIGHT: page(1);  return true;
    case LS_TK_ENTER: accept();  return true;
    case LS_TK_ESC:   ls_picker_close(); return true;
    case LS_TK_BACKSPACE:
        if (s_flen > 0) { s_filter[--s_flen] = 0; refilter(); }
        else ls_picker_close();
        return true;
    default: return true;
    }
}

static bool in(tui_rect r, int col, int row)
{
    return r.w > 0 && col >= r.x && col < r.x + r.w &&
           row >= r.y && row < r.y + r.h;
}

bool ls_picker_touch(int col, int row)
{
    if (!s_open) return false;

    if (in(s_hit_close, col, row)) { ls_picker_close(); return true; }
    if (in(s_hit_prev, col, row))  { page(-1); return true; }
    if (in(s_hit_next, col, row))  { page(1);  return true; }

    for (int i = 0; i < s_hit_rows; i++) {
        if (!in(s_hit_row[i], col, row)) continue;
        /* One tap, not two.

           A tap that only moved a selection would need a second tap on a
           GO button somewhere else, and the row was already under the
           thumb. The cursor moves too, so a keyboard user's idea of where
           they are does not jump behind their back. */
        s_cur = s_hit_row_at[i];
        accept();
        return true;
    }

    return true;
}
