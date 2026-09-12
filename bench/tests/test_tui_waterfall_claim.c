/* LS_TEST_SOURCES: ls_waterfall.c alone */

#include "ls_test.h"
#include "ls_waterfall.h"
#include "ls_tui_screen.h"

#include <string.h>

/* Never reached: this case does not draw. The linker wants every symbol the
   instrument's draw path names, and orientation is not what is under test. */
bool ls_tui_is_wide(void) { return true; }
/* Asked by the draw path for which way round the ground is. */
bool ls_tui_daylight(void) { return false; }

LS_CASE(the_waterfall_reports_each_reason_it_has_nothing_to_draw)
{
    ls_wf_stats_t st;

    /* ---- cold: nothing has ever asked for the instrument -------------- */
    ls_wf_stats(&st);
    LS_CHECK_MSG(!st.ready,
                 "something claimed the waterfall before this case ran - "
                 "this file must hold one case and no more");

    const char *why = ls_wf_idle_reason();
    LS_CHECK_MSG(why != NULL, "an unclaimed waterfall claims to be drawable");
    if (why) {
        LS_CHECK_MSG(strstr(why, "PSRAM") == NULL,
                     "an unclaimed waterfall blames memory: '%s'", why);
        LS_CHECK_MSG(strstr(why, "receiver") != NULL,
                     "an unclaimed waterfall says '%s'", why);
    }

    /* ---- claimed: the buffer is taken, nothing pushed yet ------------- */
    ls_wf_claim(LS_WF_OWNER_P25, "P25");
    ls_wf_stats(&st);
    LS_CHECK_MSG(st.ready, "claiming the waterfall did not take its history");
    LS_CHECK_MSG(st.hist_rows == 0,
                 "a freshly claimed waterfall reports %d rows of history",
                 st.hist_rows);

    why = ls_wf_idle_reason();
    LS_CHECK_MSG(why != NULL, "a waterfall with no rows claims to be drawable");
    if (why) {
        LS_CHECK_MSG(strstr(why, "PSRAM") == NULL,
                     "a claimed waterfall blames memory: '%s'", why);
        LS_CHECK_MSG(strstr(why, "waiting") != NULL,
                     "a claimed but unfed waterfall says '%s'", why);
    }

    /* ---- fed: there is something to draw ------------------------------ */
    float bins[64];
    for (int i = 0; i < 64; i++) bins[i] = (float)(i % 8) / 8.0f;
    const ls_wf_feed_t f = {
        .center_hz = 851000000u, .span_hz = 240000u,
        .floor_db = -110.0f, .top_db = -20.0f, .live = true, .note = NULL,
    };
    ls_wf_preview(LS_WF_OWNER_P25, bins, 64, &f);
    ls_wf_stats(&st);
    LS_EQ_INT(0, st.hist_rows);
    LS_CHECK(ls_wf_idle_reason() == NULL);
    ls_wf_push(LS_WF_OWNER_P25, bins, 64, &f);

    ls_wf_stats(&st);
    LS_CHECK_MSG(st.hist_rows > 0, "a pushed row did not reach the history");
    LS_CHECK_MSG(ls_wf_idle_reason() == NULL,
                 "a fed waterfall still says '%s'",
                 ls_wf_idle_reason() ? ls_wf_idle_reason() : "");
}
