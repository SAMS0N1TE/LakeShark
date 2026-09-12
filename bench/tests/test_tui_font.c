/* LS_TEST_SOURCES: the generated font tables */

#include "ls_test.h"
#include "ls_font.h"

#include <string.h>

static const ls_font_t *const FACES[] = {
    &ls_font_mono_16, &ls_font_mono_14,
};
#define N_FACES ((int)(sizeof(FACES) / sizeof(FACES[0])))

/* 4 bpp, rows packed continuously with no byte alignment between them, so a
   glyph occupies ceil(w * h / 2) bytes. */
static uint32_t glyph_bytes(const ls_font_glyph_t *g)
{
    uint32_t px = (uint32_t)g->box_w * g->box_h;
    return (px + 1) / 2;
}

LS_CASE(both_faces_are_present_and_describe_printable_ascii)
{
    for (int f = 0; f < N_FACES; f++) {
        const ls_font_t *fn = FACES[f];
        LS_CHECK(fn->name && fn->name[0]);
        LS_CHECK(fn->dsc != NULL);
        LS_CHECK(fn->bitmap != NULL);

        /* 0x20 through 0x7E is what a TUI can put on screen. Anything less
           and some character in a label renders as nothing. */
        LS_CHECK_MSG(fn->first == 0x20, "%s starts at 0x%02X",
                     fn->name, fn->first);
        LS_CHECK_MSG(fn->count == 95, "%s has %d glyphs",
                     fn->name, fn->count);
    }
}

LS_CASE(the_cell_is_big_enough_for_the_glyphs_in_it)
{
    /* The grid geometry comes from cell_w and cell_h, so a glyph wider than
       its cell does not overflow a buffer - it overwrites the neighbouring
       column, which the diff renderer then leaves on screen. */
    for (int f = 0; f < N_FACES; f++) {
        const ls_font_t *fn = FACES[f];
        LS_CHECK(fn->cell_w > 0 && fn->cell_h > 0);

        for (int i = 0; i < fn->count; i++) {
            const ls_font_glyph_t *g = &fn->dsc[i];
            int right  = g->ofs_x + g->box_w;
            LS_CHECK_MSG(right <= fn->cell_w,
                         "%s glyph 0x%02X reaches column %d of a %d-wide cell",
                         fn->name, 0x20 + i, right, fn->cell_w);
            LS_CHECK_MSG(g->box_h <= fn->cell_h,
                         "%s glyph 0x%02X is %d rows in a %d-row cell",
                         fn->name, 0x20 + i, g->box_h, fn->cell_h);
        }
    }
}

LS_CASE(no_glyph_reads_past_the_end_of_its_table)
{
    /* The failure that shipped. Every byte a glyph will read has to exist. */
    for (int f = 0; f < N_FACES; f++) {
        const ls_font_t *fn = FACES[f];
        LS_CHECK_MSG(fn->bitmap_len > 0, "%s has no bitmap", fn->name);

        for (int i = 0; i < fn->count; i++) {
            const ls_font_glyph_t *g = &fn->dsc[i];
            uint32_t end = (uint32_t)g->bitmap_index + glyph_bytes(g);
            LS_CHECK_MSG(end <= fn->bitmap_len,
                         "%s glyph 0x%02X reads to %u of %u bytes",
                         fn->name, 0x20 + i, end, fn->bitmap_len);
        }
    }
}

LS_CASE(glyph_offsets_only_move_forward)
{

    for (int f = 0; f < N_FACES; f++) {
        const ls_font_t *fn = FACES[f];
        uint32_t prev = 0;
        for (int i = 0; i < fn->count; i++) {
            uint32_t at = fn->dsc[i].bitmap_index;
            LS_CHECK_MSG(at >= prev,
                         "%s glyph 0x%02X starts at %u, behind %u",
                         fn->name, 0x20 + i, at, prev);
            prev = at;
        }
    }
}

LS_CASE(the_table_is_exactly_as_long_as_the_descriptors_require)
{
    /* The check that catches the corruption directly, and the one the
       generator only applies at generation time. Sum what every glyph needs
       and compare it with what is actually committed. Under the dropped-byte
       bug this reads 2294 against 4052. */
    for (int f = 0; f < N_FACES; f++) {
        const ls_font_t *fn = FACES[f];
        uint32_t need = 0;
        for (int i = 0; i < fn->count; i++) need += glyph_bytes(&fn->dsc[i]);

        LS_CHECK_MSG(need == fn->bitmap_len,
                     "%s: descriptors need %u bytes, the table holds %u",
                     fn->name, need, fn->bitmap_len);
    }
}

LS_CASE(the_letters_have_ink_in_them)
{

    static const char INKED[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

    for (int f = 0; f < N_FACES; f++) {
        const ls_font_t *fn = FACES[f];
        for (const char *c = INKED; *c; c++) {
            int i = (unsigned char)*c - fn->first;
            LS_CHECK(i >= 0 && i < fn->count);
            const ls_font_glyph_t *g = &fn->dsc[i];

            LS_CHECK_MSG(g->box_w > 0 && g->box_h > 0,
                         "%s: '%c' has an empty box", fn->name, *c);

            uint32_t n = glyph_bytes(g);
            uint32_t nonzero = 0;
            for (uint32_t b = 0; b < n; b++)
                if (fn->bitmap[g->bitmap_index + b]) nonzero++;
            LS_CHECK_MSG(nonzero > 0,
                         "%s: '%c' is entirely blank", fn->name, *c);
        }
    }
}

LS_CASE(space_is_blank_and_costs_nothing_to_draw)
{
    /* The blitter skips a glyph with no box, and space is the common case in
       a mostly-empty grid, so this is a performance property as much as a
       correctness one. */
    for (int f = 0; f < N_FACES; f++) {
        const ls_font_t *fn = FACES[f];
        const ls_font_glyph_t *sp = &fn->dsc[0];   /* 0x20 */
        LS_CHECK_MSG(sp->box_w == 0 || sp->box_h == 0,
                     "%s: space has a %dx%d box and will be blitted",
                     fn->name, sp->box_w, sp->box_h);
    }
}

LS_CASE(the_lookup_rejects_codepoints_the_face_does_not_have)
{
    for (int f = 0; f < N_FACES; f++) {
        const ls_font_t *fn = FACES[f];
        /* Below the first and above the last, plus the blank one. */
        LS_CHECK(ls_font_glyph(fn, 0x1F) == NULL);
        LS_CHECK(ls_font_glyph(fn, 0x7F) == NULL);
        LS_CHECK(ls_font_glyph(fn, 0xFF) == NULL);
        LS_CHECK(ls_font_glyph(fn, ' ') == NULL);   /* blank, nothing to blit */
        LS_CHECK(ls_font_glyph(fn, 'A') != NULL);
        LS_CHECK(ls_font_glyph(fn, '~') != NULL);
    }
}

LS_CASE(the_two_faces_really_are_different_sizes)
{
    /* The point of having both is a smaller cell giving more columns. If a
       regeneration ever pointed both at the same source this would pass
       everything else silently. */
    LS_CHECK(ls_font_mono_14.cell_w < ls_font_mono_16.cell_w ||
             ls_font_mono_14.cell_h < ls_font_mono_16.cell_h);
    LS_CHECK(ls_font_mono_14.bitmap != ls_font_mono_16.bitmap);
    LS_CHECK(ls_font_mono_14.dsc    != ls_font_mono_16.dsc);
}
