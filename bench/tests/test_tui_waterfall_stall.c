/* LS_TEST_SOURCES: ls_waterfall.c alone */

#include "ls_test.h"
#include "ls_waterfall.h"
#include "ls_tui_screen.h"
#include "esp_timer.h"

#include <string.h>

bool ls_tui_is_wide(void) { return true; }
/* Asked by the draw path for which way round the ground is. The
   blitter that knows is not linked here: dark. */
bool ls_tui_daylight(void) { return false; }

static tui_cell g_back[64 * 24], g_front[64 * 24];

/* Draw the instrument into a 64x24 pane and say whether `needle` appears on
   any row of it. */
static bool pane_says(const char *needle)
{
    tui_surface sf;
    tui_surface_setup(&sf, g_back, g_front, 64, 24);
    tui_frame_begin(&sf);
    ls_wf_draw_mini(&sf, tui_rect_make(0, 0, 64, 24));
    for (int y = 0; y < 24; y++) {
        char line[65];
        for (int x = 0; x < 64; x++) {
            const tui_cell *c = &g_back[y * 64 + x];
            line[x] = (c->ch >= 32 && c->ch < 127) ? (char)c->ch : ' ';
        }
        line[64] = 0;
        if (strstr(line, needle)) return true;
    }
    return false;
}

static void push_row(ls_wf_owner_t owner, uint32_t period_ms)
{
    float bins[128];
    for (int i = 0; i < 128; i++) bins[i] = (i == 64) ? 0.8f : 0.1f;
    const ls_wf_feed_t f = {
        .center_hz = 156000000u, .span_hz = 12000000u,
        .floor_db = -90.0f, .top_db = 0.0f, .live = true, .note = "sweep",
        .period_ms = period_ms,
    };
    ls_wf_push(owner, bins, 128, &f);
}

LS_CASE(a_sweep_that_takes_eleven_seconds_is_not_called_stalled_after_five)
{
    /* A zero clock reads as "never pushed", so the case starts somewhere. */
    ls_shim_time_set(10000000);
    ls_wf_claim(LS_WF_OWNER_FM, "FM");
    push_row(LS_WF_OWNER_FM, 11520);
    push_row(LS_WF_OWNER_FM, 11520);

    ls_shim_time_advance(5000000);
    LS_CHECK_MSG(!pane_says("no rows for"),
                 "5 s into an 11.5 s sweep the waterfall called it starved");

    /* Twice the period and more is starved, and still says so. */
    ls_shim_time_advance(20000000);
    LS_CHECK_MSG(pane_says("no rows for"),
                 "25 s with no row from an 11.5 s sweep went unreported");
}

LS_CASE(a_source_that_names_no_period_is_called_stalled_at_one_and_a_half)
{
    /* P25 names none and its rows are 56 ms apart: what it had, it keeps. A
       different owner, so this case starts from a cleared history whatever
       ran before it. */
    ls_shim_time_set(100000000);
    ls_wf_claim(LS_WF_OWNER_P25, "P25");
    push_row(LS_WF_OWNER_P25, 0);
    push_row(LS_WF_OWNER_P25, 0);

    ls_shim_time_advance(1000000);
    LS_CHECK(!pane_says("no rows for"));
    ls_shim_time_advance(1000000);
    LS_CHECK_MSG(pane_says("no rows for"),
                 "2 s with no row from a source that names no period went "
                 "unreported");
}

LS_CASE(a_slow_source_reports_its_row_rate_from_the_second_row)
{
    /* The rate was published only once sixteen intervals had been
       averaged, so an FM band sweep landing a row every 18 s - measured on
       the board over VHF land - read "a row every 0 ms" for five minutes
       while it worked. Two rows is one interval, and that is the rate. FM
       again after the P25 case above, so the history starts cleared. */
    ls_shim_time_set(200000000);
    ls_wf_claim(LS_WF_OWNER_FM, "FM");
    ls_wf_stats_t st;

    push_row(LS_WF_OWNER_FM, 18000);
    ls_wf_stats(&st);
    LS_EQ_INT(0, (int)st.row_ms);           /* one row is no interval yet */

    ls_shim_time_advance(18000000);
    push_row(LS_WF_OWNER_FM, 18000);
    ls_wf_stats(&st);
    LS_EQ_INT(18000, (int)st.row_ms);

    /* An average, not the last interval: 18 s and then 20 s is 19 s. */
    ls_shim_time_advance(20000000);
    push_row(LS_WF_OWNER_FM, 18000);
    ls_wf_stats(&st);
    LS_EQ_INT(19000, (int)st.row_ms);
}
