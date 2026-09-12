/* LS_TEST_SOURCES: ${FW}/components/apps/tui/ls_tui_screen.c + chrome + core */
/* The router: what a tap means, and what a key means. */

#include "ls_test.h"
#include "tui_core.h"
#include "ls_tui_screen.h"
#include "ls_notify.h"
#include "ls_tui_chrome.h"
#include "ls_theme.h"

#include <string.h>

/* ---------------------------------------------------------------- stubs -- */

static int s_cols = 115, s_rows = 27;

void ls_tui_geometry(int *cols, int *rows, int *cw, int *ch)
{
    if (cols) *cols = s_cols;
    if (rows) *rows = s_rows;
    if (cw) *cw = 10;
    if (ch) *ch = 17;
}
void ls_tui_invalidate(void) { }

/* How many cells at each end of a chrome row carry no words. The
   real ls_tui.c works it out from the panel's pixels; the router only ever
   sees the count, so a case sets it. Zero unless a case says otherwise. */
static int s_corner_pad;
int ls_tui_corner_pad(int row) { (void)row; return s_corner_pad; }

static const ls_tui_theme_t *s_active;
const ls_tui_theme_t *ls_tui_get_theme(void)
{
    return s_active ? s_active : ls_tui_theme_at(0);
}
void ls_tui_set_theme(const ls_tui_theme_t *t) { s_active = t; }
/* The waterfall linked in here asks whether the ground is white. */
bool ls_tui_daylight(void) { return false; }

/* --------------------------------------------------------- test screens -- */

static int s_keys_seen[8];
static ls_tk_t s_last_key[8];
static bool s_consume;
static int s_enters[8], s_leaves[8];

#define SCREEN(idx, nm)                                                      \
    static void enter##idx(void) { s_enters[idx]++; }                       \
    static void leave##idx(void) { s_leaves[idx]++; }                       \
    static bool key##idx(ls_tk_t k, char ch) {                               \
        (void)ch; s_keys_seen[idx]++; s_last_key[idx] = k;                   \
        return s_consume;                                                    \
    }                                                                        \
    static void draw##idx(tui_surface *sf, tui_rect a) { (void)sf; (void)a; }\
    static const ls_tui_screen_t SCR##idx = {                                \
        .name = nm, .hint = "H hint", .enter = enter##idx, .leave = leave##idx,          \
        .draw = draw##idx, .key = key##idx }

SCREEN(0, "HOME");
SCREEN(1, "P25");
SCREEN(2, "FM");
SCREEN(3, "ADSB");

/* Stable descriptors for the capacity regression below. */
static ls_tui_screen_t s_extra_screens[LS_TUI_MAX_SCREENS];

static int s_rotates;
static void on_rotate(void) { s_rotates++; }

static tui_cell g_back[140 * 70];
static tui_cell g_front[140 * 70];
static tui_surface g_sf;

static void setup(int cols, int rows)
{
    ls_tui_screen_set_dispatch_cb(NULL);
    s_cols = cols; s_rows = rows;
    s_corner_pad = 0;
    tui_surface_setup(&g_sf, g_back, g_front, cols, rows);
    memset(s_keys_seen, 0, sizeof(s_keys_seen));
    s_consume = false;
    s_rotates = 0;
    ls_tui_screen_set_rotate_cb(on_rotate);

    /* Registration is idempotent per process: the router keeps a static
       table, so register once and reset the selection between cases. */
    if (ls_tui_screen_count() == 0) {
        ls_tui_screen_register(&SCR0);
        ls_tui_screen_register(&SCR1);
        ls_tui_screen_register(&SCR2);
        ls_tui_screen_register(&SCR3);
    }
    ls_tui_screen_show(0);

    /* The router keeps the help overlay in a static, and there is no reset
       entry point, so a case that leaves it open would change what the next
       case's first touch means. One tap on the status row closes it if it is
       open and otherwise lands on the clock, which answers nothing ().
       Either way each case starts from the same state. */
    ls_tui_router_touch(0, 0);
    s_rotates = 0;

    /* A draw is what assigns the tab strip its column ranges, so a touch test
       that skips it is testing stale or zeroed bounds. */
    ls_tui_router_draw(&g_sf);
}

static int s_queued_screen;
static bool s_defer_switch;
static bool defer_switch(int index)
{
    if (!s_defer_switch) return false;
    s_queued_screen = index;
    return true;
}

LS_CASE(deferred_switch_waits_before_releasing_the_current_screen)
{
    setup(120, 32);
    memset(s_enters, 0, sizeof(s_enters));
    memset(s_leaves, 0, sizeof(s_leaves));
    s_queued_screen = -1;
    s_defer_switch = true;
    ls_tui_screen_set_dispatch_cb(defer_switch);
    ls_tui_screen_show(2);
    LS_CHECK(s_queued_screen == 2);
    LS_CHECK(ls_tui_screen_current() == 0);
    LS_CHECK(s_leaves[0] == 0 && s_enters[2] == 0);
    ls_tui_screen_show(0);
    LS_CHECK(s_queued_screen == 0);
    ls_tui_screen_show(-1);
    LS_CHECK(s_queued_screen == 0);
    ls_tui_screen_show(2);
    s_defer_switch = false;
    ls_tui_screen_show(s_queued_screen);
    LS_CHECK(ls_tui_screen_current() == 2);
    LS_CHECK(s_leaves[0] == 1 && s_enters[2] == 1);
    ls_tui_screen_set_dispatch_cb(NULL);
}

/* ---------------------------------------------------------------- cases -- */

LS_CASE(no_tap_on_the_status_row_turns_the_screen)
{

    setup(115, 27);

    const ls_tui_status_layout_t l = ls_tui_status_layout(115, 9, 0, 0, 0);
    LS_CHECK_MSG(l.clock_x >= 0, "no room for the clock at 115");
    LS_CHECK_MSG(l.help_x >= 0, "no room for help at 115");

    /* [?] is skipped here because it opens an overlay the next tap would
       close; the help cases below tap it on purpose. */
    for (int c = 0; c < 115; c++) {
        if (c >= l.help_x && c < l.help_x + LS_TUI_HELP_W) continue;
        LS_CHECK_MSG(ls_tui_router_touch(c, 0),
                     "column %d of the status row let its tap through", c);
    }
    LS_CHECK_MSG(s_rotates == 0,
                 "the status row turned the screen %d times", s_rotates);

    LS_CHECK(ls_tui_router_touch(l.help_x, 0));
    LS_CHECK_MSG(s_rotates == 0, "[?] turned the screen");
    ls_tui_router_touch(1, 1);          /* close the key list */
}

LS_CASE(f11_rotates_too)
{
    setup(115, 27);
    LS_CHECK(ls_tui_router_key(LS_TK_F11, 0));
    LS_EQ_INT(1, s_rotates);
}

LS_CASE(tapping_a_tab_selects_that_screen)
{
    setup(115, 27);
    /* Walk every column of the strip. Each one either selects the screen whose
       label covers it or is a gap, and no column may select the wrong one. */
    for (int want = 0; want < 4; want++) {
        int hits = 0;
        for (int x = 0; x < s_cols; x++) {
            ls_tui_screen_show(0);
            ls_tui_router_draw(&g_sf);
            ls_tui_router_touch(x, 1);
            if (ls_tui_screen_current() == want && want != 0) hits++;
        }
        if (want) LS_CHECK_MSG(hits > 0, "no column selects screen %d", want);
    }
}

LS_CASE(a_tap_on_the_strip_never_reaches_the_screen)
{
    /* A miss between two tabs must be swallowed. Letting it through would
       reach the body handler and move a selection the user was not touching. */
    setup(115, 27);
    memset(s_keys_seen, 0, sizeof(s_keys_seen));
    for (int x = 0; x < s_cols; x++) ls_tui_router_touch(x, 1);
    for (int i = 0; i < 4; i++)
        LS_EQ_INT(0, s_keys_seen[i]);
}

LS_CASE(the_body_maps_to_up_enter_and_down_by_thirds)
{
    setup(115, 27);
    s_consume = true;

    int body_top = 2, body_h = s_rows - 3;
    ls_tui_screen_show(0);

    /* Top third. */
    ls_tui_router_touch(50, body_top);
    LS_EQ_INT(LS_TK_UP, s_last_key[0]);

    /* Bottom third, one row above the hint bar. */
    ls_tui_router_touch(50, body_top + body_h - 1);
    LS_EQ_INT(LS_TK_DOWN, s_last_key[0]);

    /* Middle. */
    ls_tui_router_touch(50, body_top + body_h / 2);
    LS_EQ_INT(LS_TK_ENTER, s_last_key[0]);
}

LS_CASE(the_last_row_opens_the_key_list_and_anything_closes_it)
{
    setup(115, 27);
    LS_CHECK(ls_tui_router_touch(10, s_rows - 1));

    memset(s_keys_seen, 0, sizeof(s_keys_seen));
    LS_CHECK(ls_tui_router_touch(50, 10));
    LS_EQ_INT(0, s_keys_seen[0]);

    /* And it is now closed, so the next tap behaves normally. */
    s_consume = true;
    ls_tui_router_touch(50, 3);
    LS_CHECK(s_keys_seen[0] > 0);
}

LS_CASE(function_keys_select_screens_and_do_not_reach_them)
{
    setup(115, 27);
    s_consume = false;
    static const ls_tk_t F[] = { LS_TK_F1, LS_TK_F2, LS_TK_F3, LS_TK_F4 };
    for (int i = 0; i < 4; i++) {
        LS_CHECK(ls_tui_router_key(F[i], 0));
        LS_EQ_INT(i, ls_tui_screen_current());
    }
}

LS_CASE(a_screen_that_consumes_a_key_stops_the_router_seeing_it)
{
    /* The contract that lets a screen with a text field keep TAB. */
    setup(115, 27);
    ls_tui_screen_show(0);
    s_consume = true;
    ls_tui_router_key(LS_TK_TAB, 0);
    LS_EQ_INT(0, ls_tui_screen_current());

    s_consume = false;
    ls_tui_router_key(LS_TK_TAB, 0);
    LS_CHECK(ls_tui_screen_current() != 0);
}

LS_CASE(the_router_draws_within_the_grid_at_both_geometries)
{
    /* The help overlay is a fixed 46 columns wide, which is one column of
       margin either side in 48-column portrait. Draw it at both real sizes
       and check nothing landed off the grid. */
    static const int GEOM[][2] = { { 115, 27 }, { 48, 66 } };
    for (unsigned g = 0; g < 2; g++) {
        setup(GEOM[g][0], GEOM[g][1]);
        for (int i = 0; i < 140 * 70; i++) g_back[i].ch = '#';

        ls_tui_router_touch(1, GEOM[g][1] - 1);   /* open the key list */
        ls_tui_router_draw(&g_sf);

        /* The surface's stride is its own width, so "outside the grid" is
           every cell past cols*rows in the backing store, not a rectangle in
           some wider buffer. Getting that wrong reads row n+1 as if it were
           off the end of row n. */
        int used = GEOM[g][0] * GEOM[g][1];
        for (int i = used; i < 140 * 70; i++)
            LS_CHECK_MSG(g_back[i].ch == '#',
                         "wrote past the %dx%d grid at cell %d",
                         GEOM[g][0], GEOM[g][1], i);
        ls_tui_router_touch(1, 1);   /* close it again */
    }
}

LS_CASE(a_tap_outside_the_grid_is_harmless)
{
    setup(115, 27);
    s_consume = true;
    memset(s_keys_seen, 0, sizeof(s_keys_seen));
    ls_tui_router_touch(-1, -1);
    ls_tui_router_touch(9999, 9999);
    ls_tui_router_touch(-5, 10);
    ls_tui_router_touch(10, -5);
    /* Nothing to assert beyond surviving: the point is that a touch
       controller reporting a spurious coordinate cannot drive the UI. */
    LS_CHECK(1);
}

/* The left status belongs to the screen that wrote it. */

/* Row 0 of the drawn grid, as a string. The status bar is the only thing on
   it, so anything found here is on the glass. */
static void status_row(char *out, size_t n)
{
    size_t k = 0;
    for (int x = 0; x < s_cols && k + 1 < n; x++) out[k++] = g_back[x].ch;
    out[k] = 0;
}

LS_CASE(a_screens_status_note_does_not_follow_you_to_the_next_screen)
{

    setup(115, 27);
    ls_tui_status_set("z12C 12/12t r302 c4", NULL);
    ls_tui_router_draw(&g_sf);

    char row[160];
    status_row(row, sizeof(row));
    LS_CHECK_MSG(strstr(row, "z12C") != NULL,
                 "precondition: the note should be on screen first, got '%s'",
                 row);

    ls_tui_screen_show(2);
    ls_tui_router_draw(&g_sf);
    status_row(row, sizeof(row));
    LS_CHECK_MSG(strstr(row, "z12C") == NULL,
                 "the previous screen's note is still in the status bar: '%s'",
                 row);
}

LS_CASE(the_right_status_survives_a_screen_change)
{

    setup(115, 27);
    ls_tui_status_set(NULL, "KBD Terminal Bay");
    ls_tui_router_draw(&g_sf);

    ls_tui_screen_show(3);
    ls_tui_router_draw(&g_sf);

    char row[160];
    status_row(row, sizeof(row));
    LS_CHECK_MSG(strstr(row, "KBD") != NULL,
                 "the keyboard indicator was cleared by a screen change: '%s'",
                 row);
}

/* ------------------------------------------- the unread count -- */

/* Run the banner out. SHOW_FRAMES is 150 and the poll decrements once a
   call, so this is comfortably past the end of it. */
static void notices_expire(int visible)
{
    for (int i = 0; i < 200; i++) ls_notify_poll(visible);
}

static void post_on(int screen, const char *title)
{
    ls_notice_t n;
    memset(&n, 0, sizeof(n));
    snprintf(n.title, sizeof(n.title), "%s", title);
    n.screen = screen;
    ls_notify_post(&n);
}

LS_CASE(an_unread_count_is_cleared_by_looking_at_what_it_counts)
{

    setup(115, 27);
    ls_notify_clear();

    post_on(1, "A MESSAGE");
    LS_EQ_INT(1, ls_notify_unread());

    notices_expire(0);
    LS_CHECK_MSG(ls_notify_unread() == 1,
                 "the count cleared itself while the operator was on another "
                 "screen - then it is not a count of anything");

    /* And on the screen it came from, it has been seen. */
    ls_notify_poll(1);
    LS_CHECK_MSG(ls_notify_unread() == 0,
                 "the count is still %d with its own screen in front - the "
                 "badge says unread while it is being read",
                 ls_notify_unread());
}

LS_CASE(a_notice_with_nowhere_to_go_is_read_when_its_banner_ends)
{
    /* The other half, and the one that made the badge permanent.

       A notice whose screen is -1 has no app behind it - the alert self test
       posts one, and so does everything the console posts. Those can never
       reach the rule above, because there is no screen that being on counts
       as having read them. Three of them from testing left "3 MSG" on the
       status row with nothing on the unit able to clear it. */
    setup(115, 27);
    ls_notify_clear();

    post_on(-1, "SELF TEST");
    LS_EQ_INT(1, ls_notify_unread());

    /* While the banner is up it is still unread - it is being shown. */
    ls_notify_poll(0);
    LS_CHECK_MSG(ls_notify_unread() == 1,
                 "a notice was marked read before its banner had finished");

    notices_expire(0);
    LS_CHECK_MSG(ls_notify_unread() == 0,
                 "a notice with no screen behind it left a count of %d that "
                 "nothing on the unit can clear",
                 ls_notify_unread());
}

LS_CASE(the_badge_blinks_only_while_something_is_unread)
{
    /* And it must be steady when there is nothing to say: a chrome
       row with something flashing in it for no reason is worse than one that
       never flashes at all. */
    setup(115, 27);
    ls_notify_clear();

    bool seen_on = false, seen_off = false;
    for (int i = 0; i < 60; i++) {
        ls_notify_poll(0);
        if (ls_notify_badge_inverted()) seen_on = true; else seen_off = true;
    }
    LS_CHECK_MSG(!seen_on,
                 "the badge inverted with nothing unread");

    post_on(1, "A MESSAGE");
    seen_on = seen_off = false;
    /* Two seconds at 25 frames a second covers a full cycle either way. */
    for (int i = 0; i < 50; i++) {
        ls_notify_poll(0);
        if (ls_notify_badge_inverted()) seen_on = true; else seen_off = true;
    }
    LS_CHECK_MSG(seen_on && seen_off,
                 "the badge did not change state across two seconds with a "
                 "message unread - on %d, off %d", seen_on, seen_off);

    ls_notify_clear();
}

/* --------------------------------------- words off the corners -- */

/* Anywhere on the grid, as text, with the non-printing cells as spaces. The
   key list is drawn over the body, so this is how a case sees it open. */
static bool grid_has(const char *needle)
{
    char line[160];
    for (int y = 0; y < s_rows; y++) {
        int k = 0;
        for (int x = 0; x < s_cols && k + 1 < (int)sizeof(line); x++) {
            const char c = g_back[(size_t)y * s_cols + x].ch;
            line[k++] = (c >= 0x20 && c < 0x7F) ? c : ' ';
        }
        line[k] = 0;
        if (strstr(line, needle)) return true;
    }
    return false;
}

LS_CASE(the_status_rows_words_start_where_the_corner_padding_ends)
{
    /*"the very top bar in portrait needs to have the text pushed
       in a little to avoid the round corners". The bar still runs the full
       width - it is furniture - and the words stop short of both ends by
       whatever ls_tui_corner_pad says, which is three cells either way up on
       the T-Display-P4. The count is set here; test_tui_inset owns the
       arithmetic that produces it. */
    setup(54, 71);
    s_corner_pad = 3;
    ls_tui_status_set_clock("14:32Z");
    ls_tui_router_draw(&g_sf);

    const uint8_t bar = TUI_ATTR(TUI_BLACK, TUI_CYAN);
    for (int x = 0; x < 3; x++) {
        const tui_cell *l = &g_back[x], *r = &g_back[s_cols - 1 - x];
        LS_CHECK_MSG(l->ch == ' ' && r->ch == ' ',
                     "corner cell %d of the status row carries '%c' / '%c'",
                     x, l->ch, r->ch);
        LS_CHECK_MSG(l->attr == bar && r->attr == bar,
                     "the bar stopped short of corner cell %d", x);
    }
    char row[160];
    status_row(row, sizeof(row));
    /* The clock where [R] was, a space, then [?]. */
    LS_CHECK_MSG(strncmp(row + 3, "14:32Z [?]", 10) == 0,
                 "the clock and help are not where the padding ends: '%s'",
                 row);
    ls_tui_status_set_clock("");        /* the router keeps it in a static */
}

LS_CASE(the_help_control_is_tapped_where_it_moved_to)
{
    /* A label that moves and a target that does not is the worst of
       both: a control drawn in from the corner and answering from column 0
       would make the bar's corner a hidden control and the label a dead one.
       The corner cells swallow a tap and do nothing; [?] answers where it is
       drawn.
       The clock took [R]'s slot, so [?] is one clock and one space
       in from the padding, and the clock's cells answer nothing. */
    setup(54, 71);
    s_corner_pad = 3;
    ls_tui_router_draw(&g_sf);

    for (int c = 0; c < 3; c++) {
        LS_CHECK(ls_tui_router_touch(c, 0));
        LS_CHECK(ls_tui_router_touch(s_cols - 1 - c, 0));
    }
    ls_tui_router_draw(&g_sf);
    LS_CHECK_MSG(s_rotates == 0 && !grid_has("KEYS"),
                 "a corner cell of the status row acted as a control "
                 "(%d turns)", s_rotates);

    for (int c = 3; c < 3 + LS_TUI_CLOCK_W + 1; c++)
        LS_CHECK(ls_tui_router_touch(c, 0));
    ls_tui_router_draw(&g_sf);
    LS_CHECK_MSG(s_rotates == 0 && !grid_has("KEYS"),
                 "a tap on the clock acted as a control (%d turns)",
                 s_rotates);

    LS_CHECK(ls_tui_router_touch(3 + LS_TUI_CLOCK_W + 1, 0));
    ls_tui_router_draw(&g_sf);
    LS_CHECK_MSG(s_rotates == 0 && grid_has("KEYS"),
                 "[?] after the clock did not open the key list");
    ls_tui_router_touch(1, 1);          /* close it */
}

LS_CASE(the_hint_row_keeps_its_words_off_the_bottom_corners)
{
    /* Landscape has a hint row along the bottom edge, which reaches
       the other two corners. It kept one column clear at each end already;
       a pad of three moves the hint and its tail in by two more, together. */
    setup(120, 32);
    s_corner_pad = 3;
    ls_tui_router_draw(&g_sf);

    const tui_cell *last = &g_back[(size_t)(s_rows - 1) * s_cols];
    for (int x = 0; x < 3; x++)
        LS_CHECK_MSG(last[x].ch == ' ' && last[s_cols - 1 - x].ch == ' ',
                     "corner cell %d of the hint row carries '%c' / '%c'",
                     x, last[x].ch, last[s_cols - 1 - x].ch);
    LS_CHECK_MSG(last[3].ch == 'H',
                 "the hint does not start where the padding ends");
    LS_CHECK_MSG(last[s_cols - 4].ch == 'n',
                 "the tail does not end where the padding starts");
}

LS_CASE(the_thirteenth_app_is_reachable_and_capacity_is_bounded)
{
    setup(54, 71);
    while (ls_tui_screen_count() < LS_TUI_MAX_SCREENS) {
        int i = ls_tui_screen_count();
        s_extra_screens[i] = SCR0;
        s_extra_screens[i].name = i == 12 ? "LINK" : "EXTRA";
        int registered = ls_tui_screen_register(&s_extra_screens[i]);
        LS_CHECK_MSG(registered == i, "screen %d was silently rejected", i);
        if (registered < 0) break;
    }
    LS_CHECK(ls_tui_screen_count() == LS_TUI_MAX_SCREENS);
    LS_CHECK(ls_tui_screen_register(&SCR0) == -1);
    ls_tui_screen_show(12);
    LS_CHECK(ls_tui_screen_current() == 12);
    const char *name = ls_tui_screen_name(12);
    LS_CHECK(name && strcmp(name, "LINK") == 0);
    ls_tui_screen_show(0);
    ls_tui_screen_set_tab_count(4);
}
