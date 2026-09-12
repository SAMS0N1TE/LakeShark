/* LS_TEST_SOURCES: ls_waterfall.c alone */

#include "ls_test.h"
#include "ls_waterfall.h"
#include "ls_tui_screen.h"
#include "ls_theme.h"

#include <math.h>
#include <string.h>

bool ls_tui_is_wide(void) { return true; }
/* The waterfall asks the blitter which way round the ground is, and
   the blitter is not linked here. Dark, the way every theme but one is. */
bool ls_tui_daylight(void) { return false; }

/* The light twins of the four scales, held to the real Daylight palette. */

static double lum565(uint16_t c)
{
    const double ch[3] = { ((c >> 11) & 0x1F) / 31.0,
                           ((c >> 5)  & 0x3F) / 63.0,
                           ( c        & 0x1F) / 31.0 };
    double l[3];
    for (int i = 0; i < 3; i++)
        l[i] = ch[i] <= 0.03928 ? ch[i] / 12.92
                                : pow((ch[i] + 0.055) / 1.055, 2.4);
    return 0.2126 * l[0] + 0.7152 * l[1] + 0.0722 * l[2];
}

LS_CASE(the_daylight_scales_run_from_the_ground_to_the_ink)
{
    const uint16_t *pal = ls_theme_daylight.palette;
    for (int p = 0; p < LS_WF_PAL__COUNT; p++) {
        LS_EQ_INT(TUI_BLACK, ls_wf_level_colour(p, 0, true));
        LS_EQ_INT(TUI_BLACK | TUI_BRIGHT, ls_wf_level_colour(p, 1, true));
        LS_EQ_INT(TUI_WHITE | TUI_BRIGHT, ls_wf_level_colour(p, 15, true));

        double prev = lum565(pal[ls_wf_level_colour(p, 0, true) & 0x0F]);
        for (int lvl = 1; lvl < 16; lvl++) {
            const double l = lum565(pal[ls_wf_level_colour(p, lvl, true) & 0x0F]);
            LS_CHECK_MSG(l <= prev,
                         "scale %d: level %d is lighter on white than level %d "
                         "(%.3f against %.3f)", p, lvl, lvl - 1, l, prev);
            prev = l;
        }
    }
}

LS_CASE(the_dark_scales_are_what_they_were)
{

    for (int p = 0; p < LS_WF_PAL__COUNT; p++) {
        LS_EQ_INT(TUI_BLACK, ls_wf_level_colour(p, 0, false));
        LS_CHECK(ls_wf_level_colour(p, 1, false) != (TUI_BLACK | TUI_BRIGHT));
        LS_EQ_INT(TUI_WHITE | TUI_BRIGHT, ls_wf_level_colour(p, 15, false));
    }
    /* HEAT, the default, pinned whole. */
    static const uint8_t HEAT[16] = {
        TUI_BLACK, TUI_BLUE, TUI_BLUE, TUI_BLUE | TUI_BRIGHT,
        TUI_BLUE | TUI_BRIGHT, TUI_CYAN, TUI_CYAN, TUI_CYAN | TUI_BRIGHT,
        TUI_GREEN | TUI_BRIGHT, TUI_GREEN | TUI_BRIGHT, TUI_YELLOW,
        TUI_YELLOW | TUI_BRIGHT, TUI_YELLOW | TUI_BRIGHT,
        TUI_RED | TUI_BRIGHT, TUI_RED | TUI_BRIGHT, TUI_WHITE | TUI_BRIGHT };
    for (int l = 0; l < 16; l++)
        LS_EQ_INT(HEAT[l], ls_wf_level_colour(LS_WF_PAL_HEAT, l, false));

    LS_EQ_INT(HEAT[7], ls_wf_level_colour(99, 7, false));
}

static const ls_wf_feed_t FEED = {
    .center_hz = 851012500u, .span_hz = 240000u,
    .floor_db = -85.0f, .top_db = -20.0f, .live = true, .note = NULL,
};

/* A band shaped like a real one: a low floor with a little spread in it, and
   one carrier well above. The floor is at about a twelfth of full scale
   because that is where P25_SPECTRUM_FLOOR_DB and TOP_DB put a receiver
   sitting on -80 dBm, and the whole point of the fix is that a floor down
   there still has to be visible. */
static void band(float *out, int n, int carrier_bin)
{
    for (int i = 0; i < n; i++) {
        /* The floor varies with a period of 32 bins.

           It varied every 23 bins at first and that made this fixture
           useless: the instrument resamples to the display by MAX, so four
           bins collapse into one column, and a ripple that turns over inside
           four bins reads as a flat line at its own maximum. The floor came
           out one flat colour and the assertion below fired against a build
           that was working. Thirty-two bins is eight columns at the width
           this test draws, which survives the resample. A real receiver's
           floor tilts and undulates at that sort of scale; one that changed
           every other bin would be a signal, not a floor. */
        const int p = i % 32;
        const float ripple = (p < 16) ? (float)p / 16.0f
                                      : (float)(32 - p) / 16.0f;
        out[i] = 0.045f + 0.035f * ripple;
        if (carrier_bin >= 0 && i > carrier_bin - 3 && i < carrier_bin + 3)
            out[i] = 0.80f;
    }
}

static void feed(int n, int rows, int carrier_bin)
{
    float bins[LS_WF_BINS_MAX];
    band(bins, n, carrier_bin);
    for (int r = 0; r < rows; r++) ls_wf_push(LS_WF_OWNER_P25, bins, n, &FEED);
}

static tui_cell g_back[64 * 24], g_front[64 * 24];

/* Draw the history alone into a 64x24 pane and report how many distinct
   colours a carrier-free stretch of it came out in. One colour is the fault:
   it means every bin in the band landed on the same level. */
static int floor_colours(int lo_col, int hi_col, int *out_drawn)
{
    tui_surface sf;
    tui_surface_setup(&sf, g_back, g_front, 64, 24);
    tui_frame_begin(&sf);

    /* All waterfall, no spectrum. The default split gives half the pane to
       the spectrum, whose bars are SHORT when the floor is low - correctly
       so - and counting those blank rows as unpainted history is how the
       first version of this assertion failed against a working build. */
    ls_wf_cfg_t cfg = *ls_wf_cfg();
    cfg.split_pct = 0;
    ls_wf_cfg_set(&cfg);

    ls_wf_draw_mini(&sf, tui_rect_make(0, 0, 64, 24));

    uint8_t seen[256];
    memset(seen, 0, sizeof(seen));
    int colours = 0, drawn = 0;
    for (int y = 0; y < 24; y++) {
        for (int x = lo_col; x <= hi_col; x++) {
            const tui_cell *c = &g_back[y * 64 + x];
            if (c->ch == ' ') continue;
            drawn++;
            if (!seen[c->attr]) { seen[c->attr] = 1; colours++; }
        }
    }
    if (out_drawn) *out_drawn = drawn;
    return colours;
}

LS_CASE(the_waterfall_scales_to_the_band_and_forgets_a_resized_history)
{
    ls_wf_stats_t st;

    ls_wf_claim(LS_WF_OWNER_P25, "P25");

    /* ---- a history at one resolution ---------------------------------- */
    feed(192, 40, 96);
    ls_wf_stats(&st);
    LS_CHECK_MSG(st.hist_rows >= 40,
                 "40 rows at 192 bins left %d rows of history", st.hist_rows);

    /* ---- the same source, a different resolution ----------------------
       The history it wrote is unreadable at the new scale, so it goes.
       Keeping it is what drew the short waterfall. */
    feed(256, 1, 128);
    ls_wf_stats(&st);
    LS_CHECK_MSG(st.hist_rows == 1,
                 "a resolution change kept %d rows of a history written at "
                 "another bin count - they will be read at the wrong scale",
                 st.hist_rows);

    /* ---- and it is drawable again once it has been fed ----------------
       Enough rows to fill the pane twice over: `fine` packs two history rows
       into every cell, so a 24 row pane wants 48 and forty of them left four
       rows of it honestly blank - which the first version of the assertion
       below read as a mapping fault. */
    feed(256, 110, 128);
    LS_CHECK_MSG(ls_wf_idle_reason() == NULL,
                 "after a resolution change the waterfall says '%s'",
                 ls_wf_idle_reason() ? ls_wf_idle_reason() : "");

    const int lo_col = 2, hi_col = 20;
    int drawn = 0;
    int colours = floor_colours(lo_col, hi_col, &drawn);

    ls_wf_stats(&st);

    /* Printed on a pass as well as a failure: the scale is automatic, and
       a run that says which window it chose is the difference between
       "this test is green" and "this test is green for the right reason". */
    ls_wf_stats(&st);
    printf("    [scale lo=%u hi=%u top=%u rows=%d]\n",
           st.scale_lo, st.scale_hi, st.scale_top, st.hist_rows);

    const int floor_cells = 24 * (hi_col - lo_col + 1);
    LS_CHECK_MSG(drawn >= floor_cells - 4,
                 "the noise floor drew %d of %d cells of an all-history pane "
                 "- it is being mapped below the lowest level and left blank",
                 drawn, floor_cells);
    LS_CHECK_MSG(colours >= 2,
                 "the noise floor drew in %d colour: the whole floor is "
                 "collapsed into one level and the waterfall is a flat sheet",
                 colours);

    /* ---- and it is right from the FIRST rows, not eventually ---------- */

    ls_wf_claim(LS_WF_OWNER_FM, "FM");
    ls_wf_stats(&st);
    LS_CHECK_MSG(st.hist_rows == 0, "a fresh claim kept %d rows", st.hist_rows);

    float bins[LS_WF_BINS_MAX];
    band(bins, 256, 128);
    for (int r = 0; r < 3; r++) ls_wf_push(LS_WF_OWNER_FM, bins, 256, &FEED);

    colours = floor_colours(lo_col, hi_col, &drawn);
    LS_CHECK_MSG(colours >= 2,
                 "three rows after a claim the floor is in %d colour - the "
                 "scale has not caught up and the band reads as saturated",
                 colours);

    ls_wf_stats(&st);
    LS_CHECK_MSG(st.scale_hi > st.scale_lo,
                 "the colour window is inverted: lo=%u hi=%u",
                 st.scale_lo, st.scale_hi);
    LS_CHECK_MSG(st.scale_top >= st.scale_hi,
                 "the spectrum's top (%u) is below the colour window's (%u), "
                 "so a bar can saturate while its colour does not",
                 st.scale_top, st.scale_hi);
}

LS_CASE(a_written_row_keeps_its_colours_when_the_scale_moves)
{
    ls_wf_claim(LS_WF_OWNER_REC, "REC");

    ls_wf_cfg_t cfg = *ls_wf_cfg();
    cfg.split_pct = 0;
    cfg.grain = LS_WF_GRAIN_ASCII;
    cfg.avg = 1;
    cfg.decim = 1;
    cfg.paused = false;
    ls_wf_cfg_set(&cfg);

    float quiet[LS_WF_BINS_MAX], loud[LS_WF_BINS_MAX];
    band(quiet, 256, 128);
    for (int i = 0; i < 256; i++)
        loud[i] = 0.55f + 0.20f * (float)(i % 7) / 7.0f;

    for (int r = 0; r < 12; r++) ls_wf_push(LS_WF_OWNER_REC, quiet, 256, &FEED);

    tui_surface sf;
    tui_surface_setup(&sf, g_back, g_front, 64, 24);
    tui_frame_begin(&sf);
    ls_wf_draw_mini(&sf, tui_rect_make(0, 0, 64, 24));

    static tui_cell before[64 * 24];
    memcpy(before, g_back, sizeof(before));
    int drawn = 0;
    for (int i = 0; i < 12 * 64; i++) if (before[i].ch != ' ') drawn++;
    LS_CHECK_MSG(drawn > 64,
                 "the quiet rows drew only %d cells, so this case proves "
                 "nothing", drawn);

    /* Newest is the top row, so the old rows move down by one per push. */
    const int NEW = 3;
    for (int r = 0; r < NEW; r++) ls_wf_push(LS_WF_OWNER_REC, loud, 256, &FEED);

    tui_frame_begin(&sf);
    ls_wf_draw_mini(&sf, tui_rect_make(0, 0, 64, 24));

    int changed = 0;
    for (int y = 0; y < 12; y++)
        for (int x = 0; x < 64; x++) {
            const tui_cell *a = &before[y * 64 + x];
            const tui_cell *b = &g_back[(y + NEW) * 64 + x];
            if (a->ch != b->ch || a->attr != b->attr) changed++;
        }

    ls_wf_stats_t st;
    ls_wf_stats(&st);
    LS_CHECK_MSG(changed == 0,
                 "%d cells of rows written before the loud ones changed when "
                 "the automatic window moved to lo=%u hi=%u - the written "
                 "history is being re-coloured", changed,
                 st.scale_lo, st.scale_hi);
}
