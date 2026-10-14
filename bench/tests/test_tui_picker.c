/* LS_TEST_SOURCES: ls_picker.c against its own drawn output */

#include "ls_test.h"

#include "ls_picker.h"
#include "tui_core.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------- surface -- */

/* Deliberately NOT 66 rows tall. At the real portrait height the window
   happens to fit eight rows... and the old broken constant was four, so a
   test run at a height where a page IS four would pass against the bug. */
#define W 48
#define H 40

static tui_cell g_back[W * H];
static tui_cell g_front[W * H];
static tui_surface g_sf;

/* Mirrors of ls_picker.c's layout. See the header comment. */
#define ROW_H     5
#define ROW_STEP  (ROW_H - 1)
#define BTN_H     4

static tui_rect area(void) { return tui_rect_make(0, 0, W, H); }

static int btn_row(void)   { return H - 1 - BTN_H + 1; }  /* inside the button */
static int page_up_col(void)   { return 1 + ((W - 2) / 3) / 2; }
static int page_dn_col(void)   { return 1 + (W - 2) / 3 + ((W - 2) / 3) / 2; }

/* First list row's middle. The body starts one row below the border. */
static int list_row_y(int i) { return 1 + i * ROW_STEP + ROW_H / 2; }

static void surface_reset(void)
{
    tui_surface_setup(&g_sf, g_back, g_front, W, H);
    tui_frame_begin(&g_sf);
}

static void row_text(int y, char *out, size_t n)
{
    size_t k = 0;
    for (int x = 0; x < W && k + 1 < n; x++) out[k++] = g_back[y * W + x].ch;
    while (k > 0 && out[k - 1] == ' ') k--;
    out[k] = 0;
}

/* --------------------------------------------------------------- fixture -- */

static int s_chosen;
static int s_done_calls;

static void on_done(int index) { s_chosen = index; s_done_calls++; }

static void open_with(int n)
{
    s_chosen = -1;
    s_done_calls = 0;
    ls_picker_open("PLACES", on_done);
    for (int i = 0; i < n; i++) {
        char label[24], detail[16];
        snprintf(label, sizeof(label), "place %02d", i);
        snprintf(detail, sizeof(detail), "%d mi", i);
        ls_picker_add(label, detail);
    }
    surface_reset();
    ls_picker_draw(&g_sf, area());
}

static void redraw(void)
{
    surface_reset();
    ls_picker_draw(&g_sf, area());
}

/* Choose whatever is drawn in the top row and report its index. This is the
   one-tap select, which is how the window position becomes observable. */
static int index_of_top_row(void)
{
    ls_picker_touch(W / 2, list_row_y(0));
    return s_chosen;
}

/* ----------------------------------------------------------------- cases -- */

LS_CASE(the_page_button_moves_a_whole_window_not_a_fixed_four)
{
    /* The regression. Nineteen entries, eight rows visible: one press of
       PAGE DN must put entry eight at the top. Under the old code it moved
       the cursor by four, which was still on screen, so the window did not
       move at all and the top row was still entry zero. */
    open_with(19);
    LS_EQ_INT(index_of_top_row(), 0);

    open_with(19);
    ls_picker_touch(page_dn_col(), btn_row());
    redraw();
    const int top = index_of_top_row();

    LS_CHECK_MSG(top > 4,
                 "PAGE DN moved to entry %d - that is the old move(4), not a page",
                 top);
    LS_EQ_INT(top, 8);
}

LS_CASE(pages_tile_without_skipping_an_entry)
{

    open_with(19);
    char before[W + 1];
    row_text(list_row_y(7), before, sizeof(before));
    LS_CHECK_MSG(strstr(before, "place 07") != NULL,
                 "expected eight rows visible, bottom row reads '%s'", before);

    ls_picker_touch(page_dn_col(), btn_row());
    redraw();
    char after[W + 1];
    row_text(list_row_y(0), after, sizeof(after));
    LS_CHECK_MSG(strstr(after, "place 08") != NULL,
                 "after a page the top row reads '%s' - a gap or an overlap",
                 after);
}

LS_CASE(the_down_key_still_moves_exactly_one_entry)
{
    /* The other half of the fault: two inputs called DOWN that disagree.
       Whatever the button does, the key means the next entry. */
    open_with(19);
    ls_picker_key(LS_TK_DOWN, 0);
    ls_picker_key(LS_TK_ENTER, 0);
    LS_EQ_INT(s_chosen, 1);
}

LS_CASE(paging_back_returns_to_where_it_started)
{
    open_with(19);
    ls_picker_touch(page_dn_col(), btn_row());
    redraw();
    ls_picker_touch(page_up_col(), btn_row());
    redraw();
    LS_EQ_INT(index_of_top_row(), 0);
}

LS_CASE(the_end_of_the_list_can_actually_be_reached)
{
    /* Nineteen entries and eight to a page is not a whole number of pages,
       so the last press has less than a page left to give. What matters is
       not where the cursor stops - it stops at the top of the final window -
       but that the LAST entry is on screen and under a thumb. A pager that
       clamps one row early leaves an entry nobody can choose. */
    open_with(19);
    for (int i = 0; i < 6; i++) {
        ls_picker_touch(page_dn_col(), btn_row());
        redraw();
    }
    char bottom[W + 1];
    row_text(list_row_y(7), bottom, sizeof(bottom));
    LS_CHECK_MSG(strstr(bottom, "place 18") != NULL,
                 "bottom row of the last page reads '%s'", bottom);

    ls_picker_touch(W / 2, list_row_y(7));
    LS_EQ_INT(s_chosen, 18);
}

LS_CASE(a_list_that_fits_has_no_page_buttons_to_press)
{

    open_with(4);
    ls_picker_touch(page_dn_col(), btn_row());
    LS_CHECK_MSG(!ls_picker_active(),
                 "the full-width button under a short list must be CLOSE");
    LS_EQ_INT(s_done_calls, 0);
}

LS_CASE(the_border_admits_the_list_runs_past_the_window)
{

    open_with(19);
    char top[W + 1];
    row_text(0, top, sizeof(top));
    LS_CHECK_MSG(strstr(top, "1-8 of 19") != NULL,
                 "top border reads '%s'", top);

    ls_picker_touch(page_dn_col(), btn_row());
    redraw();
    row_text(0, top, sizeof(top));
    LS_CHECK_MSG(strstr(top, "9-16 of 19") != NULL,
                 "after a page the top border reads '%s'", top);
}

LS_CASE(a_short_list_does_not_claim_a_position)
{
    /* The counter is a statement that something is hidden. When nothing is,
       it is noise on the one line that carries the title. */
    open_with(4);
    char top[W + 1];
    row_text(0, top, sizeof(top));
    LS_CHECK_MSG(strstr(top, " of ") == NULL,
                 "a list that fits printed a position: '%s'", top);
}

LS_CASE(switch_picker_keeps_filter_and_cursor_after_accept) {
    open_with(19);ls_picker_keep_open();
    ls_picker_key(LS_TK_CHAR,'1');ls_picker_key(LS_TK_DOWN,0);
    ls_picker_key(LS_TK_ENTER,0);LS_CHECK(ls_picker_active());
    int first=s_chosen;ls_picker_key(LS_TK_ENTER,0);
    LS_EQ_INT(first,s_chosen);LS_EQ_INT(2,s_done_calls);
    ls_picker_key(LS_TK_ESC,0);LS_CHECK(!ls_picker_active());
}

static int preview_calls, stepped_value;
static tui_rect preview_area;
static void preview_draw(tui_surface *sf, tui_rect r)
{
    preview_calls++; preview_area=r;
    tui_put_str(sf,r,r.x,r.y,"CONTEXT",0);
}
static void preview_step(int index, int dir) { (void)index; stepped_value+=dir; }
LS_CASE(context_preview_preserves_taps_and_resets_on_open)
{
    static tui_cell back[115*67], front[115*67];
    tui_surface sf;
    const int widths[]={52,115}, heights[]={60,27};
    for(int mode=0;mode<2;mode++) {
        int w=widths[mode],h=heights[mode];
        tui_surface_setup(&sf,back,front,w,h); tui_frame_begin(&sf);
        ls_picker_open("SURVEY",on_done); ls_picker_add("SCAN INTERVAL","10");
        ls_picker_stepper(0,preview_step); ls_picker_preview(preview_draw);
        preview_calls=0; stepped_value=0;
        ls_picker_draw(&sf,tui_rect_make(0,0,w,h));
        LS_EQ_INT(preview_calls,1);
        LS_CHECK(preview_area.w>=40 && preview_area.h>=15);
        int right=mode?w-46:w-2;
        /* All eight filled columns of the increment key are tappable. */
        for(int x=right-8;x<right;x++) ls_picker_touch(x,3);
        LS_EQ_INT(stepped_value,8);
        int before=s_done_calls;
        ls_picker_touch(preview_area.x+2,preview_area.y+2);
        LS_EQ_INT(s_done_calls,before);
        ls_picker_open("PLACES",on_done); ls_picker_add("one","");
        ls_picker_draw(&sf,tui_rect_make(0,0,w,h));
        LS_EQ_INT(preview_calls,1);
    }
}
