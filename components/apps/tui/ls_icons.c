/* See ls_icons.h. */
#include "ls_icons.h"
#include <stdbool.h>
#include "esp_attr.h"

#define PX_W (LS_ICON_COLS * 2)
#define PX_H (LS_ICON_ROWS * 3)

typedef struct {
    uint8_t pal[16];            /* theme colour 0-15, 0x10/0x11 accent bright/dim, 0xFF clear */
    uint8_t dots[PX_W * PX_H / 2];
} ls_icon_packed_t;

#include "ls_icons_px.inc"

/* A built-in icon as dots, its accent in the tile's colour. */
static bool unpack(int icon, uint8_t attr, uint8_t out[PX_H][PX_W])
{
    if (icon <= 0 || icon >= LS_ICON__COUNT) return false;
    const ls_icon_packed_t *p = &PX[icon];
    int hue = TUI_ATTR_FG(attr) & 7;
    if (!hue) hue = TUI_ATTR_BG(attr) & 7;
    if (!hue) hue = TUI_WHITE;
    bool any = false;
    for (int k = 0; k < PX_W * PX_H; k++) {
        uint8_t c = p->pal[(p->dots[k / 2] >> ((k & 1) * 4)) & 15];
        if (c == 0x10) c = (uint8_t)(hue | TUI_BRIGHT);
        else if (c == 0x11) c = (uint8_t)hue;
        out[k / PX_W][k % PX_W] = c;
        any |= c != 0xFF;
    }
    return any;
}

/* App art lives in PSRAM: zero-initialised, so no initialiser here. */
static EXT_RAM_BSS_ATTR char s_art[LS_ICON_ART_MAX][LS_ICON_ROWS][LS_ICON_COLS];
static EXT_RAM_BSS_ATTR bool s_art_set[LS_ICON_ART_MAX];
static EXT_RAM_BSS_ATTR uint8_t s_px[LS_ICON_ART_MAX][LS_ICON_ROWS * 3][LS_ICON_COLS * 2];
static EXT_RAM_BSS_ATTR bool s_px_set[LS_ICON_ART_MAX];

static uint32_t s_px_gen;   /* bumped when card art changes: cached cells go stale */

int ls_icon_px(int slot, const uint8_t px[LS_ICON_ROWS * 3][LS_ICON_COLS * 2], int fallback)
{
    if (!px || slot < 0 || slot >= LS_ICON_ART_MAX) return fallback;
    bool any = false;
    for (int y = 0; y < LS_ICON_ROWS * 3; y++)
        for (int x = 0; x < LS_ICON_COLS * 2; x++) {
            s_px[slot][y][x] = px[y][x] > 15 ? 0xFF : px[y][x];
            any |= s_px[slot][y][x] != 0xFF;
        }
    s_px_set[slot] = any;
    s_art_set[slot] = false;
    s_px_gen++;
    return any ? LS_ICON_ART_FIRST + slot : fallback;
}

/* One icon as the cells it paints: ch 0 where no dot is lit. */
typedef struct { uint8_t ch, attr; } icon_cell_t;
typedef struct { int icon; uint8_t attr; uint32_t gen; bool used; icon_cell_t cell[LS_ICON_ROWS][LS_ICON_COLS]; } icon_cache_t;

static void icon_cells(icon_cell_t out[LS_ICON_ROWS][LS_ICON_COLS],
                       const uint8_t px[LS_ICON_ROWS * 3][LS_ICON_COLS * 2], uint8_t attr)
{
    const int ground = TUI_ATTR_BG(attr);
    for (int r = 0; r < LS_ICON_ROWS; r++)
        for (int c = 0; c < LS_ICON_COLS; c++) {
            out[r][c].ch = 0;
            int col[6], count[6] = { 0 };
            bool lit = false;
            for (int k = 0; k < 6; k++) {
                const uint8_t d = px[3 * r + k / 2][2 * c + (k & 1)];
                col[k] = d > 15 ? ground : d;
                lit |= d <= 15;
            }
            if (!lit) continue;
            for (int k = 0; k < 6; k++) for (int m = 0; m < 6; m++) count[k] += col[m] == col[k];
            int ink = 0;
            for (int k = 1; k < 6; k++)
                if (count[k] > count[ink] || (count[k] == count[ink] && col[k] > col[ink])) ink = k;
            int paper = -1;
            for (int k = 0; k < 6; k++)
                if (col[k] != col[ink] && (paper < 0 || count[k] > count[paper])) paper = k;
            const int fg = col[ink], bg = paper >= 0 ? col[paper] : ground;
            int bits = 0;
            for (int k = 0; k < 6; k++) if (col[k] == fg) bits |= 1 << k;
            out[r][c].ch = (uint8_t)(0xC0 | bits);
            out[r][c].attr = TUI_ATTR(fg, bg);
        }
}

static void icon_blit(tui_surface *sf, tui_rect clip, int x, int y, const icon_cell_t cell[LS_ICON_ROWS][LS_ICON_COLS])
{
    for (int r = 0; r < LS_ICON_ROWS; r++)
        for (int c = 0; c < LS_ICON_COLS; c++)
            if (cell[r][c].ch) tui_put_char(sf, clip, x + c, y + r, (char)cell[r][c].ch, cell[r][c].attr);
}

void ls_icon_draw_px(tui_surface *sf, tui_rect clip, int x, int y,
                     const uint8_t px[LS_ICON_ROWS * 3][LS_ICON_COLS * 2], uint8_t attr)
{
    icon_cell_t cell[LS_ICON_ROWS][LS_ICON_COLS];
    icon_cells(cell, px, attr);
    icon_blit(sf, clip, x, y, cell);
}

/* HOME draws the same fifteen icons every frame; their cells depend only on
   the icon, its colour and (for a card app's art) which art is loaded, so the
   work of choosing them is done once. */
#define ICON_CACHE_N 32
static EXT_RAM_BSS_ATTR icon_cache_t s_icon_cache[ICON_CACHE_N];
static uint8_t s_icon_next;

static const icon_cache_t *icon_find(int icon, uint8_t attr)
{
    for (int i = 0; i < ICON_CACHE_N; i++) {
        const icon_cache_t *e = &s_icon_cache[i];
        if (e->used && e->icon == icon && e->attr == attr && e->gen == s_px_gen) return e;
    }
    return NULL;
}

static void icon_store_draw(tui_surface *sf, tui_rect clip, int x, int y, int icon, uint8_t attr,
                            const uint8_t px[LS_ICON_ROWS * 3][LS_ICON_COLS * 2])
{
    icon_cache_t *e = &s_icon_cache[s_icon_next++ % ICON_CACHE_N];
    icon_cells(e->cell, px, attr);
    e->icon = icon; e->attr = attr; e->gen = s_px_gen; e->used = true;
    icon_blit(sf, clip, x, y, e->cell);
}

int ls_icon_art(int slot, const char art[LS_ICON_ROWS][LS_ICON_COLS + 1], int fallback)
{
    if (!art || slot < 0 || slot >= LS_ICON_ART_MAX) return fallback;
    bool any = false;
    char (*dst)[LS_ICON_COLS] = s_art[slot];
    for (int r = 0; r < LS_ICON_ROWS; r++) {
        bool ended = false;
        for (int c = 0; c < LS_ICON_COLS; c++) {
            const char ch = ended ? 0 : art[r][c];
            if (!ch) ended = true;
            dst[r][c] = ch >= 0x21 && ch <= 0x7e ? ch : ' ';
            any |= dst[r][c] != ' ';
        }
    }
    s_art_set[slot] = any;
    s_px_set[slot] = false;
    s_px_gen++;
    return any ? LS_ICON_ART_FIRST + slot : fallback;
}

void ls_icon_draw(tui_surface *sf, tui_rect clip, int x, int y,
                  int icon, uint8_t attr)
{
    if (!sf) return;
    if (icon >= LS_ICON_ART_FIRST) {
        const int k = icon - LS_ICON_ART_FIRST;
        if (k < LS_ICON_ART_MAX && s_px_set[k]) {
            const icon_cache_t *e = icon_find(icon, attr);
            if (e) icon_blit(sf, clip, x, y, e->cell);
            else icon_store_draw(sf, clip, x, y, icon, attr, s_px[k]);
            return;
        }
        if (k >= LS_ICON_ART_MAX || !s_art_set[k]) icon = LS_ICON_STAR;
        else {
            for (int r = 0; r < LS_ICON_ROWS; r++)
                for (int c = 0; c < LS_ICON_COLS; c++)
                    if (s_art[k][r][c] != ' ') tui_put_char(sf, clip, x + c, y + r, s_art[k][r][c], attr);
            return;
        }
    }
    const icon_cache_t *e = icon_find(icon, attr);
    if (e) { icon_blit(sf, clip, x, y, e->cell); return; }
    uint8_t px[PX_H][PX_W];
    if (unpack(icon, attr, px)) icon_store_draw(sf, clip, x, y, icon, attr, px);
}
