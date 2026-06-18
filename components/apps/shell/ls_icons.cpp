#include "shell/ls_icons.h"

#include <cstring>
#include "esp_heap_caps.h"


#define ICON_W 16
#define ICON_H 16

static const char *const P25[ICON_H] = {
    ".......XX.......",
    ".......XX.......",
    ".......XX.......",
    "......XXXX......",
    ".....XX..XX.....",
    ".....XXXXXX.....",
    ".....X....X.....",
    ".....X.XX.X.....",
    ".....X.XX.X.....",
    ".....X....X.....",
    ".....X.XX.X.....",
    ".....X.XX.X.....",
    ".....X.XX.X.....",
    ".....X....X.....",
    ".....XXXXXX.....",
    "................",
};

static const char *const FM[ICON_H] = {
    ".......XX.......",
    ".......XX.......",
    "..X....XX....X..",
    ".X.X..XXXX..X.X.",
    "X.X.X.XXXX.X.X.X",
    "X.X.X.XXXX.X.X.X",
    ".X.X..XXXX..X.X.",
    "..X...XXXX...X..",
    "......X..X......",
    "......X..X......",
    ".....X....X.....",
    ".....X....X.....",
    "....X......X....",
    "....X......X....",
    "...XXX....XXX...",
    "................",
};

static const char *const ADSB[ICON_H] = {
    ".......X........",
    "......XXX.......",
    "......XXX.......",
    ".....XXXXX......",
    "X....XXXXX....X.",
    "XX..XXXXXXX..XX.",
    "XXXXXXXXXXXXXXX.",
    "XXXXXXXXXXXXXXX.",
    ".XX.XXXXXXX.XX..",
    "....XXXXXXX.....",
    ".....XXXXX......",
    ".....XX.XX......",
    "....XX...XX.....",
    "...XX.....XX....",
    "................",
    "................",
};

static const char *const MESH[ICON_H] = {
    "................",
    "..XX........XX..",
    "..XX........XX..",
    "....X......X....",
    ".....X....X.....",
    "......X..X......",
    ".......XX.......",
    "......XXXX......",
    "......XXXX......",
    ".......XX.......",
    "......X..X......",
    ".....X....X.....",
    "....X......X....",
    "..XX........XX..",
    "..XX........XX..",
    "................",
};

static const char *const FILES[ICON_H] = {
    "................",
    "...XXXXX........",
    "..X.....X.......",
    "..X......XXXXXX.",
    "..XXXXXXXXXXXXX.",
    "..X...........X.",
    "..X...........X.",
    "..X...........X.",
    "..X...........X.",
    "..X...........X.",
    "..X...........X.",
    "..X...........X.",
    "..XXXXXXXXXXXXX.",
    "................",
    "................",
    "................",
};

#define BLK 6

static lv_img_dsc_t *build(const char *const *rows, int gw, int gh)
{
    const int W = gw * BLK, H = gh * BLK;
    const size_t sz = (size_t)W * H * 3;
    uint8_t *d = (uint8_t *)heap_caps_calloc(1, sz, MALLOC_CAP_SPIRAM);
    if (!d) return nullptr;

    const lv_color_t gold = lv_color_hex(0xC9A24A);
    const uint8_t lo = (uint8_t)(gold.full & 0xFF);
    const uint8_t hi = (uint8_t)((gold.full >> 8) & 0xFF);

    for (int gy = 0; gy < gh; gy++)
        for (int gx = 0; gx < gw; gx++) {
            char c = rows[gy][gx];
            if (c == '.' || c == ' ' || c == 0) continue;
            for (int by = 0; by < BLK; by++)
                for (int bx = 0; bx < BLK; bx++) {
                    size_t i = ((size_t)(gy * BLK + by) * W + (gx * BLK + bx)) * 3;
                    d[i] = lo; d[i + 1] = hi; d[i + 2] = 0xFF;
                }
        }

    lv_img_dsc_t *dsc = (lv_img_dsc_t *)heap_caps_calloc(1, sizeof(lv_img_dsc_t), MALLOC_CAP_SPIRAM);
    if (!dsc) { heap_caps_free(d); return nullptr; }
    dsc->header.cf          = LV_IMG_CF_TRUE_COLOR_ALPHA;
    dsc->header.always_zero = 0;
    dsc->header.w           = W;
    dsc->header.h           = H;
    dsc->data_size          = sz;
    dsc->data               = d;
    return dsc;
}

const lv_img_dsc_t *ls_icon_for(const char *key)
{
    if (!key) return nullptr;
    static struct { const char *k; const char *const *rows; lv_img_dsc_t *dsc; } tbl[] = {
        { "p25",   P25,   nullptr },
        { "fm",    FM,    nullptr },
        { "adsb",  ADSB,  nullptr },
        { "mesh",  MESH,  nullptr },
        { "files", FILES, nullptr },
    };
    for (auto &e : tbl) {
        if (strcmp(e.k, key) == 0) {
            if (!e.dsc) e.dsc = build(e.rows, ICON_W, ICON_H);
            return e.dsc;
        }
    }
    return nullptr;
}
