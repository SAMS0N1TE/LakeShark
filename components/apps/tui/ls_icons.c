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
    return any ? LS_ICON_ART_FIRST + slot : fallback;
}

void ls_icon_draw_px(tui_surface *sf, tui_rect clip, int x, int y,
                     const uint8_t px[LS_ICON_ROWS * 3][LS_ICON_COLS * 2], uint8_t attr)
{
    const int ground = TUI_ATTR_BG(attr);
    for (int r = 0; r < LS_ICON_ROWS; r++)
        for (int c = 0; c < LS_ICON_COLS; c++) {
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
            tui_put_char(sf, clip, x + c, y + r, (char)(0xC0 | bits), TUI_ATTR(fg, bg));
        }
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
    return any ? LS_ICON_ART_FIRST + slot : fallback;
}

void ls_icon_draw(tui_surface *sf, tui_rect clip, int x, int y,
                  int icon, uint8_t attr)
{
    if (!sf) return;
    if (icon >= LS_ICON_ART_FIRST) {
        const int k = icon - LS_ICON_ART_FIRST;
        if (k < LS_ICON_ART_MAX && s_px_set[k]) {
            ls_icon_draw_px(sf, clip, x, y, s_px[k], attr);
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
    uint8_t px[PX_H][PX_W];
    if (unpack(icon, attr, px)) ls_icon_draw_px(sf, clip, x, y, px, attr);
}
