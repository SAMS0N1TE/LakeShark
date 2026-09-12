/* LS_TEST_SOURCES: ${FW}/components/apps/map_gui/map_tile_status.c */
/**/

#include "ls_test.h"
#include "map_gui/map_tile_status.h"

#include <string.h>

static map_tile_probe_t base(void)
{
    /* Baseline: renderer up, home set, SD in, pack present, zoom present,
       full 3x3 files, and all nine drew.  Any state under test is one field
       flipped from this - the classifier's ordering is the interesting
       thing, not the plumbing that fills the struct. */
    map_tile_probe_t p;
    memset(&p, 0, sizeof(p));
    p.renderer_ok   = true;
    p.home_set      = true;
    p.sd_mounted    = true;
    p.pack_present  = true;
    p.zoom_present  = true;
    p.files_present = 9;
    p.drawn         = 9;
    return p;
}

LS_CASE(happy_path_reports_ok_and_gives_a_short_text)
{
    /* Successful tile set: OK, and the overlay text is short enough that it
       won't fight the rings for space when it does appear (it won't here -
       the AppMap side hides the label on OK - but the text still has to be
       short for the non-OK paths). */
    map_tile_probe_t p = base();
    map_tile_state_t s = map_tile_classify(&p);
    LS_EQ_INT(s, MAP_TILE_STATE_OK);
    const char *t = map_tile_state_text(s);
    LS_CHECK(t != NULL);
    LS_CHECK_MSG(strlen(t) <= 40, "OK text too long: [%s]", t);
}

LS_CASE(partial_coverage_still_counts_as_ok)
{
    /* Edge of the pack: some tiles decoded, some didn't exist.  The user
       is looking at real map; reporting NO_COVERAGE while eight tiles are
       on screen would be the very lying-to-the-user this fix removes. */
    map_tile_probe_t p = base();
    p.files_present = 8;
    p.drawn         = 8;
    LS_EQ_INT(map_tile_classify(&p), MAP_TILE_STATE_OK);
}

LS_CASE(missing_pack_reports_no_pack)
{
    /* The task's core scenario: SD is in, but /sdcard/tiles is absent.
       Before the fix this looked identical to a broken renderer.  The
       overlay text has to say what is wrong AND suggest where to look,
       so it should mention the pack path or an equivalent hint. */
    map_tile_probe_t p = base();
    p.pack_present = false;
    p.zoom_present = false;
    p.files_present = 0;
    p.drawn = 0;
    map_tile_state_t s = map_tile_classify(&p);
    LS_EQ_INT(s, MAP_TILE_STATE_NO_PACK);
    const char *t = map_tile_state_text(s);
    LS_CHECK(t != NULL);
    LS_CHECK_MSG(strstr(t, "pack") != NULL,
                 "no_pack overlay does not mention 'pack': [%s]", t);
}

LS_CASE(no_sd_wins_over_no_pack)
{
    /* If the card is not mounted at all, the pack necessarily isn't there
       either.  The user needs to hear about the card first: replugging
       fixes both, but writing tilepack.py output to a missing card does
       not fix anything.  So NO_SD must outrank NO_PACK. */
    map_tile_probe_t p = base();
    p.sd_mounted   = false;
    p.pack_present = false;
    p.zoom_present = false;
    p.files_present = 0;
    p.drawn = 0;
    LS_EQ_INT(map_tile_classify(&p), MAP_TILE_STATE_NO_SD);
}

LS_CASE(init_fail_wins_over_everything)
{
    /* If the renderer never came up (no JPEG engine, out of PSRAM), we
       cannot draw regardless of what is on the card.  INIT_FAIL is the
       actionable message; anything downstream would send the user
       hunting the wrong problem. */
    map_tile_probe_t p = base();
    p.renderer_ok = false;
    LS_EQ_INT(map_tile_classify(&p), MAP_TILE_STATE_INIT_FAIL);
}

LS_CASE(no_home_reports_no_home_not_no_sd)
{
    /* With no home, there is no origin to project the tile grid around;
       the SD state is irrelevant until the user sets a home.  Reporting
       NO_SD here would be true-in-fact but unhelpful in effect. */
    map_tile_probe_t p = base();
    p.home_set = false;
    p.sd_mounted = false;
    p.pack_present = false;
    p.zoom_present = false;
    p.files_present = 0;
    p.drawn = 0;
    LS_EQ_INT(map_tile_classify(&p), MAP_TILE_STATE_NO_HOME);
}

LS_CASE(pack_but_wrong_zoom_reports_no_zoom)
{
    /* Pack is there but was downloaded at a coarser zoom than the user
       just selected.  Different fix from NO_PACK: the user regenerates
       tilepack.py with a broader zoom range, they don't hunt for a
       missing directory. */
    map_tile_probe_t p = base();
    p.zoom_present = false;
    p.files_present = 0;
    p.drawn = 0;
    LS_EQ_INT(map_tile_classify(&p), MAP_TILE_STATE_NO_ZOOM);
}

LS_CASE(zoom_present_but_outside_covered_area)
{
    /* Zoom directory exists but no tiles at this x,y - the user has
       panned or set home outside the pack's covered region.  This is
       the state that most looks like a bug in the projection unless
       the overlay says otherwise. */
    map_tile_probe_t p = base();
    p.files_present = 0;
    p.drawn = 0;
    LS_EQ_INT(map_tile_classify(&p), MAP_TILE_STATE_NO_COVERAGE);
}

LS_CASE(files_present_but_none_decoded_is_decode_failure)
{
    /* Tiles are on the card at this position but the JPEG engine could
       not decode any of them - corrupt files, or a codec limitation.
       Different again from NO_COVERAGE: the fix is to re-generate the
       pack or check the engine, not to widen the covered area. */
    map_tile_probe_t p = base();
    p.files_present = 9;
    p.drawn         = 0;
    LS_EQ_INT(map_tile_classify(&p), MAP_TILE_STATE_DECODE_FAIL);
}

LS_CASE(null_probe_falls_back_to_init_fail)
{

    LS_EQ_INT(map_tile_classify(NULL), MAP_TILE_STATE_INIT_FAIL);
}

LS_CASE(every_state_has_a_distinct_short_text)
{

    const map_tile_state_t all[] = {
        MAP_TILE_STATE_OK,
        MAP_TILE_STATE_INIT_FAIL,
        MAP_TILE_STATE_NO_HOME,
        MAP_TILE_STATE_NO_SD,
        MAP_TILE_STATE_NO_PACK,
        MAP_TILE_STATE_NO_ZOOM,
        MAP_TILE_STATE_NO_COVERAGE,
        MAP_TILE_STATE_DECODE_FAIL,
    };
    int n = (int)(sizeof(all) / sizeof(all[0]));
    for (int i = 0; i < n; i++) {
        const char *ti = map_tile_state_text(all[i]);
        LS_CHECK(ti != NULL);
        LS_CHECK_MSG(strlen(ti) <= 40,
                     "state %d text too long: [%s]", (int)all[i], ti);
        for (int j = i + 1; j < n; j++) {
            const char *tj = map_tile_state_text(all[j]);
            LS_CHECK_MSG(strcmp(ti, tj) != 0,
                         "states %d and %d share text [%s]",
                         (int)all[i], (int)all[j], ti);
        }
    }
}
