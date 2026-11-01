/* See ls_picker.h. The list, the filter, and the rows you can hit. */
#include "ls_picker.h"

#include <stdio.h>
#include <string.h>

#include "ls_tui_ui.h"
#include "ls_i18n.h"
#include "esp_attr.h"

#define A(fg, bg) TUI_ATTR((fg), (bg))

#define DIM_FG LS_DIM_FG

static bool s_open;
static void (*s_header)(tui_surface *, tui_rect);
void ls_picker_header(void (*draw)(tui_surface *, tui_rect)) {s_header=draw;}
static void (*s_preview)(tui_surface *, tui_rect);
void ls_picker_preview(void (*draw)(tui_surface *, tui_rect)) { s_preview = draw; }
/* Long enough for a list named for what it is about and the radio doing
   it: "WATERFALL OPTIONS / RTL-SDR". */
static EXT_RAM_BSS_ATTR char s_title[40];
static char s_why[48];
static EXT_RAM_BSS_ATTR char s_note[80];

/* PSRAM: together these are a few kilobytes and they are read by the
   drawing task, never from an interrupt or with the cache down. As
   plain statics they spent internal .bss this board does not have -
   the same pressure that once left the largest DMA block at 176
   bytes and the panel dark. */
static EXT_RAM_BSS_ATTR char s_label[LS_PICKER_MAX][LS_PICKER_TEXT];
static EXT_RAM_BSS_ATTR char s_detail[LS_PICKER_MAX][LS_PICKER_DETAIL];
static int  s_n;

static bool s_keep_open;
static ls_picker_done_t s_done;
static void (*s_on_back)(void);
static void (*s_on_step)(int index, int dir);
static void (*s_navigate)(int index, int dir);
static EXT_RAM_BSS_ATTR bool s_stepper[LS_PICKER_MAX];

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
static tui_rect s_hit_dec[ROWS_MAX], s_hit_inc[ROWS_MAX];
static tui_rect s_hit_prev, s_hit_next, s_hit_close, s_hit_back;

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
    s_note[0] = 0;
    s_keep_open=false;
    s_done = on_done;
    s_preview = NULL;
    s_on_back = NULL;
    s_header = NULL;
    s_on_step = NULL;
    s_navigate = NULL;
    memset(s_stepper, 0, sizeof(s_stepper));
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
    /* A translation is already one byte a cell (ls_i18n.h) and is kept as
       it is; anything else is folded. */
    const char *tl = ls_tr(label), *td = ls_tr(detail);
    snprintf(s_label[s_n], LS_PICKER_TEXT, "%s", tl ? tl : "");
    snprintf(s_detail[s_n], LS_PICKER_DETAIL, "%s", td ? td : "");
    /* Place and node names can be UTF-8. Folded once here, the rows line up
       and typing "Lodz" on the keyboard finds Łódź. */
    if (tl == label) tui_utf8_fold_str(s_label[s_n]);
    if (td == detail) tui_utf8_fold_str(s_detail[s_n]);
    s_n++;
    refilter();
    return true;
}

void ls_picker_empty_reason(const char *why)
{
    snprintf(s_why, sizeof(s_why), "%s", why ? why : "");
}

void ls_picker_note(const char *text)
{
    snprintf(s_note, sizeof(s_note), "%s", text ? text : "");
}

void ls_picker_select(int index)
{
    for (int i = 0; i < s_shown; i++)
        if (s_order[i] == index) { s_cur = i; return; }
}

void ls_picker_set_detail(int index, const char *detail)
{
    if (index < 0 || index >= s_n) return;
    snprintf(s_detail[index], LS_PICKER_DETAIL, "%s", detail ? ls_tr(detail) : "");
}

void ls_picker_back(void (*on_back)(void)) { s_on_back = on_back; }

void ls_picker_stepper(int index, void (*on_step)(int index, int dir))
{
    if (index < 0 || index >= LS_PICKER_MAX) return;
    s_stepper[index] = on_step != NULL;
    if (on_step) s_on_step = on_step;
}

void ls_picker_navigation(void (*navigate)(int index, int dir)) { s_navigate=navigate; }

void ls_picker_close(void) { s_navigate=NULL; s_open = false; s_done = NULL; s_on_back = NULL; s_on_step = NULL; }
void ls_picker_keep_open(void) { s_keep_open=true; }

static void go_back(void)
{
    void (*cb)(void) = s_on_back;
    ls_picker_close();
    if (cb) cb();
}

static void step(int oi, int dir)
{
    if (oi < 0 || oi >= s_shown) return;
    const int idx = s_order[oi];
    if (s_stepper[idx] && s_on_step) s_on_step(idx, dir);
}
bool ls_picker_active(void) { return s_open; }
bool ls_picker_is(ls_picker_done_t on_done) { return s_open && on_done && s_done == on_done; }

static void accept(void)
{
    if (s_cur < 0 || s_cur >= s_shown) { ls_picker_close(); return; }
    const int idx = s_order[s_cur];
    const ls_picker_done_t cb = s_done;
    if(!s_keep_open) ls_picker_close();
    if (cb) cb(idx);
}

/* ------------------------------------------------------------------ draw -- */

/* A row is five cells tall, which is a decision and not a default. */

#define ROW_H 5

#define ROW_STEP (ROW_H - 1)

static int rows_visible(int body_h)
{
    int n = (body_h - 1) / (s_header ? 6 : ROW_STEP);
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
    tui_rect hole;
    if(s_header && ls_tui_cutout(&hole) && hole.x>a.x+a.w/2 &&
       a.y<hole.y+hole.h && a.y+a.h>hole.y && a.x+a.w>hole.x)
        a.w=hole.x-a.x-1;
    const int idx = s_order[oi];
    const uint8_t hue = sel ? (TUI_GREEN | TUI_BRIGHT) : TUI_CYAN;
    const uint8_t text = A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);

    ls_panel_box(sf, a, NULL, hue);
    tui_rect f = tui_rect_make(a.x + 1, a.y + 1, a.w - 2, a.h - 2);
    if (f.w <= 0 || f.h <= 0) return;
    ls_fill_dither(sf, f, sel ? LS_DITHER_MEDIUM : LS_DITHER_LIGHT, hue);

    if(s_header && s_stepper[idx]) {
        put_over(sf,f,f.x+1,f.y,s_label[idx],text);
        f.y++;f.h--; /* label above, three full rows of filled level keys */
    } else put_over(sf, f, f.x + 1, f.y + (f.h-1)/2, s_label[idx], text);

    /* A level: < and > as two solid keys the full height of the row at its
       right end, the value between them. */
    if (slot < ROWS_MAX) s_hit_dec[slot].w = s_hit_inc[slot].w = 0;
    if (s_stepper[idx] && f.w >= 30) {
        /* Both art modes use large keys; headers need wider numeric values. */
        const int kw = (s_header || s_preview) ? 8 : 5;
        /* The value's box grows to the value, up to what the keys and the
           label leave: a name (a detector rule, a profile) cut to eight
           letters could not be told from the next one. */
        int vw = s_header ? 10 : s_preview ? 6 : 12;
        const int want = (int)strlen(s_detail[idx]) + 2;
        const int room = f.w - 2 * kw - (s_header ? 1 : (int)strlen(s_label[idx]) + 3);
        if (want > vw) vw = want < room ? want : room > vw ? room : vw;
        const uint8_t key = A(TUI_BLACK, TUI_CYAN | TUI_BRIGHT);
        const tui_rect inc = tui_rect_make(f.x + f.w - kw, f.y, kw, f.h);
        const tui_rect dec = tui_rect_make(inc.x - vw - kw, f.y, kw, f.h);
        tui_fill(sf, inc, ' ', key);
        tui_fill(sf, dec, ' ', key);
        tui_put_char(sf, f, inc.x + kw / 2, f.y + (f.h-1)/2, '>', key);
        tui_put_char(sf, f, dec.x + kw / 2, f.y + (f.h-1)/2, '<', key);
        tui_fill(sf, tui_rect_make(dec.x + kw, f.y, vw, f.h), ' ', A(TUI_WHITE, TUI_BLACK));
        char v[LS_PICKER_DETAIL];
        snprintf(v, sizeof(v), "%.*s", vw - 2, s_detail[idx]);
        tui_put_str(sf, f, dec.x + kw + (vw - (int)strlen(v)) / 2, f.y + (f.h-1)/2, v,
                    A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
        if (slot < ROWS_MAX) {
            /* The whole row's height, a column either side: a thumb, not a cell. */
            s_hit_dec[slot] = tui_rect_make(dec.x - 1, a.y, kw + 1, a.h);
            s_hit_inc[slot] = tui_rect_make(inc.x, a.y, kw + 1, a.h);
            s_hit_row[slot] = a;
            s_hit_row_at[slot] = oi;
            if (slot + 1 > s_hit_rows) s_hit_rows = slot + 1;
        }
        return;
    }

    /* What is left after the label and a space between them. A detail
       longer than that is cut to fit rather than dropped: half a
       sentence still says more than none, and the label - the part
       that has to be right - is never touched. */
    int dn = (int)strlen(s_detail[idx]);
    const int room = f.w - 2 - (int)strlen(s_label[idx]) - 2;
    char cut[LS_PICKER_DETAIL];
    snprintf(cut, sizeof(cut), "%s", s_detail[idx]);
    if (dn > room) {
        /* Cut at a word, not in the middle of one. */
        dn = room > 0 ? room : 0;
        int sp = dn;
        while (sp > 0 && cut[sp] != ' ') sp--;
        if (sp > dn / 2) dn = sp;
        while (dn > 0 && cut[dn - 1] == ' ') dn--;
    }
    if (dn > 0) {
        cut[dn] = 0;
        put_over(sf, f, f.x + f.w - 1 - dn, f.y + (f.h-1)/2, cut,
                 A(TUI_WHITE, TUI_BLACK));
    }

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

/* THE DESK FORM: a popup menu over the app, not a page of thumb rows.

   One row an item: the label, the detail right-aligned, the selection bar
   in blue. A level row carries its value between two small keys. The keys
   along the bottom are the touch form's (BACK, PAGE UP/DN, CLOSE) at one
   row. Typing still filters; every rect drawn is the rect hit. */
static void draw_compact(tui_surface *sf, tui_rect area)
{
    const uint8_t text = A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    const uint8_t dim = A(DIM_FG, TUI_BLACK);
    const uint8_t sel = A(TUI_WHITE | TUI_BRIGHT, TUI_BLUE);
    const uint8_t sel_dim = A(TUI_YELLOW | TUI_BRIGHT, TUI_BLUE);

    /* As wide as the longest row asks, inside the window. */
    int want = (int)strlen(s_title) + 8;
    for (int i = 0; i < s_shown; i++) {
        const int idx = s_order[i];
        int w = (int)strlen(s_label[idx]) + (int)strlen(s_detail[idx]) + 6;
        if (s_stepper[idx]) w += 8;
        if (w > want) want = w;
    }
    if (want < 40) want = 40;
    const int note_rows = s_note[0] && s_flen == 0 ? 1 : 0;
    const int filt_rows = s_flen > 0 ? 1 : 0;
    const bool side = s_header && area.w >= want + 4 + 38;
    const bool prev = s_preview && area.w >= want + 4 + (side ? 38 : 0) + 44;
    int w = want + (side ? 38 : 0) + (prev ? 44 : 0);
    if (w > area.w - 2) w = area.w - 2;
    int rows = s_shown > 0 ? s_shown : 1;
    const int chrome = 4 + note_rows + filt_rows;      /* frames, gap, keys */
    if (rows > area.h - 2 - chrome) rows = area.h - 2 - chrome;
    if (rows > ROWS_MAX) rows = ROWS_MAX;
    if (rows < 1) return;
    int h = rows + chrome;
    if ((side || prev) && h < 16 && area.h - 2 >= 16) h = 16;
    const tui_rect box = tui_rect_make(area.x + (area.w - w) / 2,
                                       area.y + (area.h - h) / 3, w, h);
    ls_dialog1_frame(sf, box, s_title, TUI_CYAN);
    tui_rect in = tui_rect_make(box.x + 1, box.y + 1, box.w - 2, box.h - 2);
    if (side) {
        s_header(sf, tui_rect_make(in.x, in.y, 36, in.h));
        in.x += 38; in.w -= 38;
    }
    if (prev) {
        s_preview(sf, tui_rect_make(in.x + in.w - 43, in.y, 43, in.h));
        in.w -= 44;
    }

    int y = in.y;
    if (filt_rows) {
        char line[FILTER_MAX + 8];
        snprintf(line, sizeof(line), "/%s", s_filter);
        tui_put_str(sf, in, in.x + 1, y, line, A(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
        char count[24];
        snprintf(count, sizeof(count), ls_tr("%d of %d"), s_shown, s_n);
        tui_put_str(sf, in, in.x + in.w - 1 - (int)strlen(count), y, count, dim);
        y++;
    }
    if (note_rows) {
        char line[sizeof(s_note)];
        snprintf(line, sizeof(line), "%.*s", in.w - 2 > 0 ? in.w - 2 : 0, s_note);
        tui_put_str(sf, in, in.x + 1, y++, line, A(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
    }

    s_nvis = rows;
    if (s_cur < s_top) s_top = s_cur;
    if (s_cur >= s_top + rows) s_top = s_cur - rows + 1;
    if (s_top > s_shown - rows) s_top = s_shown - rows;
    if (s_top < 0) s_top = 0;

    if (s_shown <= 0) {
        tui_put_str(sf, in, in.x + 1, y, s_why[0] ? s_why : ls_tr("nothing here"), dim);
    }
    for (int i = 0; i < rows && s_top + i < s_shown; i++) {
        const int oi = s_top + i, idx = s_order[oi];
        const bool on = oi == s_cur;
        const tui_rect row = tui_rect_make(in.x, y + i, in.w, 1);
        if (on) tui_fill(sf, row, ' ', sel);
        tui_put_str(sf, row, row.x + 1, row.y, s_label[idx], on ? sel : text);
        s_hit_dec[i].w = s_hit_inc[i].w = 0;
        int right = row.x + row.w - 1;
        if (s_stepper[idx]) {
            /* [<] value [>] at the right end: the keys step, the row stays. */
            char v[LS_PICKER_DETAIL];
            snprintf(v, sizeof(v), "%.14s", s_detail[idx]);
            const int vw = 16;
            const tui_rect inc = tui_rect_make(right - 3, row.y, 3, 1);
            const tui_rect dec = tui_rect_make(inc.x - vw - 3, row.y, 3, 1);
            const uint8_t cap = A(TUI_BLACK, TUI_WHITE);
            tui_put_str(sf, row, dec.x, row.y, " < ", cap);
            tui_put_str(sf, row, inc.x, row.y, " > ", cap);
            tui_put_str(sf, row, dec.x + 3 + (vw - (int)strlen(v)) / 2, row.y, v,
                        on ? sel_dim : A(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
            s_hit_dec[i] = dec;
            s_hit_inc[i] = inc;
        } else if (s_detail[idx][0]) {
            const int room = row.w - 4 - (int)strlen(s_label[idx]);
            char d[LS_PICKER_DETAIL];
            snprintf(d, sizeof(d), "%.*s", room > 0 ? room : 0, s_detail[idx]);
            tui_put_str(sf, row, right - (int)strlen(d), row.y, d, on ? sel_dim : A(TUI_CYAN, TUI_BLACK));
        }
        s_hit_row[i] = row;
        s_hit_row_at[i] = oi;
        s_hit_rows = i + 1;
    }

    const bool paged = s_shown > rows;
    if (paged) {
        char pos[28];
        int last = s_top + rows;
        if (last > s_shown) last = s_shown;
        snprintf(pos, sizeof(pos), ls_tr(" %d-%d of %d "), s_top + 1, last, s_shown);
        tui_put_str(sf, box, box.x + box.w - 2 - (int)strlen(pos), box.y, pos, A(TUI_CYAN, TUI_BLACK));
    }
    /* The keys, one row, on the bottom line inside the frame. */
    const int ky = box.y + box.h - 2;
    int kx = in.x + 1;
    if (s_on_back) {
        const ls_btn_t b = { .label = "< BACK", .key = 0 };
        const int kw = ls_key1_draw(sf, box, kx, ky, &b, false);
        s_hit_back = tui_rect_make(kx, ky, kw, 1);
        kx += kw + 1;
    }
    if (paged) {
        const ls_btn_t up = { .label = "PAGE UP" }, dn = { .label = "PAGE DN" };
        int kw = ls_key1_draw(sf, box, kx, ky, &up, false);
        s_hit_prev = tui_rect_make(kx, ky, kw, 1);
        kx += kw + 1;
        kw = ls_key1_draw(sf, box, kx, ky, &dn, false);
        s_hit_next = tui_rect_make(kx, ky, kw, 1);
        kx += kw + 1;
    }
    const ls_btn_t close = { .label = "CLOSE" };
    const int cw = ls_key1_width(&close);
    const int cx = box.x + box.w - 2 - cw;
    ls_key1_draw(sf, box, cx, ky, &close, false);
    s_hit_close = tui_rect_make(cx, ky, cw, 1);
    if (cx - kx > 24)
        tui_put_str(sf, box, kx + 1, ky, ls_tr("type to filter"), dim);
}

void ls_picker_draw(tui_surface *sf, tui_rect area)
{
    if (!s_open || area.w < 16 || area.h < 12) return;
    if (ls_tui_compact()) {
        s_hit_rows = 0;
        s_hit_prev.w = s_hit_next.w = s_hit_close.w = s_hit_back.w = 0;
        draw_compact(sf, area);
        return;
    }

    const uint8_t dim = A(DIM_FG, TUI_BLACK);
    s_hit_rows = 0;
    s_hit_prev.w = s_hit_next.w = s_hit_close.w = s_hit_back.w = 0;

    tui_fill(sf, area, ' ', A(TUI_WHITE, TUI_BLACK));
    ls_panel_box(sf, area, s_title, TUI_CYAN);

    bool side_header=s_header && area.w>=90 && s_shown>1;
    if(side_header) {
        s_header(sf,tui_rect_make(area.x+1,area.y+2,36,area.h-3));
        area.x+=38;area.w-=38;
    }
    /* --- what is being filtered on, when anything is ------------------- */
    int y = area.y + 1;
    if(s_header && !side_header && area.h>=24) {
        int hh=area.h-8-s_shown*8;
        if(hh<7)hh=7;
        if(hh>area.h-18)hh=area.h-18;
        s_header(sf,tui_rect_make(area.x+1,y,area.w-2,hh));
        y+=hh+1;
    }
    if (s_flen > 0) {
        char line[FILTER_MAX + 8];
        snprintf(line, sizeof(line), "/%s", s_filter);
        tui_put_str(sf, area, area.x + 2, y, line,
                    A(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
        char count[24];
        snprintf(count, sizeof(count), ls_tr("%d of %d"), s_shown, s_n);
        const int cn = (int)strlen(count);
        if (area.w - 2 - cn > (int)strlen(line) + 3)
            tui_put_str(sf, area, area.x + area.w - 2 - cn, y, count, dim);
        y++;
    }

    /* --- the note, at most two lines, broken at a space ---------------- */
    if (s_note[0] && s_flen == 0) {
        const tui_rect inner = tui_rect_make(area.x + 1, area.y + 1, area.w - 2, area.h - 2);
        const int w = area.w - 4;
        const int n = (int)strlen(s_note);
        int cut = n;
        if (n > w) {
            cut = w;
            while (cut > 0 && s_note[cut] != ' ') cut--;
            if (cut <= 0) cut = w;
        }
        char line[sizeof(s_note)];
        snprintf(line, sizeof(line), "%.*s", cut, s_note);
        const uint8_t warn = A(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
        tui_put_str(sf, inner, area.x + 2, y++, line, warn);
        if (cut < n)
            tui_put_str(sf, inner, area.x + 2, y++, s_note + cut + (s_note[cut] == ' '), warn);
    }

    /* --- the buttons, anchored to the bottom --------------------------- */
    const int btn_h = 4;
    const int btn_y = area.y + area.h - 1 - btn_h;
    tui_rect body = tui_rect_make(area.x + 1, y, area.w - 2, btn_y - y - 1);

    if (s_preview) {
        tui_rect preview = body;
        if (body.w >= 90) {
            body.w -= 44;
            preview.x = body.x + body.w + 1;
            preview.w = 43;
            s_preview(sf, preview);
        } else if (body.h >= 40) {
            body.h -= 24;
            preview.y = body.y + body.h + 1;
            preview.h = 23;
            s_preview(sf, preview);
        }
    }
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
        int step = s_header ? 6 : ROW_STEP;
        if (s_shown < nvis) {
            step = (body.h - 1) / s_shown;
            if (step > 8) step = 8;
            if (step < (s_header?6:ROW_STEP)) step = s_header?6:ROW_STEP;
        }
        for (int i = 0; i < nvis && s_top + i < s_shown; i++)
            draw_row(sf, tui_rect_make(body.x, body.y + i * step,
                                       body.w, step + 1),
                     i, s_top + i, (s_top + i) == s_cur);
    }

    /* Paging exists for the thumb. The arrow keys are the same gesture for
       whoever has a keyboard, and neither is drawn when everything fits. */
    const bool paged = s_shown > nvis;

    if (paged) {
        char pos[28];
        int last = s_top + nvis;
        if (last > s_shown) last = s_shown;
        snprintf(pos, sizeof(pos), ls_tr(" %d-%d of %d "), s_top + 1, last, s_shown);
        const int pn = (int)strlen(pos);
        /* Right end of the top border, clear of the title on the left. */
        if (area.w - 2 - pn > (int)strlen(s_title) + 4)
            tui_put_str(sf, area, area.x + area.w - 2 - pn, area.y, pos,
                        A(TUI_CYAN, TUI_BLACK));
    }
    /* Named for what they do. PAGE UP and PAGE DN move a page, and a button
       that says UP has to mean the thing the UP key means. BACK, on a list
       opened from another, comes first: it is the way out most wanted. */
    const char *names[4];
    tui_rect *hits[4];
    int n = 0;
    if (s_on_back) { names[n] = "< BACK"; hits[n++] = &s_hit_back; }
    if (paged) {
        names[n] = "PAGE UP"; hits[n++] = &s_hit_prev;
        names[n] = "PAGE DN"; hits[n++] = &s_hit_next;
    }
    names[n] = "CLOSE"; hits[n++] = &s_hit_close;
    const int bw = area.w - 2, each = bw / n;
    for (int i = 0; i < n; i++) {
        const int x = area.x + 1 + i * each;
        const int w = i == n - 1 ? area.x + 1 + bw - x : each - 1;
        draw_button(sf, tui_rect_make(x, btn_y, w, btn_h), names[i],
                    hits[i] == &s_hit_back, hits[i]);
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
    /* On a level, LEFT and RIGHT are its < and >; elsewhere they page. */
    const bool level = s_cur >= 0 && s_cur < s_shown && s_stepper[s_order[s_cur]];
    switch (key) {
    case LS_TK_UP:    move(-1); return true;
    case LS_TK_DOWN:  move(1);  return true;
    case LS_TK_LEFT:  if (level) step(s_cur, -1); else if(s_navigate) s_navigate(s_order[s_cur],-1); else page(-1); return true;
    case LS_TK_RIGHT: if (level) step(s_cur, 1);  else if(s_navigate) s_navigate(s_order[s_cur],1); else page(1); return true;
    case LS_TK_ENTER: accept();  return true;
    case LS_TK_ESC:   go_back(); return true;
    case LS_TK_BACKSPACE:
        if (s_flen > 0) { s_filter[--s_flen] = 0; refilter(); }
        else go_back();
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
    if (in(s_hit_back, col, row))  { go_back(); return true; }
    if (in(s_hit_prev, col, row))  { page(-1); return true; }
    if (in(s_hit_next, col, row))  { page(1);  return true; }

    for (int i = 0; i < s_hit_rows; i++) {
        if (!in(s_hit_row[i], col, row)) continue;
        /* A level's arrows step it and leave the list up. */
        const int dir = in(s_hit_dec[i], col, row) ? -1
                      : in(s_hit_inc[i], col, row) ? 1 : 0;
        if (dir) {
            s_cur = s_hit_row_at[i];
            step(s_cur, dir);
            return true;
        }
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
