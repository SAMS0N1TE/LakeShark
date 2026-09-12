/* LS_TEST_SOURCES: ls_map.c over the real reader and the real renderer */

#include "ls_test.h"

#include "ls_map.h"
#include "zeromesh_pmtiles.h"
#include "carto/style.h"
#include "carto/raster.h"

#include <stdio.h>
#include <string.h>

#ifndef PMTILES_FIXTURE
#define PMTILES_FIXTURE "fixtures/carto/franklin_z12.pmtiles"
#endif

/* Franklin, New Hampshire: inside the archive. */
#define LAT 43.4445
#define LON (-71.6473)

#define W 256
#define H 256

static const uint16_t *render_all(int *w, int *h)
{
    const uint16_t *px = ls_map_render(w, h);
    for (int i = 0; i < 256 && ls_map_render_busy(); i++)
        px = ls_map_render(w, h);
    return px;
}

static bool ready(void)
{
    if (!ls_map_begin(W, H)) return false;
    if (!ls_map_open(PMTILES_FIXTURE)) return false;
    ls_map_center(LAT, LON);
    return true;
}

/* Pixels that are not the style's background. carto_begin clears to it, so
   this is "how much of the frame the tiles actually covered". */
static int painted(const uint16_t *px)
{
    carto_style st;
    carto_style_default(&st);
    const uint16_t bg = carto_rgb565(st.bg);
    int n = 0;
    for (int i = 0; i < W * H; i++) if (px[i] != bg) n++;
    return n;
}

/* ---------------------------------------------------------------- cases -- */

LS_CASE(a_map_that_has_not_started_is_not_a_map_that_ran_out_of_memory)
{

    ls_map_end();

    const char *why = ls_map_status();
    LS_CHECK_MSG(why != NULL, "an unstarted map claims to be drawable");
    LS_CHECK_MSG(why && strstr(why, "PSRAM") == NULL,
                 "an unstarted map blames memory: '%s'", why);

    /* And a begin that works clears it, so the distinction is not a
       permanent label on a map that has simply not been asked yet. */
    LS_CHECK(ls_map_begin(W, H));
    const char *after = ls_map_status();
    LS_CHECK_MSG(after == NULL || strstr(after, "PSRAM") == NULL,
                 "a started map still blames memory: '%s'", after);
}

LS_CASE(an_archive_opens_and_the_status_line_goes_quiet)
{
    LS_CHECK_MSG(ls_map_begin(W, H), "no framebuffer: %s", ls_map_status());
    LS_CHECK_MSG(ls_map_status() != NULL,
                 "a map with no archive open claims to be drawable");

    LS_CHECK_MSG(ls_map_open(PMTILES_FIXTURE), "cannot open %s: %s",
                 PMTILES_FIXTURE, ls_map_status());
    LS_CHECK_MSG(ls_map_status() == NULL,
                 "an open archive still says '%s'", ls_map_status());
}

LS_CASE(a_missing_archive_says_which_thing_is_wrong)
{

    LS_CHECK(ls_map_begin(W, H));
    LS_CHECK_MSG(!ls_map_open("fixtures/carto/there-is-no-such-file.pmtiles"),
                 "a missing archive opened");

    const char *why = ls_map_status();
    LS_CHECK_MSG(why != NULL, "a map with no archive claims to be drawable");
    LS_CHECK_MSG(why && strstr(why, "PSRAM") == NULL,
                 "a missing card was reported as memory: '%s'", why);
    LS_CHECK_MSG(render_all(NULL, NULL) == NULL,
                 "a map with no archive handed back a frame");
}

LS_CASE(a_frame_over_the_coverage_is_drawn_from_every_tile_it_touches)
{
    if (!ready()) { LS_CHECK_MSG(false, "%s", ls_map_status()); return; }

    int w = 0, h = 0;
    const uint16_t *px = render_all(&w, &h);
    LS_CHECK_MSG(px != NULL, "render gave nothing: %s", ls_map_status());
    if (!px) return;

    LS_CHECK(w == W && h == H);

    ls_map_stats_t st;
    ls_map_stats(&st);
    LS_CHECK_MSG(st.tiles_wanted >= 1, "the viewport covered no tiles");
    LS_CHECK_MSG(st.tiles_drawn >= 1, "none of %d tiles was in the archive",
                 st.tiles_wanted);
    LS_CHECK_MSG(painted(px) > W * H / 100,
                 "a frame over the coverage drew %d of %d pixels",
                 painted(px), W * H);

    printf("    %dx%d at z%d: %d of %d tiles, %d px, arena %u, %u us\n",
           w, h, ls_map_zoom(), st.tiles_drawn, st.tiles_wanted,
           painted(px), (unsigned)st.arena_peak, (unsigned)st.render_us);
}

LS_CASE(the_arena_holds_the_worst_case_the_viewport_can_ask_for)
{

    if (!ready()) { LS_CHECK_MSG(false, "%s", ls_map_status()); return; }

    uint32_t worst = 0;
    int worst_tiles = 0;
    for (int dx = -160; dx <= 160; dx += 40)
        for (int dy = -160; dy <= 160; dy += 40) {
            ls_map_center(LAT, LON);
            ls_map_pan(dx, dy);
            if (!render_all(NULL, NULL)) continue;

            ls_map_stats_t st;
            ls_map_stats(&st);
            if (st.arena_peak > worst) worst = st.arena_peak;
            if (st.tiles_drawn > worst_tiles) worst_tiles = st.tiles_drawn;
        }

    LS_CHECK_MSG(worst > 0, "no pan drew anything");
    printf("    worst arena %u bytes over %d tiles in one frame\n",
           (unsigned)worst, worst_tiles);

    /* The margin is the point: ARENA_BYTES is 768 KB and the scratch alone
       is 512 KB, so this says how much of what is left the geometry used. */
    LS_CHECK_MSG(worst < 768u * 1024u,
                 "a frame wanted %u bytes of a 768 KB arena", (unsigned)worst);
}

LS_CASE(panning_off_the_coverage_says_so_and_comes_back)
{
    /* Out of coverage is a place, not a fault. The map has to say which, and
       it has to recover: a status that latched would leave the screen
       claiming there is no map after one pan too far. */
    if (!ready()) { LS_CHECK_MSG(false, "%s", ls_map_status()); return; }

    render_all(NULL, NULL);
    LS_CHECK_MSG(ls_map_status() == NULL,
                 "over the town the map says '%s'", ls_map_status());

    /* The other side of the country. */
    ls_map_center(37.7749, -122.4194);
    render_all(NULL, NULL);
    const char *why = ls_map_status();
    LS_CHECK_MSG(why != NULL, "off the coverage the map claims a frame");
    LS_CHECK_MSG(why && strstr(why, "PSRAM") == NULL,
                 "off the coverage was reported as memory: '%s'", why);

    ls_map_center(LAT, LON);
    render_all(NULL, NULL);
    LS_CHECK_MSG(ls_map_status() == NULL,
                 "coming back the map still says '%s'", ls_map_status());
}

LS_CASE(the_archive_is_asked_whether_a_point_is_there_before_going_to_it)
{

    if (!ready()) { LS_CHECK_MSG(false, "%s", ls_map_status()); return; }

    const int z = ls_map_zoom_covering(LAT, LON);
    LS_CHECK_MSG(z >= 0, "the town the fixture is cut from reads as uncovered");
    /* One zoom in the archive, so that is the only answer available. */
    LS_EQ_INT(ls_map_zoom(), z);

    /* The other side of the country, and no level holds it. -1 is a real
       answer: it is what stops the caller throwing away a working view to
       display a blank one. */
    LS_EQ_INT(-1, ls_map_zoom_covering(37.7749, -122.4194));

    /* Asking must not move the map. The whole point is to decide BEFORE
       committing, and a query with a side effect would have already done the
       damage it exists to prevent. */
    double lat0 = 0, lon0 = 0, lat1 = 0, lon1 = 0;
    ls_map_get_center(&lat0, &lon0);
    const int z0 = ls_map_zoom();
    (void)ls_map_zoom_covering(37.7749, -122.4194);
    (void)ls_map_zoom_covering(LAT, LON);
    ls_map_get_center(&lat1, &lon1);
    LS_CHECK_MSG(lat0 == lat1 && lon0 == lon1 && z0 == ls_map_zoom(),
                 "asking about coverage moved the map");
}

LS_CASE(nonsense_coordinates_are_uncovered_rather_than_a_tile_somewhere)
{
    /* Web Mercator stops around 85 degrees and a caller may hand over
       anything - a pole, a zeroed struct, a position parsed wrong. Clamping
       those into the projection would report a tile that has nothing to do
       with what was asked for, which is worse than saying no. */
    if (!ready()) { LS_CHECK_MSG(false, "%s", ls_map_status()); return; }

    LS_EQ_INT(-1, ls_map_zoom_covering(90.0, 0.0));
    LS_EQ_INT(-1, ls_map_zoom_covering(-90.0, 0.0));
    LS_EQ_INT(-1, ls_map_zoom_covering(0.0, 0.0));
}

LS_CASE(coverage_is_false_before_an_archive_is_open)
{
    /* No archive is not "everywhere is covered" and not a crash. Opening a
       file that is not there is how this file already gets to that state -
       ls_map_open closes the previous archive before it fails. */
    LS_CHECK(!ls_map_open("fixtures/carto/there-is-no-such-file.pmtiles"));
    LS_EQ_INT(-1, ls_map_zoom_covering(LAT, LON));
}

LS_CASE(the_zoom_stops_at_what_the_archive_holds)
{
    /* franklin_z12 is one zoom. Stepping past it in either direction has to
       stay where the tiles are: an empty frame and a broken map look the
       same, and only one of them is worth telling somebody about. */
    if (!ready()) { LS_CHECK_MSG(false, "%s", ls_map_status()); return; }

    PmTiles *pm = pmtiles_open(PMTILES_FIXTURE);
    LS_CHECK(pm != NULL);
    if (!pm) return;
    const int lo = pmtiles_min_zoom(pm), hi = pmtiles_max_zoom(pm);
    pmtiles_close(pm);

    for (int i = 0; i < 8; i++) ls_map_zoom_by(-1);
    LS_CHECK_MSG(ls_map_zoom() == lo, "zooming out landed on z%d, not z%d",
                 ls_map_zoom(), lo);

    for (int i = 0; i < 24; i++) ls_map_zoom_by(1);
    LS_CHECK_MSG(ls_map_zoom() == hi, "zooming in landed on z%d, not z%d",
                 ls_map_zoom(), hi);

    LS_CHECK_MSG(render_all(NULL, NULL) != NULL,
                 "at the archive's own zoom the map says '%s'",
                 ls_map_status());
}

LS_CASE(a_pan_moves_the_picture_and_a_pan_back_restores_it)
{
    /* Nothing else here would catch a pan that computed the right centre and
       drew the same frame anyway, which is what a viewport that recentres
       without re-placing the tiles does. */
    if (!ready()) { LS_CHECK_MSG(false, "%s", ls_map_status()); return; }

    static uint16_t before[W * H];
    const uint16_t *px = render_all(NULL, NULL);
    if (!px) { LS_CHECK_MSG(false, "no first frame"); return; }
    memcpy(before, px, sizeof(before));

    ls_map_pan(64, 0);
    px = render_all(NULL, NULL);
    LS_CHECK(px != NULL);
    if (px)
        LS_CHECK_MSG(memcmp(before, px, sizeof(before)) != 0,
                     "a 64 pixel pan drew an identical frame");

    ls_map_pan(-64, 0);
    px = render_all(NULL, NULL);
    LS_CHECK(px != NULL);
    if (px)
        LS_CHECK_MSG(memcmp(before, px, sizeof(before)) == 0,
                     "panning back did not restore the frame");
}

LS_CASE(a_map_that_never_got_its_memory_refuses_rather_than_faulting)
{
    /* The frame path has nowhere to put a failed allocation, so everything
       is taken at begin. A begin that failed has to leave every later call
       safe to make, because a screen will make them. */
    ls_map_end();
    LS_CHECK_MSG(ls_map_status() != NULL, "an ended map claims to be drawable");
    LS_CHECK_MSG(render_all(NULL, NULL) == NULL,
                 "an ended map handed back a frame");

    ls_map_pan(10, 10);
    ls_map_zoom_by(1);
    ls_map_center(LAT, LON);
    LS_CHECK_MSG(render_all(NULL, NULL) == NULL,
                 "an ended map handed back a frame after being driven");

    /* And it comes back. */
    LS_CHECK(ls_map_begin(W, H));
    LS_CHECK(ls_map_open(PMTILES_FIXTURE));
    ls_map_center(LAT, LON);
    LS_CHECK_MSG(render_all(NULL, NULL) != NULL,
                 "a restarted map says '%s'", ls_map_status());
}

LS_CASE(the_render_serial_moves_when_the_pixels_do_and_not_otherwise)
{
    if (!ready()) { LS_CHECK_MSG(false, "fixture: %s", ls_map_status()); return; }

    LS_CHECK(render_all(NULL, NULL) != NULL);
    const uint32_t first = ls_map_render_serial();

    /* Standing still. The cache hit path must not pretend it drew. */
    for (int i = 0; i < 5; i++) LS_CHECK(render_all(NULL, NULL) != NULL);
    LS_CHECK_MSG(ls_map_render_serial() == first,
                 "five renders of an unmoved view moved the serial %u times",
                 (unsigned)(ls_map_render_serial() - first));

    /* Moved. Every way of moving it counts, because each one is a separate
       line in the cache key and each has been forgotten at least once. */
    ls_map_pan(64, 0);
    LS_CHECK(render_all(NULL, NULL) != NULL);
    const uint32_t after_pan = ls_map_render_serial();
    LS_CHECK_MSG(after_pan != first, "a 64 pixel pan left the serial alone");

    /* A zoom the archive cannot honour is clamped, which means the view did
       NOT move - and a serial that moved anyway would rebuild the screen's
       cells for a picture that is byte for byte the same. This fixture is
       one town at one zoom, so it is the clamp that gets exercised here and
       the clamp is the case worth pinning: it is the one where "I asked for
       a change" and "something changed" come apart. */
    const int z_before = ls_map_zoom();
    ls_map_zoom_by(-1);
    LS_CHECK(render_all(NULL, NULL) != NULL);
    const uint32_t after_zoom = ls_map_render_serial();
    if (ls_map_zoom() != z_before)
        LS_CHECK_MSG(after_zoom != after_pan, "a zoom step left the serial alone");
    else
        LS_CHECK_MSG(after_zoom == after_pan,
                     "a zoom the archive clamped away still moved the serial");

    ls_map_center(LAT + 0.02, LON);
    LS_CHECK(render_all(NULL, NULL) != NULL);
    LS_CHECK_MSG(ls_map_render_serial() != after_zoom,
                 "re-centring left the serial alone");

    /* And back to still, at the new view: the point is that it settles,
       not merely that it moves. */
    const uint32_t settled = ls_map_render_serial();
    for (int i = 0; i < 3; i++) LS_CHECK(render_all(NULL, NULL) != NULL);
    LS_CHECK_MSG(ls_map_render_serial() == settled,
                 "the serial kept moving after the view stopped");

    ls_map_zoom_by(1);
    ls_map_center(LAT, LON);
    LS_CHECK(render_all(NULL, NULL) != NULL);
}

LS_CASE(no_two_place_labels_carry_the_same_text)
{
    if (!ready()) { LS_CHECK_MSG(false, "fixture: %s", ls_map_status()); return; }
    LS_CHECK(render_all(NULL, NULL) != NULL);

    const carto_label *L = NULL;
    const int n = ls_map_labels(&L);
    LS_CHECK_MSG(n > 0, "the fixture produced no place names at all");
    if (n <= 0 || !L) return;

    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            LS_CHECK_MSG(strcmp(L[i].text, L[j].text) != 0,
                         "labels %d and %d are both \"%s\"", i, j, L[i].text);

    bool have_trimmed = false;
    for (int i = 0; i < n; i++)
        if (strcmp(L[i].text, "Franklin Falls Historic") == 0)
            have_trimmed = true;

    LS_CHECK_MSG(have_trimmed,
                 "this fixture no longer contains the trimmed name this case "
                 "exists for, so the de-duplication above proved nothing");
}

LS_CASE(a_render_taken_in_slices_matches_one_taken_whole)
{
    if (!ready()) { LS_CHECK_MSG(false, "fixture: %s", ls_map_status()); return; }

    static uint16_t whole[W * H];

    /* One slice, the whole thing. */
    ls_map_set_step_limit(0, 0);
    const uint16_t *px = render_all(NULL, NULL);
    LS_CHECK(px != NULL);
    if (!px) return;
    LS_CHECK_MSG(!ls_map_render_busy(),
                 "the default budget left a render unfinished on this host");
    memcpy(whole, px, sizeof(whole));

    ls_map_stats_t one;
    ls_map_stats(&one);

    ls_map_pan(4096, 0);
    render_all(NULL, NULL);
    ls_map_center(LAT, LON);

    ls_map_set_step_limit(0, 1);
    int calls = 0;
    px = ls_map_render(NULL, NULL);
    while (ls_map_render_busy() && calls < 512) { px = ls_map_render(NULL, NULL); calls++; }

    LS_CHECK_MSG(calls > 0,
                 "a one tile slice still finished in a single call, so the "
                 "resume path was never taken");
    LS_CHECK_MSG(!ls_map_render_busy(), "the sliced render never finished");
    LS_CHECK(px != NULL);

    if (px)
        LS_CHECK_MSG(memcmp(whole, px, sizeof(whole)) == 0,
                     "the sliced render drew a different picture over %d calls",
                     calls + 1);

    ls_map_stats_t many;
    ls_map_stats(&many);
    LS_CHECK_MSG(many.tiles_drawn == one.tiles_drawn,
                 "whole drew %d tiles, sliced drew %d",
                 one.tiles_drawn, many.tiles_drawn);

    ls_map_set_step_limit(0, 0);
}

LS_CASE(panning_during_a_render_abandons_it_for_the_new_view)
{
    if (!ready()) { LS_CHECK_MSG(false, "fixture: %s", ls_map_status()); return; }

    ls_map_set_step_limit(0, 0);
    render_all(NULL, NULL);

    /* Start a render somewhere, one tile at a time, then move. */
    ls_map_pan(4096, 0);
    ls_map_set_step_limit(0, 1);
    ls_map_render(NULL, NULL);
    LS_CHECK_MSG(ls_map_render_busy(),
                 "a one tile slice finished a whole render at once");

    ls_map_center(LAT, LON);
    ls_map_set_step_limit(0, 0);
    const uint16_t *px = render_all(NULL, NULL);

    LS_CHECK(px != NULL);
    LS_CHECK_MSG(!ls_map_render_busy(), "the restarted render never finished");

    /* And it is the NEW view that got drawn, not the abandoned one. */
    double lat = 0, lon = 0;
    ls_map_get_center(&lat, &lon);
    LS_CHECK_MSG(lat == LAT && lon == LON,
                 "the centre came back as %.5f %.5f", lat, lon);

    ls_map_set_step_limit(0, 0);
}

LS_CASE(classifying_a_row_agrees_with_classifying_each_pixel)
{
    static uint16_t px[256];
    static uint8_t  row[256];

    int disagreements = 0;
    uint16_t first_bad = 0;
    int first_row = -1, first_one = -1;

    for (int base = 0; base < 65536; base += 256) {
        for (int i = 0; i < 256; i++) px[i] = (uint16_t)(base + i);
        ls_map_classify_row(px, row, 256);

        for (int i = 0; i < 256; i++) {
            const ls_map_ink_t one = ls_map_classify(px[i]);
            if ((ls_map_ink_t)row[i] == one) continue;
            if (!disagreements) {

                first_bad = px[i];
                first_row = (int)row[i];
                first_one = (int)one;
            }
            disagreements++;
        }
    }

    LS_CHECK_MSG(disagreements == 0,
                 "%d of 65536 pixels classify differently in bulk, first 0x%04X "
                 "(row says %d, single says %d)",
                 disagreements, (unsigned)first_bad, first_row, first_one);

    /* And the five the style actually paints must land on themselves rather
       than on a neighbour - the fast path is exact equality, so a wrong
       entry in the ink table would show up here and nowhere else. */
    static const ls_map_ink_t WANT[] = {
        LS_MAP_GROUND, LS_MAP_WATER, LS_MAP_PARK, LS_MAP_BUILDING, LS_MAP_ROAD
    };
    carto_style st;
    carto_style_default(&st);
    const uint16_t ink[5] = {
        carto_rgb565(st.bg), carto_rgb565(st.water), carto_rgb565(st.park),
        carto_rgb565(st.building), carto_rgb565(st.road_color)
    };
    for (int i = 0; i < 5; i++)
        LS_CHECK_MSG(ls_map_classify(ink[i]) == WANT[i],
                     "the style's own colour %d (0x%04X) classifies as %d",
                     i, (unsigned)ink[i], (int)ls_map_classify(ink[i]));
}

LS_CASE(a_teardown_does_not_leave_a_render_in_flight)
{
    if (!ready()) { LS_CHECK_MSG(false, "fixture: %s", ls_map_status()); return; }

    /* One tile a slice, so a render is reliably left part way through. */
    ls_map_set_step_limit(0, 1);

    /* --- a resize --------------------------------------------------- */
    ls_map_pan(4096, 0);
    ls_map_render(NULL, NULL);
    LS_CHECK_MSG(ls_map_render_busy(), "a one tile slice finished it all");

    LS_CHECK(ls_map_begin(W / 2, H / 2));
    LS_CHECK_MSG(!ls_map_render_busy(),
                 "a resize left the previous render in flight");

    /* And the map still works afterwards. */
    ls_map_set_step_limit(0, 0);
    LS_CHECK(ls_map_begin(W, H));
    ls_map_center(LAT, LON);
    LS_CHECK_MSG(render_all(NULL, NULL) != NULL,
                 "the map did not come back after a resize: %s",
                 ls_map_status());

    /* --- ls_map_end -------------------------------------------------- */
    ls_map_set_step_limit(0, 1);
    ls_map_pan(4096, 0);
    ls_map_render(NULL, NULL);
    LS_CHECK(ls_map_render_busy());

    ls_map_end();
    LS_CHECK_MSG(!ls_map_render_busy(),
                 "ending the map left a render in flight, and nothing can "
                 "clear it because render returns early with no pixels");

    /* --- re-open ------------------------------------------------------ */
    ls_map_set_step_limit(0, 0);
    LS_CHECK(ls_map_begin(W, H));
    LS_CHECK(ls_map_open(PMTILES_FIXTURE));
    ls_map_center(LAT, LON);

    ls_map_set_step_limit(0, 1);
    ls_map_pan(4096, 0);
    ls_map_render(NULL, NULL);
    LS_CHECK(ls_map_render_busy());

    LS_CHECK(ls_map_open(PMTILES_FIXTURE));
    LS_CHECK_MSG(!ls_map_render_busy(),
                 "opening an archive left the previous render in flight");

    ls_map_set_step_limit(0, 0);
    ls_map_center(LAT, LON);
    LS_CHECK(render_all(NULL, NULL) != NULL);
}

/* ------------------------------------------------- tile store -- */

/* Straight to the end of a render, for the cache cases: they count card
   reads, and a render left half done has not read everything it wants. */
static void settle(void)
{
    int w = 0, h = 0;
    render_all(&w, &h);
}

#define BIG_BUDGET (6u * 1024u * 1024u)

LS_CASE(the_budget_buys_bytes_rather_than_slots_of_the_worst_tile_s_size)
{

    LS_CHECK(ready());
    ls_map_set_cache_budget(BIG_BUDGET);
    LS_CHECK(ls_map_open(PMTILES_FIXTURE));
    ls_map_center(LAT, LON);
    settle();

    int tiles = 0;
    uint32_t held = 0, cap = 0;
    ls_map_tile_cache_stats(&tiles, NULL, NULL, NULL);
    ls_map_tile_cache_bytes(&held, &cap);
    LS_CHECK_MSG(tiles >= 2 && held > 0,
                 "the fixture put %d tiles and %lu bytes in the store - "
                 "nothing to measure a policy against",
                 tiles, (unsigned long)held);

    PmTiles *pm = pmtiles_open(PMTILES_FIXTURE);
    LS_CHECK_MSG(pm != NULL, "cannot open the fixture to size a tile");
    const uint32_t worst = pmtiles_max_tile_len(pm);
    pmtiles_close(pm);

    ls_note("%d tiles, %lu bytes held, largest tile %lu",
            tiles, (unsigned long)held, (unsigned long)worst);

    /* Exactly the room those tiles need, and not a byte more. */
    ls_map_set_cache_budget(held);
    LS_CHECK(ls_map_open(PMTILES_FIXTURE));
    ls_map_center(LAT, LON);
    settle();

    int kept = 0;
    uint32_t kept_bytes = 0;
    ls_map_tile_cache_stats(&kept, NULL, NULL, NULL);
    ls_map_tile_cache_bytes(&kept_bytes, &cap);

    LS_EQ_INT(tiles, kept);
    LS_EQ_UINT(held, cap);
    LS_CHECK_MSG(kept_bytes <= cap,
                 "the store holds %lu bytes against a budget of %lu",
                 (unsigned long)kept_bytes, (unsigned long)cap);

    /* And the old rule could not have: budget/worst entries. The fixture is
       only able to show this when its tiles differ in size, so say so rather
       than assert something vacuous. */
    const int old_would_hold = (int)(held / worst);
    LS_CHECK_MSG(old_would_hold < kept,
                 "this fixture's tiles are all the same size (%lu bytes over "
                 "%d tiles, largest %lu), so the two policies agree on it and "
                 "the case proves nothing",
                 (unsigned long)held, kept, (unsigned long)worst);

    ls_map_set_cache_budget(BIG_BUDGET);
}

LS_CASE(panning_between_two_views_stops_reading_the_card)
{

    LS_CHECK(ready());
    ls_map_set_cache_budget(BIG_BUDGET);
    LS_CHECK(ls_map_open(PMTILES_FIXTURE));
    ls_map_center(LAT, LON);

    /* Both views once, so everything either of them wants is in the store. */
    settle();
    ls_map_pan(64, 0);
    settle();
    ls_map_pan(-64, 0);
    settle();

    int tiles = 0;
    uint32_t hits0 = 0, misses0 = 0, absent0 = 0;
    ls_map_tile_cache_stats(&tiles, &hits0, &misses0, &absent0);

    /* Four more pans over exactly the same two views. Not one may read a
       tile the store already had. */
    for (int i = 0; i < 2; i++) {
        ls_map_pan(64, 0);
        settle();
        ls_map_pan(-64, 0);
        settle();
    }

    uint32_t hits1 = 0, misses1 = 0, absent1 = 0;
    ls_map_tile_cache_stats(&tiles, &hits1, &misses1, &absent1);

    LS_CHECK_MSG(hits1 > hits0,
                 "four more pans asked the store for nothing at all");
    LS_CHECK_MSG(misses1 == misses0,
                 "four pans over two views already seen read the card %lu "
                 "more times - the store cannot hold a pan's working set",
                 (unsigned long)(misses1 - misses0));
    LS_CHECK_MSG(absent1 > absent0,
                 "this frame overhangs the archive, so the absent probes "
                 "should still be counting - if they are not, the two are "
                 "being conflated again");

    ls_map_set_cache_budget(BIG_BUDGET);
}

LS_CASE(a_tile_bigger_than_the_whole_budget_is_not_kept)
{

    LS_CHECK(ready());
    ls_map_set_cache_budget(64);          /* smaller than any real tile */
    LS_CHECK(ls_map_open(PMTILES_FIXTURE));
    ls_map_center(LAT, LON);

    int w = 0, h = 0;
    const uint16_t *px = render_all(&w, &h);
    LS_CHECK_MSG(px != NULL, "a store too small to use stopped the render");
    LS_CHECK_MSG(painted(px) > 0,
                 "nothing was drawn when the tile store could hold nothing");

    int tiles = 0;
    uint32_t held = 0, cap = 0;
    ls_map_tile_cache_stats(&tiles, NULL, NULL, NULL);
    ls_map_tile_cache_bytes(&held, &cap);
    LS_EQ_INT(0, tiles);
    LS_EQ_UINT(0, held);

    ls_map_set_cache_budget(BIG_BUDGET);
}
