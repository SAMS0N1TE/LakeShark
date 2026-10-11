/* LS_TEST_SOURCES: ${FW}/components/apps/tui/tuilib/tui_core.c */

#include "ls_test.h"
#include "tui_core.h"

#include <string.h>

#define W 24
#define H 12
#define SENTINEL '#'

static tui_cell g_back[W * H];
static tui_cell g_front[W * H];
static tui_surface g_sf;

static void fresh(void)
{
    tui_surface_setup(&g_sf, g_back, g_front, W, H);
    for (int i = 0; i < W * H; i++) {
        g_back[i].ch = SENTINEL;
        g_back[i].attr = TUI_DEFAULT_ATTR;
    }
}

/* Count cells outside `r` that are no longer the sentinel. */
static int escaped(tui_rect r)
{
    int n = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            if (tui_rect_contains(r, x, y)) continue;
            if (g_back[y * W + x].ch != SENTINEL) n++;
        }
    return n;
}

LS_CASE(put_char_never_writes_outside_its_clip)
{
    tui_rect clip = tui_rect_make(4, 3, 6, 4);

    /* Every direction, including the diagonal corners, which is where an
       `x < w && y < h` test that forgot its lower bound lets one through. */
    static const int OFFS[][2] = {
        { -1, -1 }, { 0, -1 }, { 100, -1 }, { -1, 0 }, { 100, 0 },
        { -1, 100 }, { 0, 100 }, { 100, 100 },
        { -1000, -1000 }, { 1000, 1000 },
        { 3, 3 }, { 10, 3 }, { 4, 2 }, { 4, 7 },
    };
    fresh();
    for (unsigned i = 0; i < sizeof(OFFS) / sizeof(OFFS[0]); i++)
        tui_put_char(&g_sf, clip, OFFS[i][0], OFFS[i][1], 'X',
                     TUI_DEFAULT_ATTR);

    LS_EQ_INT(0, escaped(clip));
}

LS_CASE(put_str_truncates_at_the_clip_and_never_wraps)
{
    /* Wrapping is the specific failure worth pinning: a string that ran off
       the right edge and continued on the next row would corrupt a
       neighbouring pane while staying inside the surface, so a bounds check
       against the surface alone would not catch it. */
    tui_rect clip = tui_rect_make(4, 3, 6, 4);
    fresh();
    tui_put_str(&g_sf, clip, 4, 3,
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", TUI_DEFAULT_ATTR);

    LS_EQ_INT(0, escaped(clip));

    /* The row below the one written must be untouched inside the clip too. */
    for (int x = clip.x; x < clip.x + clip.w; x++)
        LS_EQ_INT(SENTINEL, g_back[(clip.y + 1) * W + x].ch);
}

LS_CASE(put_str_starting_left_of_the_clip_is_clipped_not_shifted)
{
    /* A negative start is what a right-aligned label computes when the pane
       is narrower than the text. It must lose the leading characters, not
       slide right into view. */
    tui_rect clip = tui_rect_make(8, 4, 5, 2);
    fresh();
    tui_put_str(&g_sf, clip, 2, 4, "0123456789ABCDEF", TUI_DEFAULT_ATTR);

    LS_EQ_INT(0, escaped(clip));
    /* Column 8 holds the character whose index puts it there, not the first. */
    LS_EQ_INT('6', g_back[4 * W + 8].ch);
}

LS_CASE(fill_never_writes_outside_its_rect)
{
    tui_rect clip = tui_rect_make(5, 2, 8, 5);
    fresh();
    tui_fill(&g_sf, clip, '*', TUI_DEFAULT_ATTR);
    LS_EQ_INT(0, escaped(clip));

    /* And it filled the whole of it: a fill that is correct only because it
       did nothing would pass the check above. */
    for (int y = clip.y; y < clip.y + clip.h; y++)
        for (int x = clip.x; x < clip.x + clip.w; x++)
            LS_EQ_INT('*', g_back[y * W + x].ch);
}

LS_CASE(fill_with_a_rect_larger_than_the_surface_stays_in_the_surface)
{
    fresh();
    tui_fill(&g_sf, tui_rect_make(-50, -50, 1000, 1000), '*', TUI_DEFAULT_ATTR);
    /* Nothing to assert about escaping - the point is that it returns at all
       and did not run off either end of the buffer. */
    LS_EQ_INT('*', g_back[0].ch);
    LS_EQ_INT('*', g_back[W * H - 1].ch);
}

LS_CASE(degenerate_rects_draw_nothing)
{
    static const tui_rect BAD[] = {
        { 4, 4, 0, 5 }, { 4, 4, 5, 0 }, { 4, 4, -3, 5 }, { 4, 4, 5, -3 },
        { 4, 4, 0, 0 },
    };
    for (unsigned i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        fresh();
        tui_fill(&g_sf, BAD[i], '*', TUI_DEFAULT_ATTR);
        tui_put_str(&g_sf, BAD[i], 4, 4, "text", TUI_DEFAULT_ATTR);
        tui_put_char(&g_sf, BAD[i], 4, 4, 'X', TUI_DEFAULT_ATTR);
        /* A rect with no area addresses no cell, so the surface is untouched. */
        for (int c = 0; c < W * H; c++) LS_EQ_INT(SENTINEL, g_back[c].ch);
    }
}

LS_CASE(box_stays_inside_the_rect_it_was_given)
{
    tui_rect r = tui_rect_make(3, 2, 10, 6);
    fresh();
    tui_box(&g_sf, r, "TITLE", TUI_DEFAULT_ATTR);
    LS_EQ_INT(0, escaped(r));
}

LS_CASE(box_too_small_to_have_an_interior_draws_nothing)
{
    static const tui_rect TINY[] = {
        { 3, 3, 1, 5 }, { 3, 3, 5, 1 }, { 3, 3, 1, 1 }, { 3, 3, 0, 0 },
    };
    for (unsigned i = 0; i < sizeof(TINY) / sizeof(TINY[0]); i++) {
        fresh();
        tui_box(&g_sf, TINY[i], "T", TUI_DEFAULT_ATTR);
        for (int c = 0; c < W * H; c++) LS_EQ_INT(SENTINEL, g_back[c].ch);
    }
}

LS_CASE(a_long_title_cannot_push_the_box_border_out)
{
    /* The title is caller-supplied and frequently longer than the pane in
       portrait, which is exactly when a box would burst its own frame. */
    tui_rect r = tui_rect_make(2, 2, 8, 4);
    fresh();
    tui_box(&g_sf, r, "AN EXTREMELY LONG PANEL TITLE", TUI_DEFAULT_ATTR);
    LS_EQ_INT(0, escaped(r));
}

LS_CASE(drawing_the_same_thing_twice_gives_the_same_grid)
{
    /* Idempotence, which the diff renderer turns into "does this flicker".
       Any per-call state inside the primitives would show up here. */
    tui_cell first[W * H];
    tui_rect r = tui_rect_make(2, 1, 14, 8);

    fresh();
    tui_box(&g_sf, r, "P", TUI_DEFAULT_ATTR);
    tui_put_str(&g_sf, r, 4, 3, "value 123", TUI_DEFAULT_ATTR);
    tui_fill(&g_sf, tui_rect_make(4, 5, 6, 2), '.', TUI_DEFAULT_ATTR);
    memcpy(first, g_back, sizeof(first));

    fresh();
    tui_box(&g_sf, r, "P", TUI_DEFAULT_ATTR);
    tui_put_str(&g_sf, r, 4, 3, "value 123", TUI_DEFAULT_ATTR);
    tui_fill(&g_sf, tui_rect_make(4, 5, 6, 2), '.', TUI_DEFAULT_ATTR);

    LS_EQ_INT(0, memcmp(first, g_back, sizeof(first)));
}

LS_CASE(two_adjacent_panes_cannot_touch_each_other)
{
    /* The split layout case, reduced: fill both halves to the edge and assert
       neither reached into the other. This is the failure the renderer makes
       permanent. */
    tui_rect left  = tui_rect_make(0, 0, W / 2, H);
    tui_rect right = tui_rect_make(W / 2, 0, W - W / 2, H);

    fresh();
    tui_fill(&g_sf, left, 'L', TUI_DEFAULT_ATTR);
    tui_box(&g_sf, left, "LEFT PANE TITLE THAT IS TOO LONG", TUI_DEFAULT_ATTR);
    tui_put_str(&g_sf, left, 1, 5, "0123456789012345678901234567890",
                TUI_DEFAULT_ATTR);

    for (int y = 0; y < H; y++)
        for (int x = right.x; x < W; x++)
            LS_EQ_INT(SENTINEL, g_back[y * W + x].ch);
}

/* Row `y` from column `x`, `n` cells, as a string. */
static const char *row_text(int x, int y, int n)
{
    static char out[W + 1];
    for (int i = 0; i < n; i++) out[i] = (char)g_back[y * W + x + i].ch;
    out[n] = 0;
    return out;
}

LS_CASE(put_str_folds_utf8_letters_to_one_ascii_cell_each)
{
    /* Mesh messages and node names arrive as UTF-8 and the fonts are ASCII.
       Each letter is one cell, its base letter; before, each byte took a
       cell of its own and drew nothing. Literals are split where a hex
       escape would otherwise swallow the next letter. */
    fresh();
    tui_put_str(&g_sf, tui_surface_rect(&g_sf), 0, 0,
                "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87 g\xC4\x99\xC5\x9B" "l"
                "\xC4\x85 ja\xC5\xBA\xC5\x84", TUI_DEFAULT_ATTR);
    LS_EQ_STR("Zazolc gesla jazn", row_text(0, 0, 17));
    LS_EQ_INT(SENTINEL, g_back[17].ch);

    fresh();
    tui_put_str(&g_sf, tui_surface_rect(&g_sf), 0, 1,
                "ZA\xC5\xBB\xC3\x93\xC5\x81\xC4\x86 G\xC4\x98\xC5\x9A" "L"
                "\xC4\x84 JA\xC5\xB9\xC5\x83", TUI_DEFAULT_ATTR);
    LS_EQ_STR("ZAZOLC GESLA JAZN", row_text(0, 1, 17));

    /* Outside the folded range: one '?' per character, whatever its length. */
    fresh();
    tui_put_str(&g_sf, tui_surface_rect(&g_sf), 0, 2,
                "a\xE2\x82\xAC" "b\xF0\x9F\x98\x80" "c", TUI_DEFAULT_ATTR);
    LS_EQ_STR("a?b?c", row_text(0, 2, 5));
    LS_EQ_INT(SENTINEL, g_back[2 * W + 5].ch);
}

LS_CASE(put_str_clips_utf8_by_cells_not_bytes)
{
    /* Ten two-byte letters in a five-cell clip: five cells, not two and a
       half, and nothing past the clip. */
    tui_rect clip = tui_rect_make(2, 4, 5, 1);
    fresh();
    tui_put_str(&g_sf, clip, 2, 4,
                "\xC4\x99\xC4\x99\xC4\x99\xC4\x99\xC4\x99"
                "\xC4\x99\xC4\x99\xC4\x99\xC4\x99\xC4\x99", TUI_DEFAULT_ATTR);
    LS_EQ_STR("eeeee", row_text(2, 4, 5));
    LS_EQ_INT(0, escaped(clip));

    /* Started left of the clip, the leading letters are lost, not shifted. */
    fresh();
    tui_put_str(&g_sf, clip, 0, 4, "\xC4\x85\xC4\x87\xC4\x99\xC5\x82\xC5\x84",
                TUI_DEFAULT_ATTR);
    LS_EQ_STR("eln", row_text(2, 4, 3));
    LS_EQ_INT(0, escaped(clip));
}

LS_CASE(broken_utf8_never_reads_past_the_terminator)
{
    /* A message cut mid-letter, a stray continuation byte and a lead byte
       UTF-8 never uses: each becomes one '?' and the scan stops at the NUL.
       The bytes after the NUL are a trap: reading them would draw 'X'. */
    static const char cut[] = "ab\xC5\0XXXX";
    fresh();
    tui_put_str(&g_sf, tui_surface_rect(&g_sf), 0, 5, cut, TUI_DEFAULT_ATTR);
    LS_EQ_STR("ab?", row_text(0, 5, 3));
    LS_EQ_INT(SENTINEL, g_back[5 * W + 3].ch);

    static const char cut3[] = "\xE2\x82\0XXXX";
    fresh();
    tui_put_str(&g_sf, tui_surface_rect(&g_sf), 0, 6, cut3, TUI_DEFAULT_ATTR);
    LS_EQ_STR("?", row_text(0, 6, 1));
    LS_EQ_INT(SENTINEL, g_back[6 * W + 1].ch);

    fresh();
    tui_put_str(&g_sf, tui_surface_rect(&g_sf), 0, 7, "a\x80" "b\xFF" "c",
                TUI_DEFAULT_ATTR);
    LS_EQ_STR("a?b?c", row_text(0, 7, 5));
}

LS_CASE(utf8_cells_counts_letters_in_a_byte_prefix)
{
    /* What a layout uses to place text after a UTF-8 name. */
    LS_EQ_INT(4, tui_utf8_cells("Ko\xC5\x82o", 5));
    LS_EQ_INT(2, tui_utf8_cells("Ko\xC5\x82o", 2));
    LS_EQ_INT(2, tui_utf8_cells("ab\0cd", 5));
}

LS_CASE(fold_str_makes_one_byte_per_letter_for_byte_measured_labels)
{
    /* Map labels are measured, cut and boxed by bytes; folded in place they
       line up with the cells. */
    char s[] = "Bydgoszcz \xC5\x81\xC3\xB3" "d\xC5\xBA";
    tui_utf8_fold_str(s);
    LS_EQ_STR("Bydgoszcz Lodz", s);

    /* A name cut by a fixed-size buffer upstream loses the broken letter,
       not gains a '?'. */
    char cut[] = "Gda\xC5\x84sk \xC5";
    tui_utf8_fold_str(cut);
    LS_EQ_STR("Gdansk ", cut);

    /* A broken byte in the middle is still one '?', as tui_put_str draws it. */
    char mid[] = "a\x80" "b";
    tui_utf8_fold_str(mid);
    LS_EQ_STR("a?b", mid);
}

LS_CASE(fold_codepoint_matches_the_utf8_fold)
{
    /* CartoCore hands over decoded codepoints; they must fold the same way. */
    LS_EQ_INT('A', tui_fold_codepoint(0x41));
    LS_EQ_INT('L', tui_fold_codepoint(0x141));
    LS_EQ_INT('z', tui_fold_codepoint(0x17C));
    LS_EQ_INT('?', tui_fold_codepoint(0x0416));
    const char *p = "\xC5\x81";
    LS_EQ_INT(tui_fold_codepoint(0x141), tui_utf8_fold(&p));
}
