/* LS_TEST_SOURCES: ${FW}/components/apps/ui/ls_spectrum_waterfall_logic.c */

#include "ls_test.h"
#include "ui/ls_spectrum_waterfall_logic.h"

LS_CASE(layout_uses_available_size_and_preserves_every_pixel)
{
    ls_spectrum_layout_t l;
    LS_CHECK(ls_spectrum_layout(468, 300, 25, false, 2, &l));
    LS_EQ_INT(468, l.width);
    LS_EQ_INT(75, l.spectrum_height);
    LS_EQ_INT(225, l.waterfall_height);
    LS_EQ_INT(300, l.spectrum_height + l.waterfall_height);
    LS_EQ_UINT(468u * 300u * 2u, l.allocation_bytes);
}

LS_CASE(fullscreen_and_end_splits_have_no_phantom_canvas)
{
    ls_spectrum_layout_t l;
    LS_CHECK(ls_spectrum_layout(720, 400, 75, true, 2, &l));
    LS_CHECK(!l.spectrum_visible);
    LS_CHECK(l.waterfall_visible);
    LS_EQ_INT(400, l.waterfall_height);

    LS_CHECK(ls_spectrum_layout(480, 200, 100, false, 2, &l));
    LS_CHECK(l.spectrum_visible);
    LS_CHECK(!l.waterfall_visible);
    LS_EQ_INT(0, l.waterfall_height);
}

LS_CASE(allocation_is_bounded_and_resize_is_recomputed_not_accumulated)
{
    ls_spectrum_layout_t first, resized, rejected;
    LS_CHECK(ls_spectrum_layout(480, 300, 50, false, 2, &first));
    LS_CHECK(ls_spectrum_layout(720, 400, 50, false, 2, &resized));
    LS_EQ_UINT(720u * 400u * 2u, resized.allocation_bytes);
    LS_CHECK(resized.allocation_bytes > first.allocation_bytes);
    /* The bound is the largest panel dimension, not a height budget.
       Asserting a literal 401 pinned the old 400 row ceiling, which was
       shorter than a portrait page and left dead rows on the screen. Drive
       the edges off the constants so raising a ceiling stays a one-line
       change and the rejection behaviour is still what is under test. */
    LS_CHECK(ls_spectrum_layout(LS_SPECTRUM_CANVAS_MAX_WIDTH,
                                LS_SPECTRUM_CANVAS_MAX_HEIGHT, 50, false, 2, &resized));
    LS_EQ_UINT((unsigned)LS_SPECTRUM_CANVAS_MAX_WIDTH *
               (unsigned)LS_SPECTRUM_CANVAS_MAX_HEIGHT * 2u,
               resized.allocation_bytes);
    LS_CHECK(!ls_spectrum_layout(LS_SPECTRUM_CANVAS_MAX_WIDTH + 1,
                                 LS_SPECTRUM_CANVAS_MAX_HEIGHT, 50, false, 2, &rejected));
    LS_CHECK(!ls_spectrum_layout(720, LS_SPECTRUM_CANVAS_MAX_HEIGHT + 1,
                                 50, false, 2, &rejected));
    LS_EQ_UINT(0, rejected.allocation_bytes);
    /* A portrait page's worth of rows must now be accepted, not clamped. */
    LS_CHECK(ls_spectrum_layout(528, 700, 50, false, 2, &resized));
    LS_EQ_UINT(528u * 700u * 2u, resized.allocation_bytes);
}

LS_CASE(columns_never_read_outside_bins_even_when_upsampling)
{
    for (int columns = 1; columns <= 720; columns += 17) {
        for (int bins = 1; bins <= 240; bins += 11) {
            for (int x = 0; x < columns; ++x) {
                int lo = -1, hi = -1;
                LS_CHECK(ls_spectrum_column_bins(x, columns, bins, &lo, &hi));
                LS_CHECK(lo >= 0);
                LS_CHECK(hi > lo);
                LS_CHECK(hi <= bins);
            }
        }
    }
}

LS_CASE(no_data_and_bad_coordinates_are_rejected_without_outputs)
{
    int lo = 41, hi = 42, bin = 43;
    LS_CHECK(!ls_spectrum_column_bins(0, 480, 0, &lo, &hi));
    LS_EQ_INT(41, lo);
    LS_EQ_INT(42, hi);
    LS_CHECK(!ls_spectrum_coord_bin(-1, 480, 240, &bin));
    LS_EQ_INT(43, bin);
    LS_CHECK(!ls_spectrum_coord_bin(0, 0, 240, &bin));
}

LS_CASE(tap_coordinate_clamps_to_both_plot_edges)
{
    int bin = -1;
    LS_CHECK(ls_spectrum_coord_bin(0, 468, 240, &bin));
    LS_EQ_INT(0, bin);
    LS_CHECK(ls_spectrum_coord_bin(467, 468, 240, &bin));
    LS_EQ_INT(239, bin);
    LS_CHECK(ls_spectrum_coord_bin(900, 468, 240, &bin));
    LS_EQ_INT(239, bin);
}

LS_CASE(shared_controls_are_bounded_and_directional)
{
    LS_EQ_INT(25, ls_spectrum_split_step(50, -1));
    LS_EQ_INT(75, ls_spectrum_split_step(50, +1));
    LS_EQ_INT(0, ls_spectrum_split_step(0, -1));
    LS_EQ_INT(100, ls_spectrum_split_step(100, +1));
    LS_EQ_INT(75, ls_spectrum_contrast_step(100, -1));
    LS_EQ_INT(150, ls_spectrum_contrast_step(100, +1));
    LS_EQ_INT(50, ls_spectrum_contrast_step(50, -1));
    LS_EQ_INT(200, ls_spectrum_contrast_step(200, +1));
}

LS_CASE(no_data_resize_and_forget_lifecycle_is_explicit)
{
    ls_spectrum_buffer_state_t state = {0};
    LS_CHECK(ls_spectrum_buffer_resize(&state, 480, 300, 2));
    LS_CHECK(state.allocated);
    LS_CHECK(!state.has_data);
    LS_EQ_UINT(480u * 300u * 2u, state.allocation_bytes);

    ls_spectrum_buffer_data(&state, true);
    LS_CHECK(state.has_data);
    LS_CHECK(!ls_spectrum_buffer_resize(&state, 1233, 300, 2));
    LS_EQ_INT(480, state.width);
    LS_CHECK(state.has_data);

    LS_CHECK(ls_spectrum_buffer_resize(&state, 720, 400, 2));
    LS_CHECK(!state.has_data);
    ls_spectrum_buffer_forget(&state);
    LS_CHECK(!state.allocated);
    LS_EQ_UINT(0, state.allocation_bytes);
}
