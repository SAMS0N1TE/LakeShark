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
