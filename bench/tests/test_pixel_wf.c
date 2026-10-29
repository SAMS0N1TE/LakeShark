/* LS_TEST_SOURCES: ls_pixel_wf.c and ls_tui.c with the panel stubbed.

   The pixel waterfall's per-frame work is bounded, and the cheap path is
   exact: after any number of frames that moved the picture on the glass,
   the framebuffer is pixel for pixel what drawing every cell from the
   source gives. */
#include "ls_test.h"
#include "ls_tui.h"
#include "ls_panel.h"
#include "ls_pixel_wf.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint16_t g_fb[568 * 1232];
static unsigned flushed_rows, flush_calls;
bool ls_panel_fb(ls_panel_fb_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->pixels = g_fb;
    out->width = 568;
    out->height = 1232;
    return true;
}
void ls_panel_fb_present(void) {}
void ls_panel_fb_present_rows(int y0, int y1) {
    LS_CHECK(y0 >= 0 && y1 <= 1232 && y1 > y0);
    flushed_rows += y1 - y0; flush_calls++;
}

static const ls_pwf_stop_t STOPS[] = {{0, 0, 0, 0}, {80, 0, 40, 200}, {160, 0, 220, 90},
                                      {220, 250, 220, 0}, {255, 255, 255, 255}};

LS_CASE(colour_setup_tracks_contents_range_count_and_large_palettes)
{
    ls_pwf_t p = {0};
    ls_pwf_stop_t stops[17];
    for (int n = 1; n <= 17; n++) {
        for (int i = 0; i < n; i++)
            stops[i] = (ls_pwf_stop_t){(uint8_t)(i * 255 / (n > 1 ? n - 1 : 1)),
                                      (uint8_t)(i * 51), (uint8_t)(255 - i * 9), (uint8_t)(i * 17)};
        for (int change = 0; change < 8; change++) {
            const float lo = change == 1 ? -40 : change == 2 ? 40 : 0;
            const float hi = change == 3 ? 128 : change == 4 ? 0 : 255;
            /* Same storage, changed fields: retaining only the pointer loses
               live theme/palette updates from the RF caller's stack. */
            if (change >= 5) {
                stops[n / 2].r ^= 0xff;
                stops[n / 2].g ^= 0x55;
                stops[n / 2].b ^= 0xaa;
            }
            ls_pwf_t cold = {0};
            ls_pwf_colours(&cold, stops, n, lo, hi);
            uint16_t previous[256];
            memcpy(previous, p.lut, sizeof(previous));
            p.redraw = false;
            ls_pwf_colours(&p, stops, n, lo, hi);
            LS_CHECK(!memcmp(p.lut, cold.lut, sizeof(p.lut)));
            LS_EQ_INT(p.redraw, memcmp(previous, cold.lut, sizeof(previous)) != 0);
            p.redraw = false;
            ls_pwf_colours(&p, stops, n, lo, hi);
            LS_CHECK(!p.redraw && !memcmp(p.lut, cold.lut, sizeof(p.lut)));
        }
    }
    /* Stop positions can change without changing their number or address. */
    ls_pwf_colours(&p, STOPS, 5, 0, 255);
    memcpy(stops, STOPS, sizeof(STOPS));
    stops[1].at++;
    ls_pwf_t cold = {0};
    ls_pwf_colours(&cold, stops, 5, 0, 255);
    ls_pwf_colours(&p, stops, 5, 0, 255);
    LS_CHECK(!memcmp(p.lut, cold.lut, sizeof(p.lut)));
}
static uint32_t s_line;
/* A line that differs from every other: a moving tone over a textured floor. */
static void push(ls_pwf_t *p)
{
    uint8_t *line = ls_pwf_push(p);
    for (int k = 0; k < p->bins; k++)
        line[k] = (uint8_t)((k * 7 + s_line * 13) % 90);
    line[(s_line * 3) % p->bins] = 250;
    s_line++;
}
static void frame(ls_pwf_t *p, tui_rect cells, int lines, int64_t *now)
{
    tui_surface *sf = ls_tui_surface();
    for (int i = 0; i < lines; i++) push(p);
    *now += 40000;
    ls_pwf_compose(p, *now, 33000);
    tui_frame_begin(sf);
    ls_pwf_show(p, sf, cells);
    ls_tui_present();
}
static uint16_t *snapshot(void)
{
    uint16_t *copy = malloc(sizeof(g_fb));
    if (copy) memcpy(copy, g_fb, sizeof(g_fb));
    return copy;
}

/* Run `frames` frames of `lines` new lines each in one orientation and
   flow, and hold the moved glass to a full redraw. */
static void exact_and_bounded(bool landscape, bool cw, ls_pwf_flow_t flow, int lines)
{
    ls_tui_set_rotation_cw(cw);
    LS_CHECK(landscape ? ls_tui_begin(1232, 568) : ls_tui_begin(568, 1232));
    int cols, rows, cwid, chgt;
    ls_tui_geometry(&cols, &rows, &cwid, &chgt);
    const tui_rect cells = landscape ? tui_rect_make(6, 3, cols - 40, rows - 8)
                                     : tui_rect_make(4, 10, cols - 8, 30);
    ls_pwf_t p;
    memset(&p, 0, sizeof(p));
    LS_CHECK(ls_pwf_size(&p, flow, cells.w * cwid, cells.h * chgt, 300));
    ls_pwf_colours(&p, STOPS, 5, 0, 255);
    int64_t now = 0;
    s_line = 0;
    for (int i = 0; i < 4; i++) frame(&p, cells, lines, &now);
    /* A clean present between new rows must preserve the ring's shown-state,
       so the next row can still use the exact framebuffer shift path. */
    now += 40000;
    tui_frame_begin(ls_tui_surface());
    ls_pwf_show(&p, ls_tui_surface(), cells);
    LS_EQ_INT(ls_tui_present(), 0);
    /* Steady state: only the cells the new lines land in are drawn, and only
       the new lines are turned into pixels. */
    const int per_line = flow == LS_PWF_LEFT ? cwid : chgt;
    const int edge = flow == LS_PWF_LEFT ? cells.h : cells.w;
    const int want = edge * ((lines + per_line - 1) / per_line + 1);
    int worst = 0;
    for (int i = 0; i < 40; i++) {
        frame(&p, cells, lines, &now);
        uint32_t us; int drawn;
        ls_tui_last_cost(&us, &drawn);
        if (drawn > worst) worst = drawn;
        LS_EQ_INT((int)p.stat_px, lines * p.freq_px);
        uint32_t copied, flushed;
        ls_tui_last_traffic(&copied, &flushed);
        LS_EQ_UINT(copied, (uint32_t)p.w * p.h * 2);
        LS_EQ_UINT(flushed, (uint32_t)(landscape ? p.w : p.h) * 568 * 2);
        if (i == 0 && flow == LS_PWF_DOWN)
            printf("    %s %dx%d, %d new rows: compose=%lu B copy=%lu B flush=%lu B; at 10 Hz copy=%lu B/s flush=%lu B/s\n",
                   landscape ? (cw ? "CW" : "CCW") : "portrait", p.w, p.h, lines,
                   (unsigned long)p.stat_px * 2, (unsigned long)copied, (unsigned long)flushed,
                   (unsigned long)copied * 10, (unsigned long)flushed * 10);
    }
    LS_CHECK_MSG(worst <= want, "%s %s: %d cells drawn a frame, bound %d of %d",
                 landscape ? (cw ? "landscape cw" : "landscape ccw") : "portrait",
                 flow == LS_PWF_LEFT ? "LEFT" : "DOWN", worst, want, cells.w * cells.h);
    uint16_t *moved = snapshot();
    ls_tui_invalidate();
    tui_frame_begin(ls_tui_surface());
    ls_pwf_show(&p, ls_tui_surface(), cells);
    ls_tui_present();
    LS_CHECK(moved && !memcmp(moved, g_fb, sizeof(g_fb)));
    free(moved);
    ls_pwf_free(&p);
    ls_tui_end();
    ls_tui_set_rotation_cw(true);
}
LS_CASE(left_portrait_moves_exactly){exact_and_bounded(false, true, LS_PWF_LEFT, 4);}
LS_CASE(left_landscape_cw_moves_exactly){exact_and_bounded(true, true, LS_PWF_LEFT, 4);}
LS_CASE(left_landscape_ccw_moves_exactly){exact_and_bounded(true, false, LS_PWF_LEFT, 3);}
LS_CASE(down_portrait_moves_exactly){exact_and_bounded(false, true, LS_PWF_DOWN, 2);}
LS_CASE(down_landscape_cw_moves_exactly){exact_and_bounded(true, true, LS_PWF_DOWN, 1);}
LS_CASE(down_landscape_ccw_moves_exactly){exact_and_bounded(true, false, LS_PWF_DOWN, 5);}

LS_CASE(clean_present_preserves_ring_and_overlay_forces_repaint)
{
    LS_CHECK(ls_tui_begin(568, 1232));
    int cols, rows, cw, ch;
    ls_tui_geometry(&cols, &rows, &cw, &ch);
    const tui_rect cells = tui_rect_make(4, 10, cols - 8, 30);
    ls_pwf_t p = {0};
    LS_CHECK(ls_pwf_size(&p, LS_PWF_DOWN, cells.w * cw, cells.h * ch, 300));
    ls_pwf_colours(&p, STOPS, 5, 0, 255);
    int64_t now = 0;
    for (int i = 0; i < 4; i++) frame(&p, cells, 2, &now);

    /* Holding the same serial is an inactive/clean waterfall present. */
    tui_frame_begin(ls_tui_surface());
    ls_pwf_show(&p, ls_tui_surface(), cells);
    LS_EQ_INT(ls_tui_present(), 0);

    /* Leaving the waterfall clears its cell region; reentering with the same
       image serial restores the baseline without requiring a new IQ row. */
    tui_frame_begin(ls_tui_surface());
    tui_put_char(ls_tui_surface(), tui_surface_rect(ls_tui_surface()), 2, 2, 'H', 7);
    LS_CHECK(ls_tui_present() >= cells.w * cells.h);
    tui_frame_begin(ls_tui_surface());
    ls_pwf_show(&p, ls_tui_surface(), cells);
    LS_CHECK(ls_tui_present() >= cells.w * cells.h);

    /* A cell drawn over the ring invalidates its shift baseline. */
    push(&p); now += 40000; ls_pwf_compose(&p, now, 33000);
    tui_frame_begin(ls_tui_surface());
    ls_pwf_show(&p, ls_tui_surface(), cells);
    tui_put_char(ls_tui_surface(), cells, cells.x + 2, cells.y + 2, 'X', 7);
    ls_tui_present();

    /* The next new row restores image cells but must repaint before shifting. */
    push(&p); now += 40000; ls_pwf_compose(&p, now, 33000);
    tui_frame_begin(ls_tui_surface());
    ls_pwf_show(&p, ls_tui_surface(), cells);
    ls_tui_present();
    uint32_t copied, flushed; ls_tui_last_traffic(&copied, &flushed);
    uint32_t us; int drawn; ls_tui_last_cost(&us, &drawn);
    LS_CHECK(drawn >= cells.w * cells.h);

    /* Once the complete image is back, the following row may shift exactly. */
    push(&p); now += 40000; ls_pwf_compose(&p, now, 33000);
    tui_frame_begin(ls_tui_surface());
    ls_pwf_show(&p, ls_tui_surface(), cells);
    ls_tui_present();
    ls_tui_last_traffic(&copied, &flushed);
    ls_tui_last_cost(&us, &drawn);
    LS_CHECK(drawn < cells.w * cells.h / 4);
    LS_EQ_UINT(copied, (uint32_t)p.w * p.h * 2);

    ls_pwf_free(&p);
    ls_tui_end();
}

LS_CASE(iq_cadence_coalesces_even_more_than_a_ring){
    ls_pwf_t p = {0};
    LS_CHECK(ls_pwf_size(&p, LS_PWF_DOWN, 40, 12, 30));
    ls_pwf_colours(&p, STOPS, 5, 0, 255);
    ls_pwf_compose(&p, 100000, 100000);
    const uint32_t serial = p.serial;
    for (int i = 0; i < 30; i++) push(&p);
    ls_pwf_compose(&p, 199999, 100000);
    LS_EQ_UINT(p.serial, serial); LS_EQ_UINT(p.stat_px, 0);
    ls_pwf_compose(&p, 200000, 100000);
    LS_EQ_UINT(p.serial, serial + 1); LS_EQ_UINT(p.stat_px, 40 * 12);
    uint16_t expected[40 * 12]; memcpy(expected, p.img, sizeof(expected));
    p.redraw = true;
    ls_pwf_compose(&p, 200001, 100000);
    LS_CHECK(!memcmp(expected, p.img, sizeof(expected)));
    ls_pwf_free(&p);
}

LS_CASE(present_skips_clean_rows_between_changes){
    LS_CHECK(ls_tui_begin(568, 1232));
    tui_surface *sf = ls_tui_surface();
    tui_frame_begin(sf); ls_tui_present();
    int cols, rows, cw, ch; ls_tui_geometry(&cols, &rows, &cw, &ch);
    tui_frame_begin(sf);
    tui_put_char(sf, tui_surface_rect(sf), 8, 8, 'A', 7);
    tui_put_char(sf, tui_surface_rect(sf), 8, 38, 'B', 7);
    flushed_rows = flush_calls = 0;
    ls_tui_present();
    LS_EQ_UINT(flush_calls, 2); LS_EQ_UINT(flushed_rows, 2 * ch);
    uint32_t copied, flushed; ls_tui_last_traffic(&copied, &flushed);
    LS_EQ_UINT(flushed, 2 * ch * 568 * 2);
    ls_tui_end();
}

/* A change of colours redraws everything, and is still exact afterwards. */
LS_CASE(new_colours_draw_every_cell_then_move_again){
    LS_CHECK(ls_tui_begin(568, 1232));
    int cols, rows, cwid, chgt;
    ls_tui_geometry(&cols, &rows, &cwid, &chgt);
    const tui_rect cells = tui_rect_make(4, 10, cols - 8, 30);
    ls_pwf_t p;
    memset(&p, 0, sizeof(p));
    LS_CHECK(ls_pwf_size(&p, LS_PWF_LEFT, cells.w * cwid, cells.h * chgt, 300));
    ls_pwf_colours(&p, STOPS, 5, 0, 255);
    int64_t now = 0;
    for (int i = 0; i < 5; i++) frame(&p, cells, 4, &now);
    ls_pwf_colours(&p, STOPS, 5, 40, 200);
    frame(&p, cells, 4, &now);
    uint32_t us; int drawn;
    ls_tui_last_cost(&us, &drawn);
    LS_CHECK(drawn >= cells.w * cells.h);
    for (int i = 0; i < 3; i++) frame(&p, cells, 4, &now);
    ls_tui_last_cost(&us, &drawn);
    LS_CHECK(drawn <= cells.h * 2);
    uint16_t *moved = snapshot();
    ls_tui_invalidate();
    tui_frame_begin(ls_tui_surface());
    ls_pwf_show(&p, ls_tui_surface(), cells);
    ls_tui_present();
    LS_CHECK(moved && !memcmp(moved, g_fb, sizeof(g_fb)));
    free(moved);
    ls_pwf_free(&p);
    ls_tui_end();
}

/* The time a frame takes, against the old way of drawing every cell of the
   picture from the source. The board's 27 ms present was the full draw. */
static double frames_ms(bool full, int n)
{
    LS_CHECK(ls_tui_begin(568, 1232));
    int cols, rows, cwid, chgt;
    ls_tui_geometry(&cols, &rows, &cwid, &chgt);
    const tui_rect cells = tui_rect_make(3, 10, 46, 30);
    ls_pwf_t p;
    memset(&p, 0, sizeof(p));
    ls_pwf_size(&p, LS_PWF_LEFT, cells.w * cwid, cells.h * chgt, 1024);
    ls_pwf_colours(&p, STOPS, 5, 0, 255);
    int64_t now = 0;
    for (int i = 0; i < 3; i++) frame(&p, cells, 4, &now);
    const clock_t t0 = clock();
    for (int i = 0; i < n; i++) {
        if (full) ls_pwf_axis_changed(&p);
        frame(&p, cells, 4, &now);
    }
    const double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / n;
    ls_pwf_free(&p);
    ls_tui_end();
    return ms;
}
LS_CASE(a_moving_frame_costs_a_fraction_of_a_full_draw){
    const double moving = frames_ms(false, 200), full = frames_ms(true, 200);
    printf("    460x510 waterfall, 4 lines a frame: %.3f ms moving, %.3f ms full\n",
           moving, full);
    LS_CHECK_MSG(moving * 3.0 < full, "moving %.3f ms vs full %.3f ms", moving, full);
}

#include "switch_budget.h"
LS_CASE(reserved_waterfall_resizes_without_allocating_or_showing_old_rows)
{
    ls_pwf_t p = {0};
    LS_CHECK(ls_pwf_reserve(&p, 568u * 1232, 1232u * 256, 1232));
    uint16_t *image = p.img;
    switch_budget_begin();
    for (int i = 0; i < 4; ++i) {
        LS_CHECK(ls_pwf_size(&p, LS_PWF_DOWN, i & 1 ? 1100 : 520, i & 1 ? 400 : 1000, 256));
        memset(p.hist, 255, p.hist_capacity);
        LS_EQ_INT(p.filled, 0);
        ls_pwf_compose(&p, 1000000 + i * 100000, 0);
        LS_CHECK(p.img == image);
        for (int k = 0; k < p.w * p.h; ++k) LS_EQ_INT(p.img[k], p.lut[0]);
    }
    LS_EQ_UINT(switch_budget_end(), 0);
    ls_pwf_free(&p);
}
