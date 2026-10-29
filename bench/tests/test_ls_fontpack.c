/* Glyph bitmaps as the T-Display builds them (textpack_gen.py fonts): the
   generated font files, their tables decoded from zstd by a constructor
   before main, must hold every byte the original files hold, and the font
   structs must say the same thing. The originals are compiled into this file
   under other names to compare against. */

#include "ls_test.h"

#include <string.h>

#include "ls_font.h"

#define ls_font_mono_14 orig_ls_font_mono_14
#define ls_font_mono_16 orig_ls_font_mono_16
#define ls_font_mono_24 orig_ls_font_mono_24
#define ls_font_cyr_page orig_ls_font_cyr_page
#include "ls_font_mono_14.c"
#include "ls_font_mono_16.c"
#include "ls_font_mono_24.c"
#include "ls_font_cyr.c"
#undef ls_font_mono_14
#undef ls_font_mono_16
#undef ls_font_mono_24
#undef ls_font_cyr_page

extern const ls_font_t ls_font_mono_14;
extern const ls_font_t ls_font_mono_16;
extern const ls_font_t ls_font_mono_24;
const ls_font_t *ls_font_cyr_page(const ls_font_t *latin);
extern const ls_font_t orig_ls_font_mono_14;
extern const ls_font_t orig_ls_font_mono_16;
extern const ls_font_t orig_ls_font_mono_24;
const ls_font_t *orig_ls_font_cyr_page(const ls_font_t *latin);

static void same_font(const ls_font_t *a, const ls_font_t *b)
{
    LS_CHECK(a && b);
    if (!a || !b) return;
    LS_CHECK(a != b);
    LS_EQ_UINT(a->bitmap_len, b->bitmap_len);
    LS_EQ_UINT(a->first, b->first);
    LS_EQ_UINT(a->count, b->count);
    LS_EQ_UINT(a->cell_w, b->cell_w);
    LS_EQ_UINT(a->cell_h, b->cell_h);
    LS_EQ_UINT(a->base_line, b->base_line);
    LS_EQ_STR(a->name, b->name);
    LS_CHECK(memcmp(a->dsc, b->dsc, sizeof(a->dsc[0]) * a->count) == 0);
    LS_CHECK(a->bitmap != b->bitmap);
    LS_CHECK_MSG(memcmp(a->bitmap, b->bitmap, a->bitmap_len) == 0, "%s bitmap differs", a->name);
}

LS_CASE(packed_fonts_equal_the_originals)
{
    same_font(&ls_font_mono_14, &orig_ls_font_mono_14);
    same_font(&ls_font_mono_16, &orig_ls_font_mono_16);
    same_font(&ls_font_mono_24, &orig_ls_font_mono_24);
}

LS_CASE(packed_cyrillic_pages_equal_the_originals)
{
    same_font(ls_font_cyr_page(&ls_font_mono_14), orig_ls_font_cyr_page(&orig_ls_font_mono_14));
    same_font(ls_font_cyr_page(&ls_font_mono_16), orig_ls_font_cyr_page(&orig_ls_font_mono_16));
    same_font(ls_font_cyr_page(&ls_font_mono_24), orig_ls_font_cyr_page(&orig_ls_font_mono_24));
}
