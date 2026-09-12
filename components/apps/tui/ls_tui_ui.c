/* See ls_tui_ui.h for why buttons are blocks of cells. */
#include "ls_tui_ui.h"

#include <stdio.h>
#include <string.h>

#include "ls_icons.h"

#define MAX_HITS 24

typedef struct { int16_t x0, y0, x1, y1; } hit_t;

#define BTN_SLOTS 3
static hit_t s_btn_hit[BTN_SLOTS][MAX_HITS];
static int   s_btn_n[BTN_SLOTS];

static hit_t s_tile_hit[MAX_HITS];
static int   s_tile_n, s_tile_cols, s_tile_rows;

static void hit_clear(hit_t *h, int *n) { *n = 0; (void)h; }

static int hit_find(const hit_t *h, int n, int col, int row)
{
    for (int i = 0; i < n; i++)
        if (col >= h[i].x0 && col <= h[i].x1 && row >= h[i].y0 && row <= h[i].y1)
            return i;
    return -1;
}

/* ------------------------------------------------------------------ button */

/* A button is a filled block, not a bracketed label.

   `[SPLIT]` reads as text and invites a tap on the word; a filled rectangle
   reads as a target and invites a tap anywhere inside it. On a panel where
   the difference between hitting and missing is four millimetres that is not
   decoration, it is the whole affordance - and it costs the same number of
   cells either way, because the background was going to be painted regardless. */
void ls_btn_bar_slot(tui_surface *sf, tui_rect bar, const ls_btn_t *btn, int n,
                     int focus, int slot)
{
    if (slot < 0 || slot >= BTN_SLOTS) slot = 0;
    hit_clear(s_btn_hit[slot], &s_btn_n[slot]);
    if (!sf || n <= 0 || bar.w < 6 || bar.h < 1) return;

    /* Rows follow the height the caller gave us, columns follow from that.
       Portrait passes two or four rows and gets fat targets without this
       file knowing which orientation it is in. */
    int per_row = n;
    int rows = 1;
    while (bar.h >= rows * 2 && bar.w / per_row < 8 && per_row > 1) {
        rows++;
        per_row = (n + rows - 1) / rows;
    }
    if (rows > bar.h) rows = bar.h;
    if (rows < 1) rows = 1;
    per_row = (n + rows - 1) / rows;
    if (per_row < 1) per_row = 1;

    /* A button is as tall as it needs to be, not as tall as the rect. */

    int row_h = bar.h / rows;
    if (row_h > 5) row_h = 5;
    /* A button stops getting wider. */

#define BTN_MAX_W 16
    int cell_w = bar.w / per_row;
    int x_pad = 0;
    if (cell_w > BTN_MAX_W) {
        cell_w = BTN_MAX_W;
        x_pad = (bar.w - cell_w * per_row) / 2;
    }
    if (cell_w < 3 || row_h < 1) return;

    for (int i = 0; i < n && i < MAX_HITS; i++) {
        const int r = i / per_row, c = i % per_row;
        if (r >= rows) break;

        const int x0 = bar.x + x_pad + c * cell_w;
        const int y0 = bar.y + r * row_h;
        /* The last button on a row absorbs the rounding, so the bar always
           reaches the right edge and there is no dead strip to mis-tap into. */
        const int w = (c == per_row - 1 && !x_pad)
                      ? (bar.x + bar.w - x0) : cell_w;
        const int h = row_h;

        /* A dithered field and a border, not a slab of colour. */

        const bool sel = (i == focus);
        uint8_t hue;
        int density;
        if (btn[i].dim)      { hue = TUI_BLACK | TUI_BRIGHT; density = LS_DITHER_LIGHT; }
        else if (btn[i].on)  { hue = TUI_GREEN | TUI_BRIGHT; density = LS_DITHER_HEAVY; }
        else if (sel)        { hue = TUI_CYAN | TUI_BRIGHT;  density = LS_DITHER_MEDIUM; }
        /* Cyan, not blue. Blue was the last of the old palette left
           on a control, and it put a navy field under every page button
           while the panel around it was cyan - which is the same "belongs to
           a different program" the navigation strip had. */
        else                 { hue = TUI_CYAN;                density = LS_DITHER_LIGHT; }

        /* Text on black, so the glyph is not fighting the texture. The one
           exception is a dim button, whose whole point is to recede. */
        /* An unavailable control still has to say what it is - a
           label you cannot read is not a disabled button, it is a blank. */
        const uint8_t face = TUI_ATTR(btn[i].dim ? LS_DIM_FG
                                                 : (TUI_WHITE | TUI_BRIGHT),
                                      TUI_BLACK);

        tui_rect box = tui_rect_make(x0, y0, w - 1, h);
        ls_fill_dither(sf, box, density, hue);

        /* A border when there is height for one. Two rows is a strip and
           gets none; three or more is a key and looks like one. */
        if (h >= 3) {
            const uint8_t edge = TUI_ATTR(hue, TUI_BLACK);
            for (int c = 0; c < box.w; c++) {
                tui_put_char(sf, box, box.x + c, box.y, '-', edge);
                tui_put_char(sf, box, box.x + c, box.y + h - 1, '-', edge);
            }
            for (int r = 1; r < h - 1; r++) {
                tui_put_char(sf, box, box.x, box.y + r, '|', edge);
                tui_put_char(sf, box, box.x + box.w - 1, box.y + r, '|', edge);
            }
            tui_put_char(sf, box, box.x, box.y, '+', edge);
            tui_put_char(sf, box, box.x + box.w - 1, box.y, '+', edge);
            tui_put_char(sf, box, box.x, box.y + h - 1, '+', edge);
            tui_put_char(sf, box, box.x + box.w - 1, box.y + h - 1, '+', edge);
        }

        const char *lab = btn[i].label ? btn[i].label : "";
        int lw = (int)strlen(lab);
        if (lw > box.w) lw = box.w;
        char cut[24];
        snprintf(cut, sizeof(cut), "%.*s", lw, lab);

        const int lines = (h > 1 && btn[i].value) ? 2 : 1;
        const int ly = box.y + (h - lines) / 2;
        const int lx = box.x + (box.w - lw) / 2;
        /* A space either side: the field under the lettering is texture, and
           a word butting into it loses its first and last letter. */
        if (lx - 1 > box.x) tui_put_char(sf, box, lx - 1, ly, ' ', face);
        if (lx + lw < box.x + box.w - 1)
            tui_put_char(sf, box, lx + lw, ly, ' ', face);
        tui_put_str(sf, box, lx, ly, cut, face);

        if (lines == 2) {
            int vw = (int)strlen(btn[i].value);
            if (vw > box.w) vw = box.w;
            char vc[24];
            snprintf(vc, sizeof(vc), "%.*s", vw, btn[i].value);
            const uint8_t vattr = TUI_ATTR(btn[i].dim ? LS_DIM_FG
                                                      : (TUI_YELLOW | TUI_BRIGHT),
                                           TUI_BLACK);
            const int vx = box.x + (box.w - vw) / 2;
            if (vx - 1 > box.x) tui_put_char(sf, box, vx - 1, ly + 1, ' ', vattr);
            if (vx + vw < box.x + box.w - 1)
                tui_put_char(sf, box, vx + vw, ly + 1, ' ', vattr);
            tui_put_str(sf, box, vx, ly + 1, vc, vattr);
        }

        if (btn[i].key && box.w >= lw + 3)
            tui_put_char(sf, box, lx + lw + 1, ly, btn[i].key,
                         TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));

        hit_t *hit = &s_btn_hit[slot][s_btn_n[slot]];
        hit->x0 = (int16_t)box.x;
        hit->y0 = (int16_t)box.y;
        hit->x1 = (int16_t)(box.x + box.w - 1);
        hit->y1 = (int16_t)(box.y + h - 1);
        s_btn_n[slot]++;
    }
}

void ls_btn_bar(tui_surface *sf, tui_rect bar, const ls_btn_t *btn, int n,
                int focus)
{
    ls_btn_bar_slot(sf, bar, btn, n, focus, LS_BTN_SLOT_SCREEN);
}

int ls_btn_hit_slot(int col, int row, int slot)
{
    if (slot < 0 || slot >= BTN_SLOTS) return -1;
    return hit_find(s_btn_hit[slot], s_btn_n[slot], col, row);
}

int ls_btn_hit(int col, int row)
{
    return ls_btn_hit_slot(col, row, LS_BTN_SLOT_SCREEN);
}

int ls_btn_key(char ch, const ls_btn_t *btn, int n)
{
    if (!ch || !btn) return -1;
    char lo = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
    for (int i = 0; i < n; i++) {
        char k = btn[i].key;
        if (!k) continue;
        if (k >= 'A' && k <= 'Z') k = (char)(k + 32);
        if (k == lo) return i;
    }
    return -1;
}

/* ------------------------------------------------------------------- tiles */

void ls_tile_grid(tui_surface *sf, tui_rect area, const ls_tile_t *tile,
                  int n, int sel)
{
    hit_clear(s_tile_hit, &s_tile_n);
    s_tile_cols = s_tile_rows = 0;
    if (!sf || n <= 0 || area.w < 12 || area.h < 4) return;

    /* A tile is 14 columns and 7 rows at its most comfortable - an
       icon, a name and a subtitle with air around them. Both are minimums,
       not literals: the count comes from dividing the rect, so a 48-column
       portrait grid gets three across and a 115-column landscape one gets
       eight, and neither is written down anywhere. */
    int cols = area.w / 15;
    if (cols < 2) cols = 2;
    if (cols > 6) cols = 6;
    if (cols > n) cols = n;
    int rows = (n + cols - 1) / cols;

    /* A tile stops getting taller. */

#define TILE_MAX_H 16

    /* THE GRID IS CHOSEN SO THE ART FITS, not cropped to fit the grid. */

    {
        /* Two borders, the art, the name, and the row of air that separates
           one tile from the next. */
        const int want_h = 2 + LS_ICON_ROWS + 1 + 1;
        const int fits_rows = area.h / want_h;
        if (fits_rows >= 1 && rows > fits_rows) {
            const int want_cols = (n + fits_rows - 1) / fits_rows;
            /* The art has a width as well as a height, and a tile too narrow
               for it is the same fault turned ninety degrees. */
            if (want_cols <= MAX_HITS && area.w / want_cols >= LS_ICON_COLS + 3) {
                cols = want_cols;
                rows = fits_rows;
            }
        }
    }

    int tw = area.w / cols;
    int th = area.h / rows;
    if (th > TILE_MAX_H) th = TILE_MAX_H;
    if (th < 4) {

        rows = area.h / 4;
        if (rows < 1) rows = 1;
        cols = (n + rows - 1) / rows;
        if (cols < 1) cols = 1;
        tw = area.w / cols;
        th = area.h / rows;
    }
    if (tw < 8 || th < 3) return;

    /* Whatever the cap leaves over goes at BOTH ends. */

    const int pad_x = (area.w - cols * tw) / 2;
    const int pad_y = (area.h - rows * th) / 2;

    s_tile_cols = cols;
    s_tile_rows = rows;

    for (int i = 0; i < n && i < MAX_HITS; i++) {
        const int c = i % cols, r = i / cols;
        if (r >= rows) break;
        const int x0 = area.x + pad_x + c * tw;
        const int y0 = area.y + pad_y + r * th;
        const int w = tw - 1, h = th - 1;
        if (w < 6 || h < 3) continue;

        const bool sel_now = (i == sel);
        tui_rect box = tui_rect_make(x0, y0, w, h);

        uint8_t hue = tile[i].hue ? tile[i].hue : (uint8_t)(TUI_CYAN);
        uint8_t frame = TUI_ATTR(sel_now ? (hue | TUI_BRIGHT) : hue, TUI_BLACK);

        tui_fill(sf, box, ' ', TUI_ATTR(TUI_WHITE, TUI_BLACK));
        tui_box(sf, box, NULL, frame);

        if (sel_now)
            tui_fill(sf, tui_rect_make(box.x + 1, box.y + 1, box.w - 2, box.h - 2),
                     ' ', TUI_ATTR(TUI_BLACK, hue));

        const uint8_t on_tile = sel_now ? TUI_ATTR(TUI_BLACK, hue)
                                        : TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK);
        const uint8_t name_at = sel_now ? TUI_ATTR(TUI_BLACK, hue)
                                        : TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
        /* The subtitle under every tile on the directory - the
           single most-looked-at grey in the interface. */
        const uint8_t sub_at  = sel_now ? TUI_ATTR(TUI_BLACK, hue)
                                        : LS_ATTR_DIM;

        /* The contents sit in the middle of the tile. */

        int text_h = (tile[i].name ? 1 : 0) + (tile[i].sub ? 1 : 0);
        const int interior = h - 2;              /* rows between the borders */
        if (interior < LS_ICON_ROWS + text_h && text_h > 1) text_h = 1;
        int icon_h = interior - text_h;
        if (icon_h < 0) icon_h = 0;
        if (icon_h > LS_ICON_ROWS) icon_h = LS_ICON_ROWS;

        int top = box.y + 1 + (interior - (icon_h + text_h)) / 2;
        if (top < box.y + 1) top = box.y + 1;

        int text_y = top;
        if (icon_h > 0) {
            const tui_rect icon_clip = tui_rect_make(box.x, top, box.w, icon_h);
            ls_icon_draw(sf, icon_clip, box.x + (box.w - LS_ICON_COLS) / 2,
                        top, tile[i].icon, on_tile);
            text_y = top + icon_h;
        }

        if (tile[i].name) {
            int nw = (int)strlen(tile[i].name);
            if (nw > box.w - 2) nw = box.w - 2;
            char cut[24];
            snprintf(cut, sizeof(cut), "%.*s", nw, tile[i].name);
            tui_put_str(sf, box, box.x + (box.w - nw) / 2, text_y, cut, name_at);
        }
        /* text_h == 1 means the subtitle was the thing dropped to make room
           - tile[i].sub is still non-NULL, but no row was budgeted for it,
           so drawing it anyway would land on whatever the icon or the
           border owns there. */
        if (tile[i].sub && text_h >= 2 && text_y + 1 < box.y + box.h) {
            int sw = (int)strlen(tile[i].sub);
            if (sw > box.w - 2) sw = box.w - 2;
            char cut[24];
            snprintf(cut, sizeof(cut), "%.*s", sw, tile[i].sub);
            tui_put_str(sf, box, box.x + (box.w - sw) / 2, text_y + 1, cut, sub_at);
        }

        /* A live app says so in words, in its top border. */

        if (tile[i].live && box.w >= 10)
            tui_put_str(sf, box, box.x + box.w - 6, box.y, "LIVE",
                        sel_now ? TUI_ATTR(TUI_BLACK, TUI_GREEN | TUI_BRIGHT)
                                : TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
        else if (tile[i].live)
            tui_put_char(sf, box, box.x + box.w - 2, box.y, LS_TUI_BLOCK_FULL,
                         TUI_ATTR(TUI_GREEN | TUI_BRIGHT,
                                  sel_now ? hue : TUI_BLACK));

        s_tile_hit[s_tile_n].x0 = (int16_t)box.x;
        s_tile_hit[s_tile_n].y0 = (int16_t)box.y;
        s_tile_hit[s_tile_n].x1 = (int16_t)(box.x + box.w);
        s_tile_hit[s_tile_n].y1 = (int16_t)(box.y + box.h);
        s_tile_n++;
    }
}

int ls_tile_hit(int col, int row) { return hit_find(s_tile_hit, s_tile_n, col, row); }

void ls_tile_shape(int *cols, int *rows)
{
    if (cols) *cols = s_tile_cols;
    if (rows) *rows = s_tile_rows;
}

/* --------------------------------------------------------------- fragments */

void ls_fill_dither(tui_surface *sf, tui_rect r, int density, uint8_t hue)
{
    const char g = (density >= LS_DITHER_HEAVY)  ? LS_TUI_SHADE_75
                 : (density == LS_DITHER_MEDIUM) ? LS_TUI_SHADE_50
                                                 : LS_TUI_SHADE_25;
    const uint8_t at = TUI_ATTR(hue, TUI_BLACK);
    for (int y = 0; y < r.h; y++)
        for (int x = 0; x < r.w; x++)
            tui_put_char(sf, r, r.x + x, r.y + y, g, at);
}

void ls_dither_label(tui_surface *sf, tui_rect r, int row, const char *text,
                     uint8_t attr)
{
    if (!text || !*text) return;
    int n = (int)strlen(text);
    if (n > r.w) n = r.w;
    const int x = r.x + (r.w - n) / 2;
    const int y = r.y + row;

    if (x - 1 >= r.x) tui_put_char(sf, r, x - 1, y, ' ', attr);
    if (x + n < r.x + r.w) tui_put_char(sf, r, x + n, y, ' ', attr);
    for (int i = 0; i < n; i++)
        tui_put_char(sf, r, x + i, y, text[i], attr);
}

void ls_panel_box(tui_surface *sf, tui_rect r, const char *title, uint8_t hue)
{
    tui_box(sf, r, NULL, TUI_ATTR(hue, TUI_BLACK));
    if (title && r.w > 6)
        tui_put_str(sf, r, r.x + 2, r.y, title,
                    TUI_ATTR(TUI_BLACK, hue | TUI_BRIGHT));
}

/* An instrument with nothing to show says so in a panel its own size, in the middle of the space. */

void ls_panel_notice(tui_surface *sf, tui_rect area, const char *title,
                     const char *reason, const char *remedy)
{
    if (!sf || area.w < 12 || area.h < 4) return;

    const int lines = (reason ? 1 : 0) + (remedy ? 1 : 0);
    int h = lines + 4;                      /* border, air, lines, air, border */
    if (h > area.h) h = area.h;

    int w = 0;
    if (reason && (int)strlen(reason) > w) w = (int)strlen(reason);
    if (remedy && (int)strlen(remedy) > w) w = (int)strlen(remedy);
    w += 6;
    if (title && (int)strlen(title) + 6 > w) w = (int)strlen(title) + 6;
    if (w > area.w) w = area.w;

    tui_rect box = tui_rect_make(area.x + (area.w - w) / 2,
                                 area.y + (area.h - h) / 2, w, h);
    ls_panel_box(sf, box, title, TUI_CYAN);

    int y = box.y + 2;
    if (reason) {
        const int n = (int)strlen(reason);
        tui_put_str(sf, box, box.x + (w - n) / 2, y, reason,
                    TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
        y++;
    }
    if (remedy) {
        const int n = (int)strlen(remedy);
        tui_put_str(sf, box, box.x + (w - n) / 2, y, remedy, LS_ATTR_DIM);
    }
}

void ls_kv(tui_surface *sf, tui_rect r, int row, const char *label,
           const char *value, uint8_t vattr)
{
    tui_put_str(sf, r, r.x + 2, r.y + row, label,
                TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    if (!value) return;
    int vw = (int)strlen(value);
    int x = r.x + r.w - 2 - vw;
    /* Right-aligned until it would collide with the label, then left-aligned
       at a fixed column. Two alignments is one more than ideal and still
       better than a value that overwrites its own label. */
    int min_x = r.x + 2 + (int)strlen(label) + 1;
    if (x < min_x) x = min_x;
    tui_put_str(sf, r, x, r.y + row, value, vattr);
}

void ls_bar(tui_surface *sf, tui_rect r, int row, int x, int w, float v)
{
    if (w <= 0) return;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    int lit = (int)(v * (float)w);
    for (int i = 0; i < w; i++) {
        uint8_t c = i < lit ? (i > w * 3 / 4 ? TUI_RED | TUI_BRIGHT
                             : i > w / 2     ? TUI_YELLOW | TUI_BRIGHT
                                             : TUI_GREEN | TUI_BRIGHT)
                            : (TUI_BLACK | TUI_BRIGHT);
        tui_put_char(sf, r, x + i, r.y + row,
                     i < lit ? LS_TUI_SHADE_FULL : LS_TUI_SHADE_25,
                     TUI_ATTR(c, TUI_BLACK));
    }
}
