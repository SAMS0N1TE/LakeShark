/* LS_TEST_SOURCES: scr_p25.c + ls_waterfall.c + p25_tg_observed.c + tui_core.c */

#include "ls_test.h"
#include "tui_core.h"
#include "ls_tui_screen.h"
#include "ls_theme.h"
#include "p25_state.h"
#include "p25_tg_observed.h"
#include "esp_timer.h"
#include "ls_waterfall.h"
#include "ls_tui.h"

#include <string.h>

/* ---------------------------------------------------------------- fakes -- */

p25_state_t P25;
scan_state_t SCAN;
uint32_t s_tune_freq_hz;

void p25_get_receiver_status(ls_iq_control_status_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
}

static int  s_shape;
static bool s_have_spectrum = true;

#define BINS 120

static void feed_row(void)
{
    if (!s_have_spectrum) {
        /* Releasing the instrument is what the source does when no receiver
           is running, and it clears the history. The screen then has nothing
           to draw and must say so. */
        ls_wf_claim(LS_WF_OWNER_NONE, NULL);
        return;
    }

    ls_wf_claim(LS_WF_OWNER_P25, "P25");

    float bins[BINS];
    for (int i = 0; i < BINS; i++) {
        switch (s_shape) {
        case 0: bins[i] = 0.0f; break;
        case 1: bins[i] = 1.0f; break;
        case 2: bins[i] = (i == 0) ? 1.0f : 0.0f; break;
        case 3: bins[i] = (i == BINS - 1) ? 1.0f : 0.0f; break;
        /* Deliberately out of contract: the header promises 0..1, and an
           instrument that trusts that without clamping indexes a colour
           table or a row offset with a negative or oversized value. */
        case 4: bins[i] = (i & 1) ? 4.0f : -4.0f; break;
        default: bins[i] = (float)(i % 8) / 8.0f; break;
        }
    }

    const ls_wf_feed_t f = {
        .center_hz = 851000000u,
        .span_hz = 240000u,
        .floor_db = -110.0f,
        .top_db = -20.0f,
        .live = true,
        .note = NULL,
    };
    ls_wf_push(LS_WF_OWNER_P25, bins, BINS, &f);
}

/* ------------------------------------------------------------- fixtures -- */

#define W 130
#define H 70
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

static int painted(tui_rect r)
{
    int n = 0;
    for (int y = r.y; y < r.y + r.h && y < H; y++)
        for (int x = r.x; x < r.x + r.w && x < W; x++)
            if (g_back[y * W + x].ch != SENTINEL) n++;
    return n;
}

extern const ls_tui_screen_t ls_scr_p25;

/* Panes down to nothing, because the waterfall subtracts a trace height and a
   border from the pane and can reach a negative row count. */
/* ls_theme.c is linked for real: it is plain C with no ESP dependency. The
   active-theme pointer lives in ls_tui.c, which owns the framebuffer and
   cannot come to the host, so only that pointer is faked. The router reaches
   for it when it draws the help overlay. */
static const ls_tui_theme_t *s_active;

const ls_tui_theme_t *ls_tui_get_theme(void)
{
    return s_active ? s_active : ls_tui_theme_at(0);
}
void ls_tui_set_theme(const ls_tui_theme_t *t) { s_active = t; }
/* The waterfall linked in here asks whether the ground is white. */
bool ls_tui_daylight(void) { return false; }

/* ------------------------------------------------------------ the grid -- */

static int s_grid_cols = 115, s_grid_rows = 27;

void ls_tui_geometry(int *cols, int *rows, int *cw, int *ch)
{
    if (cols) *cols = s_grid_cols;
    if (rows) *rows = s_grid_rows;
    if (cw)   *cw = 10;
    if (ch)   *ch = 17;
}

void ls_tui_invalidate(void) { }
/* The corner padding the real ls_tui.c works out from the panel's
   pixels. These panes have square corners, so there is nothing to stand off. */
int ls_tui_corner_pad(int row) { (void)row; return 0; }

static void grid_for(tui_rect pane)
{
    const bool wide = pane.w > pane.h;
    s_grid_cols = wide ? 115 : 48;
    s_grid_rows = wide ? 27 : 66;
}

static const tui_rect PANES[] = {
    {  1,  2, 113, 24 },   /* landscape body   */
    {  1,  2,  46, 63 },   /* portrait body    */
    {  1,  2,  46, 30 },
    {  1,  2,  56, 24 },
    {  1,  2,  20, 10 },
    {  1,  2,  20,  8 },   /* exactly the screen's minimum */
    {  1,  2,  20,  7 },   /* one below it                 */
    {  1,  2,  12,  5 },
    {  1,  2,   6,  3 },
    {  1,  2,   4,  2 },
};
#define N_PANES ((int)(sizeof(PANES) / sizeof(PANES[0])))
#define N_SHAPES 6

/* The screen pages with LEFT and RIGHT, not TAB, and both clamp at the ends.
   Pressing the same direction twice therefore lands on a known page whatever
   the previous case left behind, which is what keeps these cases independent.

   Worth stating because it was got wrong here first: TAB is not handled by
   this screen at all, so a helper that pressed TAB left every test running
   against the decode page while claiming to test the spectrum. */
static void show_signal_page(void)
{
    ls_scr_p25.key(LS_TK_CHAR, '2');
}

static void show_decode_page(void)
{
    ls_scr_p25.key(LS_TK_CHAR, '1');
}

static void draw_pane(const ls_tui_screen_t *scr, tui_rect pane)
{
    feed_row();
    grid_for(pane);
    scr->draw(&g_sf, pane);
}

/* ---------------------------------------------------------------- cases -- */

LS_CASE(the_p25_screen_stays_in_its_rect_on_every_pane_and_every_page)
{
    for (int page = 0; page < 2; page++) {
        if (page == 0) show_decode_page(); else show_signal_page();
        for (int shape = 0; shape < N_SHAPES; shape++) {
            s_shape = shape;
            for (int p = 0; p < N_PANES; p++) {
                fresh();
                draw_pane(&ls_scr_p25, PANES[p]);
                LS_CHECK_MSG(escaped(PANES[p]) == 0,
                             "escaped on page %d shape %d pane %dx%d (%d cells)",
                             page, shape, PANES[p].w, PANES[p].h,
                             escaped(PANES[p]));
            }
        }
    }
}

/* How many rows of the pane the history is painting.

   With `fine` on, which is the default and the reason a waterfall is worth
   having on a character grid at all, a history cell is the upper-half block
   carrying two rows of history in one cell. The spectrum trace above it uses
   the eighth-height run instead, so the glyph tells the two apart without
   knowing where the split fell. */

static void pin_grain_to_shade(void)
{
    ls_wf_cfg_t cfg = *ls_wf_cfg();
    cfg.grain = LS_WF_GRAIN_SHADE;
    ls_wf_cfg_set(&cfg);
}

static bool is_history_glyph(char c)
{
    return c == LS_TUI_SHADE_25 || c == LS_TUI_SHADE_50 ||
           c == LS_TUI_SHADE_75 || c == LS_TUI_SHADE_FULL;
}

static int history_rows(tui_rect pane)
{
    int rows = 0;
    for (int y = pane.y; y < pane.y + pane.h && y < H; y++) {
        int n = 0;
        for (int x = pane.x; x < pane.x + pane.w && x < W; x++)
            if (is_history_glyph(g_back[y * W + x].ch)) n++;
        if (n > pane.w / 2) rows++;
    }
    return rows;
}

/* The history is a ring that advances a row per push, so one draw fills one
   row and the rest hold whatever an earlier case left. Prime it past the
   pane's own height before counting. */
static void prime(tui_rect pane, int frames)
{
    for (int i = 0; i < frames; i++) {
        fresh();
        draw_pane(&ls_scr_p25, pane);
    }
}

LS_CASE(the_waterfall_fills_the_pane_it_is_given)
{

    show_signal_page();
    pin_grain_to_shade();
    s_shape = 1;                       /* every bin at full scale */

    tui_rect shortp = { 1, 2, 60, 12 };
    prime(shortp, 40);
    const int few = history_rows(shortp);

    tui_rect tallp = { 1, 2, 60, 40 };
    prime(tallp, 60);
    const int many = history_rows(tallp);

    LS_CHECK_MSG(few > 0, "a 12-row pane gave the waterfall no history rows");
    LS_CHECK_MSG(many > few,
                 "a 40-row pane gave %d history rows and a 12-row pane %d",
                 many, few);
}

LS_CASE(the_spectrum_takes_its_share_and_the_history_takes_the_rest)
{

    show_signal_page();
    pin_grain_to_shade();
    s_shape = 1;

    tui_rect tall = { 1, 2, 60, 60 };
    ls_wf_cfg_t cfg = *ls_wf_cfg();
    const uint8_t was = cfg.split_pct;

    cfg.split_pct = 25;
    ls_wf_cfg_set(&cfg);
    prime(tall, 80);
    const int history_when_small = history_rows(tall);

    cfg.split_pct = 75;
    ls_wf_cfg_set(&cfg);
    prime(tall, 80);
    const int history_when_large = history_rows(tall);

    cfg.split_pct = was;
    ls_wf_cfg_set(&cfg);

    LS_CHECK_MSG(history_when_small > history_when_large,
                 "a quarter spectrum gave the history %d rows and three "
                 "quarters gave it %d", history_when_small,
                 history_when_large);
    LS_CHECK_MSG(history_when_small > tall.h / 3,
                 "a quarter spectrum left the history only %d of %d rows",
                 history_when_small, tall.h);
}

LS_CASE(out_of_contract_bin_values_do_not_escape_the_pane)
{
    /* Shape 4 hands the screen -4.0 and 4.0 where the header promises 0..1.
       The screen clamps; this is what proves it still does. */
    show_signal_page();
    s_shape = 4;
    for (int p = 0; p < N_PANES; p++) {
        fresh();
        draw_pane(&ls_scr_p25, PANES[p]);
        LS_CHECK_MSG(escaped(PANES[p]) == 0,
                     "out-of-range bins escaped a %dx%d pane",
                     PANES[p].w, PANES[p].h);
    }
}

static int untouched_rows(tui_rect r)
{
    int n = 0;
    for (int y = r.y; y < r.y + r.h && y < H; y++) {
        bool clear = true;
        for (int x = r.x; x < r.x + r.w && x < W; x++)
            if (g_back[y * W + x].ch != SENTINEL) { clear = false; break; }
        if (clear) n++;
    }
    return n;
}

LS_CASE(an_unavailable_spectrum_says_so_in_a_box_its_own_size)
{

    show_signal_page();
    s_have_spectrum = false;
    fresh();
    draw_pane(&ls_scr_p25, PANES[0]);
    LS_CHECK(escaped(PANES[0]) == 0);

    const tui_rect p = PANES[0];
    LS_CHECK_MSG(painted(p) > 60,
                 "only %d cells drawn when the spectrum is unavailable - the "
                 "screen has gone blank instead of saying why", painted(p));
    LS_CHECK_MSG(untouched_rows(p) >= p.h / 3,
                 "the notice left %d of %d rows clear - a box stretched over "
                 "the whole pane lands on whatever is under it",
                 untouched_rows(p), p.h);

    s_have_spectrum = true;
}

LS_CASE(the_waterfall_ring_survives_more_frames_than_it_is_deep)
{
    /* The ring is 32 deep and advances once a frame. Drawing well past that
       walks the modulo the whole way round, which is where an index written
       against the old depth of eight reads a row it never wrote. */
    show_signal_page();
    s_shape = 5;
    for (int i = 0; i < 200; i++) {
        fresh();
        draw_pane(&ls_scr_p25, PANES[1]);
        LS_CHECK_MSG(escaped(PANES[1]) == 0, "escaped on frame %d", i);
    }
}

/* ----------------------------------------------------- activity -- */

static bool rect_has(tui_rect r, const char *needle)
{
    for (int y = r.y; y < r.y + r.h && y < H; y++) {
        char row[W + 1];
        int n = 0;
        for (int x = r.x; x < r.x + r.w && x < W; x++) {
            const char c = g_back[y * W + x].ch;
            row[n++] = (c == SENTINEL) ? ' ' : c;
        }
        row[n] = 0;
        if (strstr(row, needle)) return true;
    }
    return false;
}

/* Which row a string landed on, or -1. Order is the one thing about this
   table that IS positional. */
static int rect_row_of(tui_rect r, const char *needle)
{
    for (int y = r.y; y < r.y + r.h && y < H; y++) {
        char row[W + 1];
        int n = 0;
        for (int x = r.x; x < r.x + r.w && x < W; x++) {
            const char c = g_back[y * W + x].ch;
            row[n++] = (c == SENTINEL) ? ' ' : c;
        }
        row[n] = 0;
        if (strstr(row, needle)) return y;
    }
    return -1;
}

/* Just the table, found by its own title.

   Asking about the whole pane cannot answer "is there a frequency in the
   table", because DECODE's own FREQ field is a frequency and sits above it.
   That mistake was made here first and the case passed for the wrong
   reason. */
static tui_rect table_of(tui_rect pane)
{
    const int y = rect_row_of(pane, "ACTIVITY");
    if (y < 0) return tui_rect_make(0, 0, 0, 0);
    return tui_rect_make(pane.x, y, pane.w, pane.y + pane.h - y);
}

#define TUNED_HZ 154785000u

static void no_traffic(void)
{
    p25_tg_observed_clear();
    ls_shim_time_set(0);
    s_tune_freq_hz = TUNED_HZ;
    P25.dsd_has_sync = false;
}

static void heard(uint16_t tg, uint32_t at_ms, uint8_t source)
{
    LS_CHECK(p25_tg_observed_record(TUNED_HZ, 0x4B3, tg, source, at_ms));
}

static const tui_rect PORTRAIT = { 1, 2, 46, 63 };

static void draw_decode_now(tui_rect pane)
{
    show_decode_page();
    fresh();
    draw_pane(&ls_scr_p25, pane);
}

LS_CASE(the_decode_page_lists_talkgroups_the_decoder_already_recorded)
{

    no_traffic();
    heard(1101, 1000, P25_TG_SEEN_LCW);
    heard(2202, 1000, P25_TG_SEEN_GRANT);
    ls_shim_time_set(5 * 1000000LL);

    draw_decode_now(PORTRAIT);

    LS_CHECK_MSG(rect_has(PORTRAIT, "ACTIVITY"),
                 "the decode page drew no activity panel");
    LS_CHECK_MSG(rect_has(PORTRAIT, "1101"),
                 "a talkgroup the decoder recorded is not on the screen");
    LS_CHECK_MSG(rect_has(PORTRAIT, "2202"),
                 "the second recorded talkgroup is not on the screen");
    LS_CHECK(escaped(PORTRAIT) == 0);
}

LS_CASE(the_most_recently_heard_talkgroup_is_at_the_top)
{
    /* A scanner's list is read from the top and abandoned part way down, so
       an unsorted table buries the call that just ended under thirty stale
       rows. The store itself stays in insertion order - it is a record, not
       a view - so the ordering has to happen here. */
    no_traffic();
    heard(1111, 1000,  P25_TG_SEEN_LCW);
    heard(2222, 60000, P25_TG_SEEN_LCW);
    heard(3333, 30000, P25_TG_SEEN_LCW);
    ls_shim_time_set(90 * 1000000LL);

    draw_decode_now(PORTRAIT);

    const tui_rect t = table_of(PORTRAIT);
    const int newest = rect_row_of(t, "2222");
    const int mid    = rect_row_of(t, "3333");
    const int oldest = rect_row_of(t, "1111");
    LS_CHECK_MSG(newest > 0 && mid > 0 && oldest > 0,
                 "rows missing: 2222 at %d, 3333 at %d, 1111 at %d",
                 newest, mid, oldest);
    LS_CHECK_MSG(newest < mid && mid < oldest,
                 "listed oldest first: 2222 at row %d, 3333 at %d, 1111 at %d",
                 newest, mid, oldest);
}

LS_CASE(a_talkgroup_from_a_channel_no_longer_tuned_carries_its_frequency)
{

    no_traffic();
    heard(4444, 1000, P25_TG_SEEN_LCW);                        /* on the dial */
    LS_CHECK(p25_tg_observed_record(TUNED_HZ + 3000u, 0x4B3, 4444,
                                    P25_TG_SEEN_LCW, 1000));   /* same, offset */
    LS_CHECK(p25_tg_observed_record(851012500ULL, 0x4B3, 5555,
                                    P25_TG_SEEN_GRANT, 1000)); /* elsewhere    */
    ls_shim_time_set(2 * 1000000LL);

    draw_decode_now(PORTRAIT);

    const tui_rect t = table_of(PORTRAIT);
    LS_CHECK_MSG(t.h > 0, "no activity table to look in");
    LS_CHECK_MSG(rect_has(t, "851.0125"),
                 "a talkgroup heard on another channel does not say which");
    LS_CHECK_MSG(!rect_has(t, "154.7"),
                 "a talkgroup on the tuned channel was labelled with a "
                 "frequency, so every row now reads as off-channel");
}

LS_CASE(a_silent_receiver_and_a_quiet_system_do_not_say_the_same_thing)
{
    /* The failure this panel exists to make visible. A decoder that has lost
       sync has heard nothing BECAUSE it is not decoding; one that is synced
       with an empty table is on a genuinely quiet system. One message for
       both reports a dead receiver as a quiet channel, which is exactly the
       pair somebody opens this screen to tell apart. */
    no_traffic();

    P25.dsd_has_sync = false;
    draw_decode_now(PORTRAIT);
    LS_CHECK_MSG(rect_has(table_of(PORTRAIT), "no sync"),
                 "an unsynced decoder does not say so in the activity panel");

    P25.dsd_has_sync = true;
    draw_decode_now(PORTRAIT);
    LS_CHECK_MSG(rect_has(table_of(PORTRAIT), "synced"),
                 "a synced decoder with nothing heard reports it as no sync");
    P25.dsd_has_sync = false;
}

LS_CASE(the_activity_panel_stays_inside_every_pane_it_is_offered)
{
    /* A full store against every pane the screen is drawn into, including
       the ones too small for the panel to appear at all - which is where a
       rect computed from a height that went negative escapes. */
    no_traffic();
    for (unsigned i = 1; i <= P25_TG_OBSERVED_MAX; ++i)
        LS_CHECK(p25_tg_observed_record(TUNED_HZ + i * 12500u, 0x4B3,
                                        (uint16_t)(1000 + i),
                                        P25_TG_SEEN_LCW, i * 1000u));
    ls_shim_time_set(600 * 1000000LL);

    show_decode_page();
    for (int p = 0; p < N_PANES; p++) {
        fresh();
        draw_pane(&ls_scr_p25, PANES[p]);
        LS_CHECK_MSG(escaped(PANES[p]) == 0,
                     "the activity panel escaped a %dx%d pane (%d cells)",
                     PANES[p].w, PANES[p].h, escaped(PANES[p]));
    }
}

LS_CASE(the_panels_above_the_table_keep_their_own_rows)
{
    /* measured the panels and capped them at nine rows each; this table
       went into the space that freed. A layout that took its rows from the
       panels instead would put the fields back inside a frame too short to
       hold them, which is the failure that cap was for. */
    no_traffic();
    heard(1234, 1000, P25_TG_SEEN_LCW);
    ls_shim_time_set(2 * 1000000LL);

    draw_decode_now(PORTRAIT);

    LS_CHECK(rect_has(PORTRAIT, "DECODE"));
    LS_CHECK(rect_has(PORTRAIT, "SIGNAL"));
    LS_CHECK(rect_has(PORTRAIT, "ACTIVITY"));

    /* SYNC is DECODE's last field and RX is SIGNAL's: if either panel lost
       rows to the table, its bottom field is the one that goes. */
    LS_CHECK_MSG(rect_has(PORTRAIT, "SYNC"),
                 "DECODE lost its last field to the activity table");
    LS_CHECK_MSG(rect_has(PORTRAIT, "RX"),
                 "SIGNAL lost its last field to the activity table");

    const int activity = rect_row_of(PORTRAIT, "ACTIVITY");
    const int rx       = rect_row_of(PORTRAIT, "RX");
    LS_CHECK_MSG(activity > rx,
                 "the table is at row %d and SIGNAL's last field at %d - they "
                 "are drawn over each other", activity, rx);
}

LS_CASE(landscape_gets_the_table_too)
{
    /* Landscape had the same hole for the same reason: the two panels sit
       side by side there, each stretched over a twenty-four row body to hold
       seven rows of fields. */
    no_traffic();
    heard(7777, 1000, P25_TG_SEEN_HDU);
    ls_shim_time_set(2 * 1000000LL);

    draw_decode_now(PANES[0]);

    LS_CHECK_MSG(rect_has(PANES[0], "ACTIVITY"),
                 "landscape draws no activity panel");
    LS_CHECK_MSG(rect_has(table_of(PANES[0]), "7777"),
                 "landscape drew the panel without the talkgroup in it");
    LS_CHECK(escaped(PANES[0]) == 0);
}

LS_CASE(the_table_is_the_size_of_what_it_holds)
{

    no_traffic();
    draw_decode_now(PORTRAIT);
    const int clear_when_empty = untouched_rows(table_of(PORTRAIT));

    for (unsigned i = 1; i <= 16; ++i)
        heard((uint16_t)(2000 + i), i * 1000u, P25_TG_SEEN_LCW);
    ls_shim_time_set(60 * 1000000LL);
    draw_decode_now(PORTRAIT);
    const int clear_when_full = untouched_rows(table_of(PORTRAIT));

    LS_CHECK_MSG(clear_when_empty >= 16,
                 "an empty table left only %d rows of the body clear - it is "
                 "a frame stretched over the space, not a box its own size",
                 clear_when_empty);
    LS_CHECK_MSG(clear_when_full + 8 < clear_when_empty,
                 "sixteen talkgroups left %d rows clear and none left %d - "
                 "the frame is not sized to its contents",
                 clear_when_full, clear_when_empty);
    LS_CHECK(escaped(PORTRAIT) == 0);
}

LS_CASE(held_p25_keeps_a_working_hold_button_in_both_postures)
{
    const ls_wf_cfg_t saved = *ls_wf_cfg();
    const bool saved_spectrum = s_have_spectrum;
    s_have_spectrum = true;
    ls_scr_p25.key(LS_TK_CHAR, '2');
    for (int posture = 0; posture < 2; posture++) {
        for (int empty = 0; empty < 2; empty++) {
            ls_wf_cfg_t cfg = saved;
            cfg.paused = false;
            ls_wf_cfg_set(&cfg);
            ls_wf_claim(LS_WF_OWNER_NONE, NULL);
            ls_wf_claim(LS_WF_OWNER_P25, "P25");
            if (!empty) feed_row();
            cfg.paused = true;
            ls_wf_cfg_set(&cfg);
            fresh();
            draw_pane(&ls_scr_p25, PANES[posture]);
            if (posture == 0) {
                ls_scr_p25.key(LS_TK_CHAR, 'b');
                fresh();
                draw_pane(&ls_scr_p25, PANES[posture]);
            }
            int hx = -1, hy = -1;
            for (int y = 0; y < H; y++) {
                for (int x = 0; x + 4 < W; x++) {
                    if (g_back[y * W + x].ch == 'H' &&
                        g_back[y * W + x + 1].ch == 'O' &&
                        g_back[y * W + x + 2].ch == 'L' &&
                        g_back[y * W + x + 3].ch == 'D') {
                        hx = x; hy = y;
                    }
                }
            }
            LS_CHECK_MSG(hx >= 0, "no HOLD control, posture=%d empty=%d", posture, empty);
            if (hx >= 0) {
                LS_CHECK(ls_scr_p25.touch(hx + 1, hy));
                LS_CHECK_MSG(!ls_wf_cfg()->paused, "visible HOLD did not resume");
                const uint32_t before = ls_wf_seq();
                feed_row();
                LS_CHECK(ls_wf_seq() != before);
            }
            if (posture == 0) ls_scr_p25.key(LS_TK_CHAR, 'b');
        }
    }
    ls_wf_cfg_set(&saved);
    s_have_spectrum = saved_spectrum;
}
