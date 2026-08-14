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

/*LS-605*/
static const char *const HOME[ICON_H] = {
    "................",
    "..........X.....",
    ".........XX.....",
    "........XXX.....",
    ".......XXXX.....",
    "......XXXXX.....",
    ".....XXXXXX.....",
    "....XXXXXXX.....",
    "...XXXXXXXX.....",
    "..XXXXXXXXX.....",
    ".XXXXXXXXXX.....",
    "................",
    "XX...XXX...XXX..",
    "................",
    "..XXX...XXX...XX",
    "................",
};

/*LS-605*/
static const char *const SETTINGS[ICON_H] = {
    "................",
    "......XXXX......",
    "......XXXX......",
    "..X...XXXX...X..",
    "..XXXXXXXXXXXX..",
    "..XXXX....XXXX..",
    "....XX....XX....",
    "XXXXX......XXXXX",
    "XXXXX......XXXXX",
    "....XX....XX....",
    "..XXXX....XXXX..",
    "..XXXXXXXXXXXX..",
    "..X...XXXX...X..",
    "......XXXX......",
    "......XXXX......",
    "................",
};

/*LS-716*/
/* AppREC asks for "rec" and the table had no entry, so the rail drew an empty
   button. Placeholder record dot - replace the grid, not the wiring. */
static const char *const REC[ICON_H] = {
    "................",
    "................",
    ".....XXXXXX.....",
    "...XXXXXXXXXX...",
    "..XXXXXXXXXXXX..",
    "..XXXXXXXXXXXX..",
    ".XXXXXXXXXXXXXX.",
    ".XXXXXXXXXXXXXX.",
    ".XXXXXXXXXXXXXX.",
    ".XXXXXXXXXXXXXX.",
    "..XXXXXXXXXXXX..",
    "..XXXXXXXXXXXX..",
    "...XXXXXXXXXX...",
    ".....XXXXXX.....",
    "................",
    "................",
};

/*LS-605*/
static lv_img_dsc_t *build(const char *const *rows, int gw, int gh, int blk)
{
    const int W = gw * blk, H = gh * blk;
    const size_t sz = (size_t)W * H;
    uint8_t *d = (uint8_t *)heap_caps_calloc(1, sz, MALLOC_CAP_SPIRAM);
    if (!d) return nullptr;

    const int span = (blk >= 4) ? blk - 1 : blk;

    for (int gy = 0; gy < gh; gy++)
        for (int gx = 0; gx < gw; gx++) {
            char c = rows[gy][gx];
            if (c == '.' || c == ' ' || c == 0) continue;
            for (int by = 0; by < span; by++)
                for (int bx = 0; bx < span; bx++)
                    d[(size_t)(gy * blk + by) * W + (gx * blk + bx)] = 0xFF;
        }

    lv_img_dsc_t *dsc = (lv_img_dsc_t *)heap_caps_calloc(1, sizeof(lv_img_dsc_t),
                                                         MALLOC_CAP_SPIRAM);
    if (!dsc) { heap_caps_free(d); return nullptr; }
    dsc->header.cf          = LV_IMG_CF_ALPHA_8BIT;
    dsc->header.always_zero = 0;
    dsc->header.w           = W;
    dsc->header.h           = H;
    dsc->data_size          = sz;
    dsc->data               = d;
    return dsc;
}

/*LS-605*/
const lv_img_dsc_t *ls_icon_for(const char *key, int px)
{
    if (!key) return nullptr;

    int blk = px / ICON_W;
    if (blk < 1) blk = 1;
    if (blk > 8) blk = 8;

    static const int SIZES = 8;
    static struct { const char *k; const char *const *rows; lv_img_dsc_t *dsc[SIZES]; } tbl[] = {
        { "p25",      P25,      {nullptr} },
        { "fm",       FM,       {nullptr} },
        { "adsb",     ADSB,     {nullptr} },
        { "mesh",     MESH,     {nullptr} },
        { "files",    FILES,    {nullptr} },
        /*LS-716*/
        { "rec",      REC,      {nullptr} },
        { "home",     HOME,     {nullptr} },
        { "settings", SETTINGS, {nullptr} },
    };

    for (auto &e : tbl) {
        if (strcmp(e.k, key) != 0) continue;
        lv_img_dsc_t *&slot = e.dsc[blk - 1];
        if (!slot) slot = build(e.rows, ICON_W, ICON_H, blk);
        return slot;
    }
    return nullptr;
}
