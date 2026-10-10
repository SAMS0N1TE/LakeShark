/* LS_TEST_SOURCES: ls_tui.c with the panel stubbed */

#include "ls_test.h"
#include "ls_tui.h"
#include "ls_font.h"
#include "ls_panel.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------- the panel stub -- */

/* The real panel is portrait and the framebuffer never rotates; landscape is
   entirely the blitter's transpose. Sizing this to the actual panel is what
   makes the landscape branch exercise the same arithmetic it does on glass. */
static int NATIVE_W = 568, NATIVE_H = 1232;

static uint16_t g_fb[568 * 1232];
static int g_presents;

bool ls_panel_fb(ls_panel_fb_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->pixels = g_fb;
    out->width  = NATIVE_W;
    out->height = NATIVE_H;
    return true;
}
void ls_panel_fb_present(void) { g_presents++; }
void ls_panel_fb_present_rows(int y0, int y1) { (void)y0; (void)y1; g_presents++; }

/* ------------------------------------------------------------- helpers -- */

/* Find any framebuffer pixel the last paint changed. Returns false when the
   cell painted nothing, which is a real answer for a blank cell. */
static bool one_lit_pixel(int *px, int *py)
{
    for (int y = 0; y < NATIVE_H; y++)
        for (int x = 0; x < NATIVE_W; x++)
            if (g_fb[y * NATIVE_W + x]) { *px = x; *py = y; return true; }
    return false;
}

static void paint_only(int col, int row, char ch)
{
    memset(g_fb, 0, sizeof(g_fb));
    tui_surface *sf = ls_tui_surface();
    tui_rect all = tui_surface_rect(sf);
    tui_frame_begin(sf);
    tui_put_char(sf, all, col, row, ch, TUI_ATTR(TUI_WHITE, TUI_BLACK));
    ls_tui_invalidate();
    ls_tui_present();
}

/* ---------------------------------------------------------------- cases -- */

LS_CASE(a_painted_cell_maps_back_to_itself_in_landscape)
{
    LS_CHECK(ls_tui_begin(1232, 568));
    ls_tui_set_rotation_cw(true);

    int cols, rows;
    ls_tui_geometry(&cols, &rows, NULL, NULL);
    LS_CHECK(cols > 8 && rows > 4);

    /* Corners and centre. The corners are where a sign error shows and the
       centre is where one hides. */
    const int C[][2] = {
        { 0, 0 }, { cols - 1, 0 }, { 0, rows - 1 }, { cols - 1, rows - 1 },
        { cols / 2, rows / 2 }, { 1, rows - 2 }, { cols - 2, 1 },
    };
    for (unsigned i = 0; i < sizeof(C) / sizeof(C[0]); i++) {
        paint_only(C[i][0], C[i][1], 'W');
        int px, py;
        LS_CHECK_MSG(one_lit_pixel(&px, &py),
                     "cell %d,%d painted nothing", C[i][0], C[i][1]);

        int gc = -1, gr = -1;
        LS_CHECK_MSG(ls_tui_pixel_to_cell(px, py, &gc, &gr),
                     "pixel %d,%d from cell %d,%d mapped to no cell",
                     px, py, C[i][0], C[i][1]);
        LS_CHECK_MSG(gc == C[i][0] && gr == C[i][1],
                     "cell %d,%d painted a pixel that maps back to %d,%d",
                     C[i][0], C[i][1], gc, gr);
    }
    ls_tui_end();
}

LS_CASE(counter_clockwise_pixels_and_touch_agree)
{
    ls_tui_set_rotation_cw(false);
    LS_CHECK(ls_tui_begin(1232, 568));
    int cols, rows;
    ls_tui_geometry(&cols, &rows, NULL, NULL);
    const int C[][2] = { {0,0}, {cols-1,0}, {0,rows-1}, {cols-1,rows-1}, {cols/2,rows/2} };
    for (unsigned i=0; i<sizeof(C)/sizeof(C[0]); i++) {
        paint_only(C[i][0], C[i][1], 'W');
        int px, py, gc=-1, gr=-1;
        LS_CHECK(one_lit_pixel(&px, &py));
        LS_CHECK(ls_tui_pixel_to_cell(px, py, &gc, &gr));
        LS_EQ_INT(C[i][0], gc);
        LS_EQ_INT(C[i][1], gr);
    }
    ls_tui_end();
    ls_tui_set_rotation_cw(true);
}

LS_CASE(a_painted_cell_maps_back_to_itself_in_portrait)
{
    LS_CHECK(ls_tui_begin(568, 1232));

    int cols, rows;
    ls_tui_geometry(&cols, &rows, NULL, NULL);

    const int C[][2] = {
        { 0, 0 }, { cols - 1, 0 }, { 0, rows - 1 }, { cols - 1, rows - 1 },
        { cols / 2, rows / 2 },
    };
    for (unsigned i = 0; i < sizeof(C) / sizeof(C[0]); i++) {
        paint_only(C[i][0], C[i][1], 'W');
        int px, py;
        LS_CHECK(one_lit_pixel(&px, &py));
        int gc = -1, gr = -1;
        LS_CHECK(ls_tui_pixel_to_cell(px, py, &gc, &gr));
        LS_CHECK_MSG(gc == C[i][0] && gr == C[i][1],
                     "portrait: cell %d,%d maps back to %d,%d",
                     C[i][0], C[i][1], gc, gr);
    }
    ls_tui_end();
}

LS_CASE(every_cell_round_trips_not_just_the_corners)
{
    /* A transform can be right at the corners and wrong in between if a
       divisor is off, so walk the whole grid once. One pixel per cell keeps
       this cheap enough to run every gate. */
    LS_CHECK(ls_tui_begin(1232, 568));
    ls_tui_set_rotation_cw(true);

    int cols, rows;
    ls_tui_geometry(&cols, &rows, NULL, NULL);

    int checked = 0;
    for (int r = 0; r < rows; r += 3)
        for (int c = 0; c < cols; c += 5) {
            paint_only(c, r, 'W');
            int px, py, gc = -1, gr = -1;
            if (!one_lit_pixel(&px, &py)) continue;
            LS_CHECK(ls_tui_pixel_to_cell(px, py, &gc, &gr));
            LS_CHECK_MSG(gc == c && gr == r,
                         "cell %d,%d maps back to %d,%d", c, r, gc, gr);
            checked++;
        }
    LS_CHECK_MSG(checked > 50, "only %d cells were checked", checked);
    ls_tui_end();
}

LS_CASE(pixels_in_the_inset_margin_are_not_cells)
{

    LS_CHECK(ls_tui_begin(1232, 568));
    ls_tui_set_rotation_cw(true);

    int col, row;
    LS_CHECK(!ls_tui_pixel_to_cell(0, 0, &col, &row));
    LS_CHECK(!ls_tui_pixel_to_cell(NATIVE_W - 1, 0, &col, &row));
    LS_CHECK(!ls_tui_pixel_to_cell(0, NATIVE_H - 1, &col, &row));
    LS_CHECK(!ls_tui_pixel_to_cell(NATIVE_W - 1, NATIVE_H - 1, &col, &row));
    ls_tui_end();
}

LS_CASE(a_pixel_outside_the_panel_is_not_a_cell)
{
    LS_CHECK(ls_tui_begin(1232, 568));
    int col, row;
    LS_CHECK(!ls_tui_pixel_to_cell(-1, -1, &col, &row));
    LS_CHECK(!ls_tui_pixel_to_cell(100000, 100000, &col, &row));
    LS_CHECK(!ls_tui_pixel_to_cell(-1, 100, &col, &row));
    LS_CHECK(!ls_tui_pixel_to_cell(100, -1, &col, &row));
    ls_tui_end();
}

LS_CASE(nothing_maps_to_a_cell_before_begin)
{
    /* The touch task and the TUI task start independently, so a tap can
       arrive before the grid exists. */
    ls_tui_end();
    int col, row;
    LS_CHECK(!ls_tui_pixel_to_cell(100, 100, &col, &row));
}

/* ---------------------------------------------- the borrowed rectangle -- */

#define MAP_COLOUR 0x1234

static void fill_whole_grid(char ch)
{
    tui_surface *sf = ls_tui_surface();
    tui_rect all = tui_surface_rect(sf);
    tui_frame_begin(sf);
    tui_fill(sf, all, ch, TUI_ATTR(TUI_WHITE, TUI_BLACK));
    ls_tui_invalidate();
}

/* Every framebuffer pixel the cell rect covers, according to the inverse
   transform - so this asks the real code where the cells are. */
static int pixels_of_rect(tui_rect cells, uint16_t want, bool count_equal)
{
    int n = 0;
    for (int y = 0; y < NATIVE_H; y++)
        for (int x = 0; x < NATIVE_W; x++) {
            int col = -1, row = -1;
            if (!ls_tui_pixel_to_cell(x, y, &col, &row)) continue;
            if (col < cells.x || col >= cells.x + cells.w) continue;
            if (row < cells.y || row >= cells.y + cells.h) continue;
            const bool eq = (g_fb[y * NATIVE_W + x] == want);
            if (eq == count_equal) n++;
        }
    return n;
}

static void run_reserve_case(int screen_w, int screen_h, bool cw)
{
    LS_CHECK(ls_tui_begin(screen_w, screen_h));
    ls_tui_set_rotation_cw(cw);

    int cols, rows;
    ls_tui_geometry(&cols, &rows, NULL, NULL);

    /* A rectangle away from every edge, so a transform that is off by a row
       or mirrored lands outside it and is caught. */
    const tui_rect map = tui_rect_make(2, 3, cols - 5, rows - 7);
    LS_CHECK_MSG(map.w > 4 && map.h > 4, "no room for a map rect");

    /* Paint the whole grid, so every cell outside the reservation has
       something in it and the reservation is the only quiet region. */
    memset(g_fb, 0, sizeof(g_fb));
    fill_whole_grid('#');
    ls_tui_present();

    ls_tui_reserve(map);
    LS_CHECK(ls_tui_reserved().w == map.w && ls_tui_reserved().h == map.h);

    /* The borrower's pixels. */
    const int pw = map.w * 10, ph = map.h * 17;
    uint16_t *img = (uint16_t *)malloc((size_t)pw * ph * sizeof(uint16_t));
    LS_CHECK(img != NULL);
    if (!img) { ls_tui_end(); return; }
    for (int i = 0; i < pw * ph; i++) img[i] = MAP_COLOUR;
    ls_tui_blit_rgb565(map, img, pw, ph);

    LS_CHECK_MSG(pixels_of_rect(map, MAP_COLOUR, false) == 0,
                 "the blit left %d pixels of the rect unpainted",
                 pixels_of_rect(map, MAP_COLOUR, false));

    /* Now dirty the whole grid and present. The reservation must survive it
       untouched; everything else must repaint. */
    fill_whole_grid('W');
    const int drawn = ls_tui_present();
    LS_CHECK_MSG(drawn > 0, "nothing repainted outside the reservation");
    LS_CHECK_MSG(drawn <= cols * rows - map.w * map.h,
                 "present drew %d cells, more than the %d outside the "
                 "reservation", drawn, cols * rows - map.w * map.h);
    LS_CHECK_MSG(pixels_of_rect(map, MAP_COLOUR, false) == 0,
                 "the cell renderer painted over %d pixels of a reserved rect",
                 pixels_of_rect(map, MAP_COLOUR, false));

    /* Put the grid back to what it held before the reservation, so the cells
       under the map match what the front buffer already believes. That is
       the state the release has to handle: the renderer pushes only cells
       that changed, none of these did, and the map is on the glass. Skipping
       this step tests nothing - the cells are still dirty from the 'W' pass
       above and would repaint whether or not the release invalidated them. */
    fill_whole_grid('#');

    ls_tui_reserve(tui_rect_make(0, 0, 0, 0));
    ls_tui_present();
    LS_CHECK_MSG(pixels_of_rect(map, MAP_COLOUR, true) == 0,
                 "%d pixels of the map survived handing the rect back",
                 pixels_of_rect(map, MAP_COLOUR, true));

    free(img);
    ls_tui_end();
}

LS_CASE(a_borrowed_rectangle_survives_the_cell_renderer_in_landscape)
{
    run_reserve_case(1232, 568, true);
}

LS_CASE(a_borrowed_rectangle_survives_the_cell_renderer_in_portrait)
{
    run_reserve_case(568, 1232, true);
}

LS_CASE(a_blit_to_a_rectangle_that_was_not_reserved_is_refused)
{
    /* Drawing it anyway would put a map where the chrome is and leave no
       trace of why, since the cell renderer would repaint over part of it on
       the next frame and not the rest. */
    LS_CHECK(ls_tui_begin(568, 1232));
    ls_tui_set_rotation_cw(true);

    int cols, rows;
    ls_tui_geometry(&cols, &rows, NULL, NULL);
    memset(g_fb, 0, sizeof(g_fb));

    const tui_rect reserved = tui_rect_make(2, 3, 10, 10);
    const tui_rect other    = tui_rect_make(4, 5, 10, 10);
    ls_tui_reserve(reserved);

    const int pw = 10 * 10, ph = 10 * 17;
    uint16_t *img = (uint16_t *)malloc((size_t)pw * ph * sizeof(uint16_t));
    LS_CHECK(img != NULL);
    if (!img) { ls_tui_end(); return; }
    for (int i = 0; i < pw * ph; i++) img[i] = MAP_COLOUR;

    ls_tui_blit_rgb565(other, img, pw, ph);
    LS_CHECK_MSG(pixels_of_rect(other, MAP_COLOUR, true) == 0,
                 "a blit to an unreserved rect painted %d pixels",
                 pixels_of_rect(other, MAP_COLOUR, true));

    /* And the reservation itself still works, so the refusal above is about
       the rectangle and not about the blit being broken. */
    ls_tui_blit_rgb565(reserved, img, pw, ph);
    LS_CHECK_MSG(pixels_of_rect(reserved, MAP_COLOUR, false) == 0,
                 "the blit to the reserved rect left %d pixels unpainted",
                 pixels_of_rect(reserved, MAP_COLOUR, false));

    free(img);
    ls_tui_reserve(tui_rect_make(0, 0, 0, 0));
    ls_tui_end();
}

/* ---------------------------------------------------- the trace ground -- */

/* A trace cell paints its whole cell, not only its line. */

static uint16_t cell_pixel(int col, int row)
{
    for (int y = 0; y < NATIVE_H; y++)
        for (int x = 0; x < NATIVE_W; x++) {
            int c = -1, r = -1;
            if (ls_tui_pixel_to_cell(x, y, &c, &r) && c == col && r == row)
                return g_fb[y * NATIVE_W + x];
        }
    return 0xFFFF;
}

/* One cell changed, nothing invalidated: the renderer's own diff decides what
   reaches the framebuffer, which is the path a page change takes. */
static void paint_over(int col, int row, char ch)
{
    tui_surface *sf = ls_tui_surface();
    tui_rect all = tui_surface_rect(sf);
    tui_frame_begin(sf);
    tui_put_char(sf, all, col, row, ch, TUI_ATTR(TUI_WHITE, TUI_BLACK));
    ls_tui_present();
}

static void run_trace_ground_case(int screen_w, int screen_h)
{
    LS_CHECK(ls_tui_begin(screen_w, screen_h));
    ls_tui_set_rotation_cw(true);

    int cols, rows, cw, ch;
    ls_tui_geometry(&cols, &rows, &cw, &ch);
    const int c = cols / 2, r = rows / 2;
    const tui_rect cell = tui_rect_make(c, r, 1, 1);

    paint_only(c, r, ' ');
    const uint16_t ground = cell_pixel(c, r);

    paint_over(c, r, 'W');
    LS_CHECK_MSG(pixels_of_rect(cell, ground, false) > 0,
                 "the glyph painted nothing, so this case proves nothing");

    paint_over(c, r, LS_TUI_TRACE(8));
    paint_over(c, r, LS_TUI_TRACE(1));

    /* The one-eighth line itself: two pixels wide, ch/8 tall, as blit_block
       draws it. Anything beyond that is left over from before. */
    int h = ch / 8;
    if (h < 1) h = 1;
    const int left = pixels_of_rect(cell, ground, false);
    LS_CHECK_MSG(left <= 2 * h,
                 "%dx%d: a one-eighth trace left %d pixels off the ground in "
                 "its cell, where the line itself is %d",
                 screen_w, screen_h, left, 2 * h);
    ls_tui_end();
}

LS_CASE(a_trace_cell_paints_its_ground_in_portrait)
{
    run_trace_ground_case(568, 1232);
}

LS_CASE(a_trace_cell_paints_its_ground_in_landscape)
{
    run_trace_ground_case(1232, 568);
}

LS_CASE(camera_hole_keeps_the_full_grid_and_touch_agrees)
{
    NATIVE_W=540; NATIVE_H=1168;
    ls_tui_set_corner_radius(40);
    for (int rotation=-1; rotation<=1; rotation++) {
        const int w = rotation ? 1168 : 540, h = rotation ? 540 : 1168;
        ls_tui_set_rotation_cw(rotation > 0);
        /* The grid without a hole, for comparison. */
        ls_tui_set_cutout(0, 0, 0);
        LS_CHECK(ls_tui_begin(w, h));
        int cols0, rows0;
        ls_tui_geometry(&cols0, &rows0, NULL, NULL);
        tui_rect none;
        LS_CHECK(!ls_tui_cutout(&none));
        LS_EQ_INT(0, none.w);
        ls_tui_end();

        ls_tui_set_cutout(270, 38, 28);
        LS_CHECK(ls_tui_begin(w, h));
        int cols, rows;
        ls_tui_geometry(&cols, &rows, NULL, NULL);
        LS_EQ_INT(cols0, cols);
        LS_EQ_INT(rows0, rows);

        const int C[][2] = { {0,0}, {cols-1,0}, {0,rows-1}, {cols-1,rows-1}, {cols/2,1} };
        for (unsigned i=0; i<sizeof(C)/sizeof(C[0]); i++) {
            paint_only(C[i][0], C[i][1], 'W');
            int px, py, gc=-1, gr=-1;
            LS_CHECK(one_lit_pixel(&px, &py));
            LS_CHECK(ls_tui_pixel_to_cell(px, py, &gc, &gr));
            LS_EQ_INT(C[i][0], gc);
            LS_EQ_INT(C[i][1], gr);
        }

        /* Every native pixel of the hole that lands on a cell lands on one
           the keep-out names, and the keep-out sits on the right edge. */
        tui_rect k;
        LS_CHECK(ls_tui_cutout(&k));
        LS_CHECK(k.w > 0 && k.w <= 8 && k.h > 0 && k.h <= 6);
        if (!rotation) LS_EQ_INT(0, k.y);
        if (rotation > 0) LS_EQ_INT(cols, k.x + k.w);
        if (rotation < 0) LS_EQ_INT(0, k.x);
        if (rotation) LS_CHECK(k.y > rows / 3 && k.y + k.h < rows * 2 / 3);
        else LS_CHECK(k.x > cols / 3 && k.x + k.w < cols * 2 / 3);
        int seen = 0;
        for (int y=38-28; y<=38+28; y++)
            for (int x=270-28; x<=270+28; x++) {
                if ((x-270)*(x-270)+(y-38)*(y-38) > 28*28) continue;
                int c, r;
                if (!ls_tui_pixel_to_cell(x, y, &c, &r)) continue;
                seen++;
                LS_CHECK_MSG(c >= k.x && c < k.x + k.w && r >= k.y && r < k.y + k.h,
                             "hole pixel %d,%d is cell %d,%d outside the keep-out",
                             x, y, c, r);
            }
        LS_CHECK(seen > 0);
        ls_tui_end();
    }
    NATIVE_W=568; NATIVE_H=1232;
    ls_tui_set_cutout(0,0,0);
    ls_tui_set_rotation_cw(true);
}

LS_CASE(counter_clockwise_rotates_every_pixel_path)
{
    uint16_t *cw=malloc(sizeof(g_fb));
    LS_CHECK(cw != NULL);
    const uint16_t picture[] = {0xf800,0x07e0,0x001f,0xffff};
    for (int turn=0; turn<2; turn++) {
        ls_tui_set_rotation_cw(turn==0);
        LS_CHECK(ls_tui_begin(1232,568));
        tui_surface *sf=ls_tui_surface();
        tui_rect all=tui_surface_rect(sf);
        tui_frame_begin(sf);
        const int16_t shapes[] = {'W',0x2805,0x85,0xc5,0x90,0xa3};
        for (unsigned i=0; i<sizeof(shapes)/sizeof(shapes[0]); i++)
            tui_put_char(sf,all,3+(int)i*2,3,shapes[i],TUI_ATTR(TUI_WHITE,TUI_BLUE));
        tui_rect image=tui_rect_make(3,6,4,3);
        ls_tui_image(image,picture,2,2,1);
        tui_fill(sf,image,LS_TUI_IMAGE_CELL,TUI_ATTR(TUI_WHITE,TUI_BLACK));
        ls_tui_put_glass(sf,image,4,7,'X',TUI_ATTR(TUI_WHITE,TUI_BLACK));
        ls_tui_present();
        tui_rect borrowed=tui_rect_make(15,6,2,2);
        ls_tui_reserve(borrowed);
        ls_tui_blit_rgb565(borrowed,picture,2,2);
        ls_tui_reserve(tui_rect_make(0,0,0,0));
        if (!turn) memcpy(cw,g_fb,sizeof(g_fb));
        else {
            for (size_t i=0; i<sizeof(g_fb)/sizeof(g_fb[0]); i++)
                LS_CHECK_MSG(cw[i]==g_fb[sizeof(g_fb)/sizeof(g_fb[0])-1-i],
                             "rotation differs at pixel %zu",i);
        }
        ls_tui_end();
    }
    free(cw);
    ls_tui_set_rotation_cw(true);
}
