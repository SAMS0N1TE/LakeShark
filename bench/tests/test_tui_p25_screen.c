/* LS_TEST_SOURCES: scr_p25.c + ls_waterfall.c + p25_tg_observed.c + tui_core.c */

#include "ls_test.h"
#include "scan_engine.h"
#include "tui_core.h"
#include "ls_tui_screen.h"
#include "ls_theme.h"
#include "p25_state.h"
#include "p25_tg_observed.h"
#include "esp_timer.h"
#include "ls_waterfall.h"
#include "ls_wf_source.h"
#include "p25_spectrum.h"
extern int ls_test_wf_releases;
#include "ls_tui.h"
#include "p25_p2_runtime.h"
#include "ls_action.h"
#include "ls_value.h"
#include "lakeshark_backend.h"
#include "ls_options.h"
#include "ls_picker.h"
/* The deck's parts, and the keypad it must not open by accident. */
#include "ls_radio_panel.h"
#include "ls_numpad.h"
#include "audio/audio_out.h"
#include "radio_choice.h"
#include "ls_radio_select.h"
#include "ls_lora.h"
static void filter_option(const char *name);

#include <string.h>

/* Link-time tripwires at the hardware/flash and command boundaries. */
static unsigned option_locks, option_spi, option_nvs, option_actions, option_reads;
void __wrap_ls_lora_hw_lock(void) { option_locks++; }
int __wrap_spi_device_transmit(void *dev, void *transaction)
{ (void)dev; (void)transaction; option_spi++; return -1; }
int __wrap_spi_device_polling_transmit(void *dev, void *transaction)
{ (void)dev; (void)transaction; option_spi++; return -1; }
#define NVS_TRIPWIRE(type, value_type) \
    int __wrap_nvs_set_##type(unsigned handle, const char *key, value_type value) \
    { (void)handle; (void)key; (void)value; option_nvs++; return -1; }
NVS_TRIPWIRE(u8, uint8_t)
NVS_TRIPWIRE(i8, int8_t)
NVS_TRIPWIRE(u32, uint32_t)
NVS_TRIPWIRE(i32, int32_t)
NVS_TRIPWIRE(u64, uint64_t)
#define NVS_READ_TRIPWIRE(type, value_type) \
    int __wrap_nvs_get_##type(unsigned handle, const char *key, value_type *value) \
    { (void)handle; (void)key; (void)value; option_nvs++; return -1; }
NVS_READ_TRIPWIRE(u8, uint8_t)
NVS_READ_TRIPWIRE(i8, int8_t)
NVS_READ_TRIPWIRE(u32, uint32_t)
NVS_READ_TRIPWIRE(i32, int32_t)
NVS_READ_TRIPWIRE(u64, uint64_t)
int __wrap_nvs_commit(unsigned handle) { (void)handle; option_nvs++; return -1; }
ls_act_status_t __real_ls_action_call(const char *, const ls_args_t *, ls_val_t *, ls_cap_t);
ls_act_status_t __wrap_ls_action_call(const char *name, const ls_args_t *in, ls_val_t *out, ls_cap_t cap)
{ option_actions++; return __real_ls_action_call(name, in, out, cap); }
/* Count actual row getter work even when the formatted value is unchanged. */
const char *__real_lakeshark_p25_mode_name(void);
const char *__wrap_lakeshark_p25_mode_name(void)
{ option_reads++; return __real_lakeshark_p25_mode_name(); }

/* ---------------------------------------------------------------- fakes -- */

p25_state_t P25;
scan_state_t SCAN;
uint32_t s_tune_freq_hz;

void p25_get_receiver_status(ls_iq_control_status_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
}

/* The IQ probe the SIGNAL panel reads, stubbed in tools/lssim_field.c
   because every target that compiles scr_p25.c needs one. */
void ls_sim_set_clipping(int percent);
#define set_clipping ls_sim_set_clipping

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

/* The music waterfall's image cells; nothing in this test draws pixels. */
void ls_tui_image(tui_rect cells, const uint16_t *src, int w, int h, uint32_t serial) { (void)cells; (void)src; (void)w; (void)h; (void)serial; }
bool ls_tui_image_now(tui_rect *cells, const uint16_t **src, int *w, int *h, uint32_t *serial) { (void)cells; (void)src; (void)w; (void)h; (void)serial; return false; }
void ls_tui_image_ring(tui_rect cells, const uint16_t *src, int w, int h, int x0, int y0, int dx, int dy, uint32_t serial) { (void)cells; (void)src; (void)w; (void)h; (void)x0; (void)y0; (void)dx; (void)dy; (void)serial; }
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
bool ls_tui_cutout(tui_rect *cells) { if (cells) *cells = tui_rect_make(0, 0, 0, 0); return false; }

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
    ls_scr_p25.key(LS_TK_CHAR, '1');
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

LS_CASE(split_roles_and_fallback_fit_the_decode_pane)
{
    no_traffic();
    P25.split_enabled = true;
    P25.split_active = true;
    P25.split_crc_percent = 80;
    P25.split_site = 2;
    P25.split_rfss = 1;
    P25.grant_on_traffic = true;
    P25.grant_talkgroup = 42;
    P25.grant_freq_hz = 851200000;
    snprintf(P25.split_reason, sizeof(P25.split_reason), "busy: lower/equal priority");
    draw_decode_now(PORTRAIT);
    LS_CHECK(escaped(PORTRAIT) == 0);
    LS_CHECK(rect_has(PORTRAIT, "CC: LR2021 site 1/2 CRC 80%"));
    LS_CHECK(rect_has(PORTRAIT, "VOICE: RTL TG 42 851.2000 MHz"));
    LS_CHECK(rect_has(PORTRAIT, "busy: lower/equal priority"));
    P25.split_active = false;
    P25.grant_on_traffic = false;
    draw_decode_now(PORTRAIT);
    LS_CHECK(rect_has(PORTRAIT, "CC: RTL fallback"));
    LS_CHECK(rect_has(PORTRAIT, "VOICE: RTL idle"));
    P25.split_enabled = false;
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

LS_CASE(a_front_end_driven_into_its_rails_says_so_on_the_level_line)
{
    /* A strong signal and a saturated front end both read near 1.0, and the
       cure for one is the opposite of the cure for the other. The share of
       samples pinned at the rails is what separates them. */
    no_traffic();

    set_clipping(0);
    draw_decode_now(PORTRAIT);
    LS_CHECK_MSG(!rect_has(PORTRAIT, "CLIP"),
                 "a clean signal was reported as clipping");

    set_clipping(5);
    draw_decode_now(PORTRAIT);
    LS_CHECK_MSG(!rect_has(PORTRAIT, "CLIP"),
                 "ordinary peaks were reported as clipping");

    set_clipping(35);
    draw_decode_now(PORTRAIT);
    LS_CHECK_MSG(rect_has(PORTRAIT, "CLIP 35%"),
                 "a saturated front end did not say so");

    set_clipping(0);
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

    /* One window, not two tabs: DECODE and SIGNAL are a single frame, and
       no page button carries either name any more. */
    LS_CHECK(rect_has(PORTRAIT, "DECODE / SIGNAL"));
    LS_CHECK(rect_has(PORTRAIT, "WATERFALL"));
    LS_CHECK(rect_has(PORTRAIT, "ACTIVITY"));

    /* SYNC is the decode half's last field and AUDIO the signal half's: if
       the window lost rows to the table, its bottom field is the one that
       goes. */
    LS_CHECK_MSG(rect_has(PORTRAIT, "SYNC"),
                 "the decode half lost its last field to the activity table");
    LS_CHECK_MSG(rect_has(PORTRAIT, "AUDIO"),
                 "the signal half lost its last field to the activity table");

    const int activity = rect_row_of(PORTRAIT, "ACTIVITY");
    const int rx       = rect_row_of(PORTRAIT, "AUDIO");
    LS_CHECK_MSG(activity > rx,
                 "the table is at row %d and SIGNAL's last field at %d - they "
                 "are drawn over each other", activity, rx);
}

static bool find_word(const char *word, int *sx, int *sy)
{
    const int n = (int)strlen(word);
    *sx = *sy = -1;
    for (int y = PORTRAIT.y; y < PORTRAIT.y + PORTRAIT.h && y < H; y++)
        for (int x = PORTRAIT.x; x + n < W && x < PORTRAIT.x + PORTRAIT.w; x++) {
            int i = 0;
            while (i < n && g_back[y * W + x + i].ch == word[i]) i++;
            if (i == n) { *sx = x; *sy = y; }
        }
    return *sx >= 0;
}

LS_CASE(the_views_are_tabs_and_vfo_is_the_way_back)
{
    /* VFO, WATERFALL and SCANNER are tabs on every view: the way back from
       the waterfall is the tab that says VFO, not a toggle pressed twice. */
    show_decode_page();
    no_traffic();
    draw_decode_now(PORTRAIT);
    int sx, sy;
    LS_CHECK_MSG(find_word("WATERFALL", &sx, &sy), "no WATERFALL tab on the VFO view");
    if (sx < 0) return;
    LS_CHECK(ls_scr_p25.touch(sx + 2, sy));
    fresh(); draw_pane(&ls_scr_p25, PORTRAIT);
    LS_CHECK_MSG(!rect_has(PORTRAIT, "DECODE / SIGNAL"),
                 "WATERFALL did not leave the VFO view");
    LS_CHECK_MSG(find_word("VFO", &sx, &sy), "no VFO tab on the waterfall");
    LS_CHECK_MSG(find_word("SCANNER", &sx, &sy), "no SCANNER tab on the waterfall");
    LS_CHECK(find_word("VFO", &sx, &sy));
    if (sx < 0) return;
    LS_CHECK(ls_scr_p25.touch(sx + 1, sy));
    fresh(); draw_pane(&ls_scr_p25, PORTRAIT);
    LS_CHECK_MSG(rect_has(PORTRAIT, "DECODE / SIGNAL"),
                 "the VFO tab did not come back to the VFO view");
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

LS_CASE(decode_uses_spare_space_for_health_and_keeps_busy_history_visible)
{

    no_traffic();
    draw_decode_now(PORTRAIT);
    LS_CHECK(rect_has(PORTRAIT, "RECEIVER HEALTH"));
    LS_CHECK(rect_has(PORTRAIT, "USB errors"));

    for (unsigned i = 1; i <= 16; ++i)
        heard((uint16_t)(2000 + i), i * 1000u, P25_TG_SEEN_LCW);
    ls_shim_time_set(60 * 1000000LL);
    draw_decode_now(PORTRAIT);
    LS_CHECK(rect_has(PORTRAIT, "2016"));
    LS_CHECK(rect_has(PORTRAIT, "ACTIVITY"));
    LS_CHECK(escaped(PORTRAIT) == 0);
}

LS_CASE(an_lr2021_in_use_elsewhere_says_so_in_the_health_panel)
{
    /* P25 left on the LR2021 hands the chip to an experiment or a sweep;
       the health panel says why nothing arrives. */
    no_traffic();
    P25.lora_wait = true;
    draw_decode_now(PORTRAIT);
    LS_CHECK_MSG(rect_has(PORTRAIT, "IN USE ELSEWHERE"),
                 "P25 waiting for the LR2021 does not say so");
    LS_CHECK(rect_has(PORTRAIT, "Read errors"));
    LS_CHECK(escaped(PORTRAIT) == 0);

    P25.lora_wait = false;
    draw_decode_now(PORTRAIT);
    LS_CHECK(!rect_has(PORTRAIT, "IN USE ELSEWHERE"));
}

LS_CASE(the_merged_decode_page_has_the_large_dial_and_receive_history)
{
    no_traffic();
    heard(4321, 1000, P25_TG_SEEN_HDU);
    ls_shim_time_set(4 * 1000000LL);
    draw_decode_now(PORTRAIT);

    LS_CHECK(rect_has(PORTRAIT, "P25 / RECEIVER"));
    LS_CHECK(rect_has(PORTRAIT, "ACTIVITY"));
    LS_CHECK(rect_has(PORTRAIT, "4321"));
    LS_CHECK(rect_has(PORTRAIT, "SCAN"));

    int blocks = 0;
    /* Under the control row, as FM's VFO puts its own. */
    for (int y = PORTRAIT.y; y < PORTRAIT.y + 14; ++y)
        for (int x = PORTRAIT.x; x < PORTRAIT.x + PORTRAIT.w; ++x)
            if (g_back[y * W + x].ch == LS_TUI_SHADE_FULL) blocks++;
    LS_CHECK_MSG(blocks >= 20, "merged decode dial used only %d large blocks", blocks);
}

LS_CASE(the_large_font_grid_keeps_history_and_all_quick_actions)
{
    const tui_rect large_font = {1, 2, 32, 41};
    no_traffic();
    heard(2468, 1000, P25_TG_SEEN_LCW);
    ls_shim_time_set(2 * 1000000LL);
    draw_decode_now(large_font);

    LS_CHECK(rect_has(large_font, "P25 / RECEIVER"));
    LS_CHECK(rect_has(large_font, "ACTIVITY"));
    LS_CHECK(rect_has(large_font, "2468"));
    LS_CHECK(rect_has(large_font, "VOLUME"));
    LS_CHECK(rect_has(large_font, "GAIN"));
    LS_CHECK(escaped(large_font) == 0);
}

/* ------------------------------------------------------- the P25 deck -- */

/* The decode page carries the scanner's deck: a travel that sets where it is
   pressed, and a bracketed number that is the only way to the keypad. Both
   drive the real actions - nothing here is decoration. */
extern bool ls_scr_p25_control_rect(int card, int part, int *x, int *y, int *w, int *h);
extern bool ls_scr_p25_pad_rect(char c, int *x, int *y);
static bool press_pad(char c)
{
    int x, y;
    return ls_scr_p25_pad_rect(c, &x, &y) && ls_scr_p25.touch(x, y);
}

static long s_deck_vol = -1;
static ls_act_status_t a_deck_volume(const ls_args_t *in, ls_val_t *out)
{
    s_deck_vol = in->v[0].i;
    out->kind = LS_VAL_INT;
    out->i = s_deck_vol;
    return LS_ACT_OK;
}
/* The step keys read the value before they move it, the same as the keyboard
   does, so the reading has to exist for the key to reach the action. */
static long s_deck_vol_state = 40;
static bool v_deck_volume(ls_val_t *v)
{
    v->kind = LS_VAL_INT;
    v->i = s_deck_vol_state;
    return true;
}

LS_CASE(the_p25_deck_bar_is_a_slider_and_the_title_value_opens_exact_entry)
{
    no_traffic();
    P25.lora_rx = false;
    ls_action_register("audio.volume", "i", LS_CAP_TUNE, a_deck_volume, "test volume");
    ls_value_publish("sys.volume", "percent", v_deck_volume);
    const tui_rect panes[] = {PORTRAIT, {1, 2, 113, 24}};
    for (int posture = 0; posture < 2; ++posture) {
        ls_numpad_close();
        draw_decode_now(panes[posture]);
        for (int card = 0; card < 2; ++card) {
            int x, y, w, h;
            LS_CHECK(ls_scr_p25_control_rect(card, LS_RADIO_PART_VALUE, &x, &y, &w, &h));
            for (int cy = y; cy < y + h; ++cy)
                for (int cx = x; cx < x + w; ++cx) {
                    LS_CHECK(ls_scr_p25.touch(cx, cy));
                    LS_CHECK_MSG(ls_numpad_active(), "card %d value cell %d,%d", card, cx, cy);
                    ls_numpad_close();
                }
            /* The bar never opens the editor: it is a slider. */
            LS_CHECK(ls_scr_p25_control_rect(card, LS_RADIO_PART_BAR, &x, &y, &w, &h));
            for (int cy = y; cy < y + h; ++cy)
                for (int cx = x; cx < x + w; ++cx) {
                    s_deck_vol = -1;
                    LS_CHECK(ls_scr_p25.touch(cx, cy));
                    LS_CHECK_MSG(!ls_numpad_active(), "card %d bar cell %d,%d opened an editor", card, cx, cy);
                    if (card == 0) LS_CHECK_MSG(s_deck_vol >= 0, "volume bar cell %d,%d set nothing", cx, cy);
                }
        }
        /* Left end of the volume bar is quiet, right end is full. */
        {
            int x, y, w, h;
            LS_CHECK(ls_scr_p25_control_rect(0, LS_RADIO_PART_BAR, &x, &y, &w, &h));
            s_deck_vol = -1; LS_CHECK(ls_scr_p25.touch(x + w - 1, y));
            LS_EQ_INT(s_deck_vol, 100);
            s_deck_vol = -1; LS_CHECK(ls_scr_p25.touch(x, y));
            LS_CHECK(s_deck_vol >= 0 && s_deck_vol <= 15);
        }
        for (int part = LS_RADIO_PART_DOWN; part <= LS_RADIO_PART_UP; ++part) {
            int x, y, w, h;
            LS_CHECK(ls_scr_p25_control_rect(0, part, &x, &y, &w, &h));
            for (int cy = y; cy < y + h; ++cy)
                for (int cx = x; cx < x + w; ++cx) {
                    s_deck_vol = -1;
                    LS_CHECK(ls_scr_p25.touch(cx, cy));
                    LS_EQ_INT(s_deck_vol, part == LS_RADIO_PART_UP ? 45 : 35);
                    LS_CHECK(!ls_numpad_active());
                }
        }
    }
}

/* TUNE IS STILL ON THE SCREEN, AND IT TYPES IN PLACE.

   The deck must not cost the one control this radio cannot do without. The
   toggle in the bottom right corner swaps the controls for the frequency
   keyboard, the keyboard types where the controls were - not over the decode
   - and TUNE applies it through the same action every other retune uses. */
static double s_tuned_mhz = -1;
static ls_act_status_t a_deck_freq(const ls_args_t *in, ls_val_t *out)
{
    s_tuned_mhz = in->v[0].f;
    out->kind = LS_VAL_FLOAT;
    out->f = in->v[0].f;
    return LS_ACT_OK;
}

/* Draw the page again without pressing '1' to get back to it: with the
   frequency keyboard up a digit IS a digit, the same rule the VFO's inline
   keypad follows, so a redraw must not type one. */
static void redraw(tui_rect pane)
{
    fresh();
    draw_pane(&ls_scr_p25, pane);
}

/* Press the first cell of a word the screen actually drew. */
static bool press_text(tui_rect r, const char *needle)
{
    const int len = (int)strlen(needle);
    for (int y = r.y; y < r.y + r.h && y < H; y++)
        for (int x = r.x; x + len <= r.x + r.w && x + len <= W; x++) {
            int n = 0;
            while (n < len && g_back[y * W + x + n].ch == needle[n]) n++;
            if (n == len) return ls_scr_p25.touch(x, y);
        }
    return false;
}

LS_CASE(the_frequency_keyboard_takes_the_decks_place_and_tunes)
{
    /* The deck, where the keys are: a digit printed in the decode above must
       never be mistaken for a key a thumb can press. */
    const tui_rect DECK = { PORTRAIT.x, PORTRAIT.y + PORTRAIT.h * 2 / 3,
                            PORTRAIT.w, PORTRAIT.h / 3 };
    no_traffic();
    P25.lora_rx = false;
    ls_action_register("p25.freq", "f", LS_CAP_TUNE, a_deck_freq, "test freq");
    draw_decode_now(PORTRAIT);
    ls_numpad_close();

    /* The controls are showing, and the way to the keyboard is a labelled
       target in the corner - not a popup and not a hidden key. */
    LS_CHECK_MSG(rect_has(PORTRAIT, "KEYBOARD"), "no way into the frequency keyboard");
    LS_CHECK(rect_has(PORTRAIT, "VOLUME"));
    LS_CHECK(press_text(DECK, "KEYBOARD"));
    redraw(PORTRAIT);

    /* It took the deck's place: the keys are where the cards were, and the
       decode above them is untouched. */
    LS_CHECK_MSG(rect_has(PORTRAIT, "FREQUENCY"), "the keyboard did not open in place");
    LS_CHECK_MSG(!rect_has(PORTRAIT, "VOLUME"), "the deck is still under the keyboard");
    LS_CHECK_MSG(rect_has(PORTRAIT, "CONTROLS"), "no way back to the controls");
    LS_CHECK_MSG(rect_has(PORTRAIT, "DECODE / SIGNAL"), "the keyboard covered the decode");
    LS_CHECK_MSG(!ls_numpad_active(), "the inline keyboard opened a window as well");
    LS_EQ_INT(escaped(PORTRAIT), 0);

    /* Type 851.3 and apply it. */
    s_tuned_mhz = -1;
    const char *digits = "851.3";
    for (const char *p = digits; *p; ++p) {
        LS_CHECK_MSG(press_pad(*p), "no key for '%c'", *p);
        redraw(PORTRAIT);
    }
    LS_CHECK_MSG(rect_has(PORTRAIT, "851.3"), "the keyboard did not show what was typed");
    LS_CHECK(press_text(DECK, "TUNE"));
    LS_CHECK_MSG(s_tuned_mhz > 851.29 && s_tuned_mhz < 851.31,
                 "TUNE asked for %.4f MHz", s_tuned_mhz);
    redraw(PORTRAIT);
    LS_CHECK_MSG(rect_has(PORTRAIT, "VOLUME"), "tuning did not put the controls back");

    /* A frequency this radio does not have is refused, and refusing it does
       not retune anything. */
    LS_CHECK(press_text(DECK, "KEYBOARD"));
    redraw(PORTRAIT);
    s_tuned_mhz = -1;
    for (const char *p = "9999"; *p; ++p) {
        LS_CHECK(press_pad(*p));
        redraw(PORTRAIT);
    }
    LS_CHECK(press_text(DECK, "TUNE"));
    LS_EQ_INT((int)s_tuned_mhz, -1);
    redraw(PORTRAIT);
    LS_CHECK_MSG(rect_has(PORTRAIT, "INVALID RANGE"), "a bad frequency was taken quietly");

    /* CANCEL puts the controls back and keeps the carrier where it was. */
    LS_CHECK(press_text(DECK, "CANCEL"));
    redraw(PORTRAIT);
    LS_CHECK(rect_has(PORTRAIT, "VOLUME"));
    LS_EQ_INT((int)s_tuned_mhz, -1);

    /* And the keyboard shortcut still reaches it: T opens, ESC closes. */
    LS_CHECK(ls_scr_p25.key(LS_TK_CHAR, 't'));
    redraw(PORTRAIT);
    LS_CHECK(rect_has(PORTRAIT, "FREQUENCY"));
    LS_CHECK(ls_scr_p25.key(LS_TK_CHAR, '8'));
    LS_CHECK(ls_scr_p25.key(LS_TK_CHAR, '5'));
    LS_CHECK(ls_scr_p25.key(LS_TK_CHAR, '2'));
    LS_CHECK(ls_scr_p25.key(LS_TK_ENTER, 0));
    LS_CHECK_MSG(s_tuned_mhz > 851.9 && s_tuned_mhz < 852.1,
                 "ENTER asked for %.4f MHz", s_tuned_mhz);
    redraw(PORTRAIT);
    LS_CHECK(rect_has(PORTRAIT, "VOLUME"));
    LS_CHECK(ls_scr_p25.key(LS_TK_CHAR, 't'));
    LS_CHECK(ls_scr_p25.key(LS_TK_ESC, 0));
    redraw(PORTRAIT);
    LS_CHECK(rect_has(PORTRAIT, "VOLUME"));
}

static long s_fake_lrgain;
static bool v_fake_lrgain(ls_val_t *v) { v->kind = LS_VAL_INT; v->i = s_fake_lrgain; return true; }
static ls_act_status_t a_fake_lrgain(const ls_args_t *in, ls_val_t *out)
{
    s_fake_lrgain = in->v[0].i;
    out->kind = LS_VAL_INT; out->i = s_fake_lrgain;
    return LS_ACT_OK;
}

LS_CASE(lr_iq_gain_keys_cannot_dispatch_retired_fsk_gain)
{
    const tui_rect large_font = {1, 2, 32, 41};
    ls_value_publish("p25.lrgain", "step", v_fake_lrgain);
    ls_action_register("p25.lrgain", "i", LS_CAP_TUNE, a_fake_lrgain, "test");
    no_traffic();
    s_fake_lrgain = 0;
    P25.lora_rx = true;
    draw_decode_now(large_font);
    LS_CHECK_MSG(!rect_has(large_font, "GAIN"),
                 "IQ path must not offer the retired FSK gain card");

    ls_scr_p25.key(LS_TK_CHAR, 'u');
    LS_EQ_INT((int)s_fake_lrgain, 0);
    ls_scr_p25.key(LS_TK_CHAR, 'u');
    LS_EQ_INT((int)s_fake_lrgain, 0);
    ls_scr_p25.key(LS_TK_CHAR, 'j');
    LS_EQ_INT((int)s_fake_lrgain, 0);
    LS_CHECK(escaped(large_font) == 0);

    P25.lora_rx = false;
    draw_decode_now(large_font);
    LS_CHECK(rect_has(large_font, "GAIN"));
}

LS_CASE(the_scanner_workflow_remains_reachable_from_the_merged_screen)
{
    ls_scr_p25.key(LS_TK_CHAR, '3');
    fresh(); draw_pane(&ls_scr_p25, PORTRAIT);
    LS_CHECK(rect_has(PORTRAIT, "SCAN"));
    LS_CHECK(rect_has(PORTRAIT, "LISTS"));
    /* 2026-10-19: the scanner's bar is LISTS / SETUP / BACK; P25's own
       OPTIONS stay reachable from the control row the scanner now keeps. */
    LS_CHECK(rect_has(PORTRAIT, "SETUP"));
    LS_CHECK(rect_has(PORTRAIT, "OPTIONS"));
    LS_CHECK(rect_has(PORTRAIT, "LIST"));
    LS_CHECK(rect_has(PORTRAIT, "BAND"));

    /* One key starts the scan; no choice page in between. */
    ls_scr_p25.key(LS_TK_CHAR, 's');
    LS_CHECK(scan_engine_active());
    fresh(); draw_pane(&ls_scr_p25, PORTRAIT);
    LS_CHECK(rect_has(PORTRAIT, "STOP"));
    ls_scr_p25.key(LS_TK_CHAR, 'x');
    LS_CHECK(!scan_engine_active());
    ls_scr_p25.key(LS_TK_CHAR, 'm');
    fresh(); draw_pane(&ls_scr_p25, PORTRAIT);
    LS_CHECK(rect_has(PORTRAIT, "P25 / RECEIVER"));
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

LS_CASE(settings_and_options_share_the_same_backend_controls)
{
    ls_opt_close(); ls_scr_p25.key(LS_TK_CHAR, '1');
    ls_scr_p25.key(LS_TK_CHAR, '4');
    LS_CHECK(ls_picker_active()); filter_option("Trunking auto-follow");
    bool before = p25_get_auto_follow();
    ls_picker_key(LS_TK_ENTER, 0);
    LS_CHECK(p25_get_auto_follow() != before);
    for (int p = 0; p < 2; p++) {
        fresh(); grid_for(PANES[p]); ls_picker_draw(&g_sf, PANES[p]);
        LS_EQ_INT(escaped(PANES[p]), 0);
    }
    ls_opt_close(); ls_scr_p25.key(LS_TK_CHAR, '1');
}

LS_CASE(waterfall_palette_shortcut_does_not_open_p25_settings)
{
    ls_scr_p25.key(LS_TK_CHAR,'2');
    int before=ls_wf_cfg()->palette;
    ls_scr_p25.key(LS_TK_CHAR,'p');
    LS_EQ_INT(ls_wf_cfg()->palette,(before+1)%LS_WF_PAL__COUNT);
}

LS_CASE(experimental_phase2_setting_is_reachable_in_both_postures)
{
    ls_opt_close(); p25_p2_enable(false);
    ls_scr_p25.key(LS_TK_CHAR, '1'); ls_scr_p25.key(LS_TK_CHAR, '4');
    filter_option("Phase II (exp)");
    for (int p = 0; p < 2; p++) {
        fresh(); grid_for(PANES[p]); ls_picker_draw(&g_sf, PANES[p]);
        LS_EQ_INT(escaped(PANES[p]), 0);
    }
    ls_picker_key(LS_TK_ENTER, 0); LS_CHECK(p25_p2_enabled());
    ls_picker_key(LS_TK_ENTER, 0); LS_CHECK(!p25_p2_enabled());
    ls_opt_close();
}

static uint32_t cursor_committed;
static bool cursor_tuner(ls_wf_owner_t owner,uint32_t hz)
{ (void)owner;cursor_committed=hz;return true; }
LS_CASE(p25_waterfall_arrows_select_space_commits)
{
    fresh();
    ls_scr_p25.key(LS_TK_CHAR,'2');
    ls_wf_claim(LS_WF_OWNER_NONE,NULL);
    feed_row();
    draw_pane(&ls_scr_p25,PANES[0]);
    ls_wf_set_tuner(cursor_tuner);cursor_committed=0;
    uint32_t start=ls_wf_marker_hz();LS_CHECK(start>0);
    ls_scr_p25.key(LS_TK_RIGHT,0);
    LS_CHECK(ls_wf_marker_hz()>start);LS_EQ_INT(cursor_committed,0);
    uint32_t selected=ls_wf_marker_hz();
    ls_scr_p25.key(LS_TK_CHAR,' ');
    LS_EQ_INT(cursor_committed,selected);
    ls_wf_set_tuner(NULL);
}

/* The radio panel shows system volume; this screen test does not run audio. */
int audio_volume_get(void) { return 60; }

/* scr_p25.c gained a profile picker, which reaches the PROGRAM session.  That
   session lives in p25_program_sd.c, which the bench deliberately does not
   link - a host test that really loaded a profile off a card would be lying
   about what it exercised.  NULL here is the same first-run empty state a
   board shows before any profile has been read. */
#include "p25_program.h"
const p25_program_t *p25_program_session(void) { return NULL; }
bool p25_program_request_reload_path(const char *path)
{ (void)path; return false; }

#include "dmr_watch.h"
#include "dmr_gen.h"
#include "ls_tui_ui.h"
#include "ls_options.h"
#include "ls_picker.h"

static void dmr_ui_feed(const uint8_t *b, unsigned n)
{
    for (unsigned i = 0; i < n * 8; i += 2) {
        unsigned hi = (b[i/8] >> (7-i%8)) & 1;
        unsigned lo = (b[i/8] >> (6-i%8)) & 1;
        dmr_watch_symbol(hi ? (lo ? -3600 : -1200) : (lo ? 3600 : 1200), 0, 2400, -2400);
    }
}
LS_CASE(dmr_mode_has_both_slots_touch_hold_and_layered_steppers)
{
    static const uint8_t sync[6] = {0xDF,0xF5,0x7D,0x75,0xDF,0x5D};
    static const uint8_t cach[2][3] = {{0x80,0x02,0x02},{0x88,0x00,0x20}};
    dmr_watch_reset(); ls_shim_time_set(1000000);
    for (unsigned i = 0; i < 2; ++i) {
        dmr_lc_t lc = {.destination = i ? 16777215 : 2051, .source = 1234567,
                       .service_options = i ? 0x40 : 0};
        uint8_t b[33]; dmr_gen_burst(b, sync, 7, 1, &lc);
        dmr_ui_feed(cach[i], 3); dmr_ui_feed(b, 33);
    }
    ls_scr_p25.key(LS_TK_CHAR, '6');
    for (unsigned p = 0; p < 2; ++p) {
        fresh(); draw_pane(&ls_scr_p25, PANES[p]);
        LS_CHECK(rect_has(PANES[p], "SLOT 1"));
        LS_CHECK(rect_has(PANES[p], "SLOT 2"));
        LS_CHECK(rect_has(PANES[p], "16777215"));
        LS_CHECK(rect_has(PANES[p], "ENCRYPTED"));
        LS_EQ_INT(escaped(PANES[p]), 0);
        int x,y,w,h;
        LS_CHECK(ls_btn_rect_slot(LS_BTN_SLOT_QUICK, 2, &x,&y,&w,&h));
        LS_CHECK(ls_scr_p25.touch(x+w/2,y+h/2));
        fresh(); draw_pane(&ls_scr_p25, PANES[p]);
        LS_CHECK(rect_has(tui_rect_make(x,y,w,h), "2051"));
        ls_scr_p25.key(LS_TK_CHAR, 'h');
    }
    LS_CHECK(ls_scr_p25.key(LS_TK_CHAR, 'o'));
    LS_CHECK(ls_picker_active());
    ls_picker_key(LS_TK_ENTER, 0); ls_opt_poll();
    LS_CHECK(ls_picker_active());
    ls_picker_key(LS_TK_RIGHT, 0);
    ls_opt_close();
    fresh(); draw_pane(&ls_scr_p25, PANES[0]);
    LS_CHECK(!rect_has(PANES[0], "BOTH"));
    ls_scr_p25.key(LS_TK_CHAR, 's'); ls_scr_p25.key(LS_TK_CHAR, 's');
    ls_scr_p25.key(LS_TK_CHAR, '1');
}
LS_CASE(dmr_mode_clips_every_small_pane)
{
    ls_scr_p25.key(LS_TK_CHAR, '6');
    for (int p = 0; p < N_PANES; ++p) {
        fresh(); draw_pane(&ls_scr_p25, PANES[p]);
        LS_EQ_INT(escaped(PANES[p]), 0);
    }
    ls_scr_p25.key(LS_TK_CHAR, '1');
}

LS_CASE(spectrum_keyboard_toggle_and_page_exits_release_source)
{
    const char exits[] = {'1', '3', '6', '4', 'o', 'r'};
    for (unsigned i = 0; i < sizeof(exits); i++) {
        ls_scr_p25.key(LS_TK_CHAR, '1');
        ls_scr_p25.key(LS_TK_CHAR, '2');
        fresh(); draw_pane(&ls_scr_p25, PORTRAIT);
        LS_CHECK(!rect_has(PORTRAIT, "DECODE / SIGNAL"));
        int releases = ls_test_wf_releases;
        ls_scr_p25.key(LS_TK_CHAR, exits[i]);
        LS_EQ_INT(ls_test_wf_releases, releases + 1);
        ls_opt_close(); ls_picker_close();
    }
    ls_scr_p25.key(LS_TK_CHAR, '1');
    ls_scr_p25.key(LS_TK_CHAR, '2');
    int releases = ls_test_wf_releases;
    ls_scr_p25.key(LS_TK_CHAR, '1');
    LS_EQ_INT(ls_test_wf_releases, releases + 1);
    fresh(); draw_pane(&ls_scr_p25, PORTRAIT);
    LS_CHECK(rect_has(PORTRAIT, "DECODE / SIGNAL"));
    LS_CHECK(!strcmp(ls_wf_source_name(), "none"));
}
LS_CASE(valid_zero_nac_renders_and_old_channel_identity_is_hidden)
{
    ls_scr_p25.key(LS_TK_CHAR, '1');
    P25.dsd_nac = 0; P25.dsd_nac_valid = true;
    P25.dsd_nac_generation = P25.channel_generation = 7;
    fresh(); draw_pane(&ls_scr_p25, PORTRAIT);
    LS_CHECK(rect_has(PORTRAIT, "000"));
    P25.dsd_nac = 0x293; P25.channel_generation++;
    fresh(); draw_pane(&ls_scr_p25, PORTRAIT);
    LS_CHECK(!rect_has(PORTRAIT, "293"));
    P25.dsd_nac_valid = false;
}
LS_CASE(lr_iq_settings_omit_fsk_gain_and_refuse_phase2)
{
    ls_value_publish("p25.lrgain", "step", v_fake_lrgain);
    ls_action_register("p25.lrgain", "i", LS_CAP_TUNE, a_fake_lrgain, "test");
    P25.lora_rx = true; s_fake_lrgain = 0;
    ls_scr_p25.enter();
    ls_rsel_set(LS_RSEL_P25, LS_RSEL_LORA);
    ls_opt_close(); ls_scr_p25.key(LS_TK_CHAR, '4');
    /* Inspect the real options picker while scrolling: the screen callback
       itself draws the app underneath the overlay, not its options rows. */
    bool saw_polarity = false;
    for (unsigned row = 0; row < LS_PICKER_MAX; ++row) {
        fresh(); grid_for(PORTRAIT); ls_picker_draw(&g_sf, PORTRAIT);
        saw_polarity |= rect_has(PORTRAIT, "Polarity");
        LS_CHECK(!rect_has(PORTRAIT, "Receiver gain"));
        LS_CHECK(!rect_has(PORTRAIT, "Automatic gain"));
        LS_CHECK(!rect_has(PORTRAIT, "Demodulation"));
        ls_picker_key(LS_TK_DOWN, 0);
    }
    LS_CHECK(saw_polarity);
    /* The visible control must also dispatch its callback and remain usable
       after a toggle. Restore the simulated setting for subsequent tests. */
    ls_opt_close(); ls_scr_p25.key(LS_TK_CHAR, '4');
    filter_option("Polarity");
    bool before_polarity = lakeshark_p25_polarity_inverted();
    ls_picker_key(LS_TK_ENTER, 0);
    LS_CHECK(lakeshark_p25_polarity_inverted() != before_polarity);
    ls_picker_key(LS_TK_ENTER, 0);
    LS_CHECK(lakeshark_p25_polarity_inverted() == before_polarity);
    LS_EQ_INT(s_fake_lrgain, 0);
    ls_opt_close(); ls_scr_p25.key(LS_TK_CHAR, '4');
    filter_option("Phase II (exp)"); p25_p2_enable(false);
    ls_picker_key(LS_TK_ENTER, 0); LS_CHECK(!p25_p2_enabled());
    ls_opt_close(); P25.lora_rx = false;
    ls_rsel_set(LS_RSEL_P25, LS_RSEL_SDR_RTL);
}
LS_CASE(validated_nonzero_then_zero_nac_updates_the_production_snapshot)
{
    static p25_state_t state;
    memset(&state, 0, sizeof(state)); state.channel_generation = 1;
    p25_nac_publish(&state, 0x293, 1, 10);
    LS_CHECK(p25_nac_valid(&state)); LS_EQ_INT(state.dsd_last_ok_nac, 0x293);
    state.channel_generation = 2;
    LS_CHECK(!p25_nac_valid(&state));
    p25_nac_publish(&state, 0, 2, 20);
    LS_CHECK(p25_nac_valid(&state)); LS_EQ_INT(state.dsd_nac, 0);
    LS_EQ_INT(state.dsd_last_ok_nac, 0); LS_EQ_UINT(state.nac_seen_us, 20);
    p25_nac_publish(&state, 0x293, 1, 30);
    LS_CHECK(!p25_nac_valid(&state));
}

static void filter_option(const char *name)
{
    for (const char *p = name; *p; p++) ls_picker_key(LS_TK_CHAR, *p);
}
LS_CASE(options_numeric_levels_step_without_opening_the_keypad)
{
    ls_opt_close(); ls_scr_p25.key(LS_TK_CHAR, '1');
    ls_scr_p25.key(LS_TK_CHAR, 'o'); filter_option("Voice gate");
    int before = lakeshark_p25_voice_gate();
    ls_picker_key(LS_TK_RIGHT, 0);
    LS_EQ_INT(lakeshark_p25_voice_gate(), before + 1);
    LS_CHECK(ls_picker_active());
    ls_picker_key(LS_TK_LEFT, 0); LS_EQ_INT(lakeshark_p25_voice_gate(), before);
    ls_opt_close();
}
LS_CASE(options_reach_p25_policy_and_survey_controls)
{
    ls_scr_p25.key(LS_TK_CHAR, '1'); P25.dsd_tg = 2051; P25.hold_tg = 0;
    ls_scr_p25.key(LS_TK_CHAR, 'o'); filter_option("Talkgroup policy");
    ls_picker_key(LS_TK_ENTER, 0); filter_option("Hold current");
    ls_picker_key(LS_TK_ENTER, 0); LS_EQ_INT(P25.hold_tg, 2051);
    ls_opt_close();
    ls_scr_p25.key(LS_TK_CHAR, 'o'); filter_option("Control survey");
    bool before = p25_program_survey_active_now();
    ls_picker_key(LS_TK_ENTER, 0);
    LS_CHECK(p25_program_survey_active_now() != before);
    ls_picker_key(LS_TK_ENTER, 0);
    LS_EQ_INT(p25_program_survey_active_now(), before);
    ls_opt_close(); P25.hold_tg = 0; P25.dsd_tg = 0;
}
LS_CASE(trunking_failure_and_unresolved_grants_are_visible)
{
    ls_scr_p25.key(LS_TK_CHAR, '1');
    P25.receive_state = P25_RX_TUNE_FAILED;
    fresh(); draw_pane(&ls_scr_p25, PORTRAIT);
    LS_CHECK(rect_has(PORTRAIT, "TUNE FAILED"));
    P25.receive_state = P25_RX_CONTROL_LOCKED;
    P25.grant_on_traffic = false;
    P25.grant_observed.support = P25_CALL_MISSING_IDEN;
    fresh(); draw_pane(&ls_scr_p25, PORTRAIT);
    LS_CHECK(rect_has(PORTRAIT, "UNRESOLVED GRANT"));
    P25.grant_observed.support = P25_CALL_UNSUPPORTED;
    fresh(); draw_pane(&ls_scr_p25, PORTRAIT);
    LS_CHECK(rect_has(PORTRAIT, "UNSUPPORTED GRANT"));
    P25.grant_observed.support = P25_CALL_NONE;
    P25.receive_state = P25_RX_CONTROL_SEARCH;
}

LS_CASE(lr_iq_options_do_not_offer_a_retired_receive_path)
{
    P25.lr_iq_requested = true;
    ls_opt_close(); ls_scr_p25.key(LS_TK_CHAR, '1');
    ls_scr_p25.key(LS_TK_CHAR, 'o');
    fresh(); draw_pane(&ls_scr_p25, PORTRAIT);
    LS_CHECK(!rect_has(PORTRAIT, "LR2021 receive path"));
    LS_CHECK(P25.lr_iq_requested);
    ls_opt_close();
    P25.lr_iq_active = true; P25.lr_iq_good = 3; P25.lr_iq_bad = 2;
    fresh(); draw_pane(&ls_scr_p25, PORTRAIT);
    LS_CHECK(rect_has(PORTRAIT, "IQ blocks 3"));
    LS_CHECK(rect_has(PORTRAIT, "I/Q"));
    P25.lr_iq_active = false;
}

LS_CASE(lr_signal_page_draws_iq_and_explains_acquisition)
{
    show_signal_page(); pin_grain_to_shade(); s_shape = 1;
    tui_rect pane = {1, 2, 46, 40};
    P25.lora_rx = true; P25.lr_iq_active = false;
    fresh(); draw_pane(&ls_scr_p25, pane);
    LS_CHECK(rect_has(pane, "LR IQ acquiring"));
    P25.lr_iq_active = true;
    prime(pane, 60);
    LS_CHECK(!rect_has(pane, "LR IQ acquiring"));
    LS_CHECK(history_rows(pane) > 0);
    P25.lr_iq_active = P25.lora_rx = false;
}

LS_CASE(lr_iq_options_open_scroll_and_idle_are_read_only_and_bounded)
{
    extern void lssim_rsel_board(const ls_rsel_hw_t *);
    extern int lssim_rsel_writes;
    ls_rsel_hw_t hw = {0};
    hw.present[LS_RSEL_LORA] = true;
    hw.lora_lr20xx = true; hw.lora_name = "LR2021";
    hw.lora_caps = LS_LORA_CAP_LORA | LS_LORA_CAP_FSK_STREAM;
    lssim_rsel_board(&hw);
    ls_opt_close(); ls_scr_p25.key(LS_TK_CHAR, '1');
    P25.lr_iq_active = P25.lr_iq_requested = P25.lora_rx = true;
    int screen = ls_tui_screen_register(&ls_scr_p25);
    ls_tui_screen_show(screen);
    ls_shim_time_set(100000000);
    option_locks = option_spi = option_nvs = option_actions = option_reads = 0;
    const int writes = lssim_rsel_writes;
    LS_CHECK(ls_tui_router_key(LS_TK_CHAR, 'o'));
    LS_CHECK(ls_picker_active());
    const unsigned open_reads = option_reads;
    /* This tripwire counts the Demodulation getter specifically. Its row is
       absent for LR, so opening its options must not read that preference. */
    LS_EQ_INT(0, open_reads);
    for (int orientation = 0; orientation < 2; orientation++) {
        s_grid_cols = orientation ? 115 : 48;
        s_grid_rows = orientation ? 27 : 66;
        for (int frame = 0; frame < 100; frame++) {
            fresh();
            ls_tui_router_key(frame < 50 ? LS_TK_DOWN : LS_TK_UP, 0);
            ls_tui_router_draw(&g_sf);
            ls_shim_time_advance(10000);
        }
    }
    LS_CHECK(option_reads <= open_reads + 8); /* at most four refreshes/second */
    LS_EQ_INT(0, option_locks); LS_EQ_INT(0, option_spi);
    LS_EQ_INT(0, option_nvs); LS_EQ_INT(0, option_actions);
    LS_EQ_INT(writes, lssim_rsel_writes);
    LS_CHECK(P25.lr_iq_active && P25.lr_iq_requested);
    ls_opt_close(); P25.lr_iq_active = P25.lora_rx = false;
    hw.present[LS_RSEL_SDR_RTL] = true;
    lssim_rsel_board(&hw);
}

#include "switch_budget.h"
#include "ls_calls.h"
LS_CASE(p25_enter_leave_has_zero_allocation_budget)
{
    ls_calls_leave(); ls_opt_close();
    ls_scr_p25.key(LS_TK_CHAR, '1'); pin_grain_to_shade();
    option_locks = option_spi = option_nvs = option_actions = 0;
    for (int visit = 0; visit < 4; ++visit) {
        switch_budget_begin();
        ls_scr_p25.enter();
        fresh(); draw_pane(&ls_scr_p25, PORTRAIT);
        ls_scr_p25.leave();
        LS_EQ_UINT(switch_budget_end(), 0); /* N = 0 bytes */
        LS_EQ_UINT(option_locks + option_spi + option_nvs + option_actions, 0);
    }
}

void ls_tui_last_cost(uint32_t *us, int *cells)
{ if (us) *us = 0; if (cells) *cells = 0; }

LS_CASE(shared_glass_retunes_settle_and_do_not_restart_on_idle_frames)
{
    ls_radio_glass_t a = {0}, b = {0};
    LS_EQ_UINT(ls_radio_glass_tick(&a, 851000000), 851000000);
    for (int i = 0; i < 20; ++i) ls_radio_glass_tick(&a, 851000000);
    LS_EQ_INT(a.dial_frames, 0);
    uint32_t first = ls_radio_glass_tick(&a, 852000000);
    LS_CHECK(first > 851000000 && first < 852000000);
    LS_EQ_UINT(ls_radio_glass_tick(&b, 154000000), 154000000);
    for (int i = 0; i < 10; ++i) ls_radio_glass_tick(&a, 852000000);
    LS_EQ_UINT(a.dial_hz, 852000000);
    LS_EQ_INT(a.dial_frames, 0);
    LS_EQ_UINT(b.dial_hz, 154000000);
    LS_CHECK(ls_radio_glass_tick(&a, 850000000) < 852000000);
}

LS_CASE(shared_glass_entry_has_ghosts_and_caret_and_does_not_retune)
{
    ls_radio_glass_t g = {0};
    const tui_rect r = {1, 2, 62, 14};
    fresh();
    ls_radio_glass_bezel(&g_sf, r, "ENTER FREQUENCY");
    ls_radio_glass_frequency(&g, &g_sf, r, r.y + 2, 851000000, "154.", 0, false);
    LS_CHECK(rect_has(r, "ENTER FREQUENCY"));
    int ghosts = 0, carets = 0;
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x) {
            ghosts += g_back[y * W + x].ch == LS_TUI_SHADE_25;
            carets += g_back[y * W + x].ch == LS_TUI_SHADE_75;
        }
    LS_CHECK(ghosts > 20);
    LS_EQ_INT(carets, 0);
    fresh();
    ls_radio_glass_frequency(&g, &g_sf, r, r.y + 2, 851000000, "154.", 8, false);
    carets = 0;
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x)
            carets += g_back[y * W + x].ch == LS_TUI_SHADE_75;
    LS_CHECK(carets >= 4);
    LS_EQ_UINT(g.dial_target, 851000000);
    LS_EQ_INT(escaped(r), 0);
}

LS_CASE(shared_glass_trace_wraps_and_meter_holds_without_a_sql_gate)
{
    ls_radio_glass_t g = {0};
    for (int i = 0; i < LS_RADIO_TRACE_N + 17; ++i)
        ls_radio_glass_sample(&g, i & 1 ? 0.5f : 1.0f);
    LS_EQ_INT(g.trace_n, LS_RADIO_TRACE_N);
    const tui_rect r = {1, 2, 62, 14};
    fresh();
    ls_radio_glass_trace(&g, &g_sf, r, 3, 4, 50, 3, 0.0f);
    ls_radio_glass_meter(&g, &g_sf, r, 3, 10, 54, 1, 0.8f, true, false, false, 0, NULL);
    LS_NEAR(g.hold, 0.8f, 0.001f);
    ls_radio_glass_meter(&g, &g_sf, r, 3, 10, 54, 1, 0.2f, true, false, false, 0, NULL);
    LS_CHECK(g.hold > 0.7f && g.hold < 0.8f);
    LS_CHECK(!rect_has(r, "SQL"));
    LS_EQ_INT(escaped(r), 0);
}

LS_CASE(shared_glass_mode_wipe_starts_only_on_a_mode_change_and_stops)
{
    ls_radio_glass_t g = {0};
    const tui_rect r = {1, 2, 62, 14};
    fresh();
    ls_radio_glass_wipe(&g, &g_sf, r, 4, 10, 0);
    LS_EQ_INT(g.wipe_frames, 0);
    ls_radio_glass_wipe(&g, &g_sf, r, 4, 10, 1);
    LS_EQ_INT(g.wipe_frames, 11);
    for (int i = 0; i < 20; ++i) ls_radio_glass_wipe(&g, &g_sf, r, 4, 10, 1);
    LS_EQ_INT(g.wipe_frames, 0);
    ls_radio_glass_wipe(&g, &g_sf, r, 4, 10, 0);
    LS_EQ_INT(g.wipe_frames, 11);
    LS_EQ_INT(escaped(r), 0);
}

LS_CASE(p25_lr_volume_follows_health_without_a_value_box)
{
    no_traffic();
    P25.lora_rx = true;
    P25.lr_iq_active = true;
    const tui_rect pane = {1, 2, 50, 68};
    draw_decode_now(pane);
    const int title = rect_row_of(pane, "VOLUME");
    const int health = rect_row_of(pane, "RECEIVER HEALTH");
    LS_CHECK(health >= 0 && title == health + 8);
    int x, y, w, h;
    LS_CHECK(ls_scr_p25_control_rect(0, LS_RADIO_PART_VALUE, &x, &y, &w, &h));
    LS_EQ_INT(y, title);
    LS_EQ_INT(h, 1);
    LS_CHECK(!ls_scr_p25_control_rect(1, LS_RADIO_PART_UP, &x, &y, &w, &h));
    LS_CHECK(ls_scr_p25_control_rect(0, LS_RADIO_PART_BAR, &x, &y, &w, &h));
    LS_CHECK(w >= 20);
    LS_CHECK(!rect_has(pane, "GAIN ["));
}

LS_CASE(p25_glass_and_thumb_keys_work_in_portrait_and_landscape)
{
    const tui_rect panes[] = {{1, 2, 54, 65}, {1, 2, 115, 27}};
    for (int posture = 0; posture < 2; ++posture) {
        no_traffic();
        P25.lora_rx = false;
        draw_decode_now(panes[posture]);
        for (int card = 0; card < 2; ++card) {
            for (int part = LS_RADIO_PART_DOWN; part <= LS_RADIO_PART_VALUE; ++part) {
                int x, y, w, h;
                LS_CHECK(ls_scr_p25_control_rect(card, part, &x, &y, &w, &h));
                LS_CHECK(w >= (part == LS_RADIO_PART_VALUE ? 3 : 8) &&
                         h >= (part == LS_RADIO_PART_VALUE ? 1 : 3));
            }
        }
        LS_CHECK(press_text(panes[posture], "KEYBOARD"));
        redraw(panes[posture]);
        for (const char *p = "154."; *p; ++p) LS_CHECK(ls_scr_p25.key(LS_TK_CHAR, *p));
        redraw(panes[posture]);
        LS_CHECK(rect_has(panes[posture], "ENTER FREQUENCY"));
        LS_CHECK(rect_has(panes[posture], "UNIT"));
        LS_CHECK(rect_has(panes[posture], "MOD"));
        LS_CHECK(rect_has(panes[posture], "AUDIO"));
        const int top = rect_row_of(panes[posture], "ENTER FREQUENCY");
        const int bottom = rect_row_of(panes[posture], "FREQUENCY / MHz");
        int ghosts = 0;
        for (int y = top; y < bottom; ++y)
            for (int x = panes[posture].x; x < panes[posture].x + panes[posture].w; ++x)
                ghosts += g_back[y * W + x].ch == LS_TUI_SHADE_25;
        LS_CHECK_MSG(ghosts > 20, "posture %d has no ghost digits in glass", posture);
        for (const char *p = "785000"; *p; ++p) LS_CHECK(ls_scr_p25.key(LS_TK_CHAR, *p));
        redraw(panes[posture]);
        LS_CHECK(rect_has(panes[posture], "154.785000"));
        LS_EQ_UINT(s_tune_freq_hz, TUNED_HZ);
        LS_CHECK(ls_scr_p25.key(LS_TK_ESC, 0));
    }
}

LS_CASE(shared_fm_p25_lcd_ten_character_entry_has_only_its_drawn_digits)
{
    /* MinGW has no ASan runtime: count the actual glyph cells instead.
       1,5,4,7,8,5,0,0,0 contain 10+14+10+8+16+14+14+14+14
       lit cells, plus two for the decimal point, at horizontal scale one.
       Exclude the fourth glyph row, which also carries the MHz caption. */
    const tui_rect r = {1, 2, 62, 14};
    ls_radio_glass_t g = {0};
    fresh();
    ls_radio_glass_frequency(&g, &g_sf, r, r.y + 2, 851000000,
                             "154.785000", 0, false);
    int lit = 0, ghosts = 0;
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x) {
            lit += y != r.y + 6 && g_back[y * W + x].ch == LS_TUI_SHADE_FULL;
            ghosts += g_back[y * W + x].ch == LS_TUI_SHADE_25;
        }
    LS_EQ_INT(lit, 102);
    LS_EQ_INT(ghosts, 0);
    LS_EQ_INT(escaped(r), 0);
}

LS_CASE(shared_vfo_full_value_and_every_bar_cell_are_exact_targets)
{
    const int widths[] = {24, 28, 32, 34, 50, 54, 62};
    const char *values[] = {"100%", "28.0", "-49.6", "12%"};
    for (unsigned w = 0; w < sizeof(widths) / sizeof(widths[0]); ++w)
        for (unsigned v = 0; v < sizeof(values) / sizeof(values[0]); ++v) {
            const tui_rect box = {1, 2, widths[w], 6};
            ls_radio_card_t c = {.label = "VOLUME", .value = values[v],
                                 .live = true, .hue = TUI_CYAN, .frac = 0.5f};
            fresh(); ls_radio_vfo_card_draw(&g_sf, box, &c);
            LS_EQ_INT(c.value_rect.x, box.x + 12);
            LS_EQ_INT(c.value_rect.y, box.y + 1);
            LS_EQ_INT(c.value_rect.w, strlen(values[v]) + 2);
            LS_EQ_INT(c.value_rect.h, 1);
            LS_CHECK(rect_has(c.value_rect, values[v]));
            const tui_rect targets[] = {c.value_rect, c.bar, c.down, c.up};
            const int expected[] = {LS_RADIO_PART_VALUE, LS_RADIO_PART_BAR,
                                    LS_RADIO_PART_DOWN, LS_RADIO_PART_UP};
            for (int t = 0; t < 4; ++t)
                for (int y = targets[t].y; y < targets[t].y + targets[t].h; ++y)
                    for (int x = targets[t].x; x < targets[t].x + targets[t].w; ++x)
                        LS_EQ_INT(ls_radio_vfo_card_hit(c.down, c.up, c.value_rect,
                                                       c.bar, x, y), expected[t]);
            /* Explicit overlapping-target regression: step keys win. */
            LS_EQ_INT(ls_radio_vfo_card_hit(c.down, c.up, c.value_rect, box,
                                           c.down.x, c.down.y), LS_RADIO_PART_DOWN);
            LS_EQ_INT(ls_radio_vfo_card_hit(c.down, c.up, c.value_rect, box,
                                           c.up.x, c.up.y), LS_RADIO_PART_UP);
            LS_EQ_INT(escaped(box), 0);
        }
}

/* Cached chip identity for the shared VFO receive-range guard. */
ls_lora_chip_t ls_lora_chip(void) { return P25.lr_iq_active ? LS_LORA_CHIP_LR20XX : LS_LORA_CHIP_SX126X; }
#include "settings.h"
#include "scan_engine.h"
#include <math.h>
static unsigned tune_next_calls, tune_skip_calls, tune_hold_calls, tune_stop_calls;
void __real_scan_engine_next(void);
void __real_scan_engine_skip(void);
void __real_scan_engine_hold(bool);
void __real_scan_engine_stop(void);
void __wrap_scan_engine_next(void) { tune_next_calls++; __real_scan_engine_next(); }
void __wrap_scan_engine_skip(void) { tune_skip_calls++; __real_scan_engine_skip(); }
void __wrap_scan_engine_hold(bool on) { tune_hold_calls++; __real_scan_engine_hold(on); }
void __wrap_scan_engine_stop(void) { tune_stop_calls++; __real_scan_engine_stop(); }

LS_CASE(p25_tune_every_painted_key_cell_dispatches_idle_and_scan_actions)
{
    const tui_rect panes[] = {{0, 6, 52, 64}, {0, 6, 52, 61},
                              {0, 2, 118, 28}, {0, 2, 115, 27}};
    ls_action_register("p25.freq", "f", LS_CAP_TUNE, a_deck_freq, "test raster");
    for (unsigned p = 0; p < sizeof(panes)/sizeof(panes[0]); ++p) {
        no_traffic(); P25.lora_rx = P25.lr_iq_active = false;
        scan_engine_stop(); settings_set_p25_step(0);
        draw_decode_now(panes[p]);
        const int title = rect_row_of(panes[p], "TUNE  ");
        LS_CHECK(title >= 0);
        if (title < 0) continue;
        tui_rect keys[5]; int nk = 0;
        for (int x = panes[p].x; x < panes[p].x + panes[p].w && nk < 5;) {
            if (g_back[(title+1)*W+x].ch != '=') { x++; continue; }
            int start = x;
            while (x < panes[p].x + panes[p].w && g_back[(title+1)*W+x].ch == '=') x++;
            keys[nk++] = tui_rect_make(start, title+1, x-start, p < 2 ? 5 : 3);
        }
        LS_EQ_INT(nk, 5);
        if (nk != 5) continue;
        for (int active = 0; active < 2; ++active)
            for (int key = 0; key < 5; ++key) {
                LS_CHECK(keys[key].w >= 8 && keys[key].h >= 3);
                for (int y = keys[key].y; y < keys[key].y + keys[key].h; ++y)
                    for (int x = keys[key].x; x < keys[key].x + keys[key].w; ++x) {
                        scan_engine_stop();
                        settings_set_p25_step(12500); s_tune_freq_hz = 150000000;
                        if (active) scan_engine_start();
                        tune_next_calls = tune_skip_calls = tune_hold_calls = tune_stop_calls = 0;
                        s_tuned_mhz = -1;
                        LS_CHECK(ls_scr_p25.touch(x, y));
                        if (key == 0) LS_EQ_UINT(settings_get_p25_step(), 25000);
                        else if (active) {
                            LS_EQ_UINT(tune_next_calls, key == 1);
                            LS_EQ_UINT(tune_skip_calls, key == 2);
                            LS_EQ_UINT(tune_hold_calls, key == 3);
                            LS_EQ_UINT(tune_stop_calls, key == 4);
                            if (key == 3) LS_CHECK(scan_engine_manual_hold());
                            if (key == 4) LS_CHECK(!scan_engine_active());
                        } else if (key == 1 || key == 4) {
                            LS_CHECK(scan_engine_active());
                            LS_EQ_INT(scan_engine_seek_direction(), key == 1 ? -1 : 1);
                        } else LS_CHECK(fabs(s_tuned_mhz - (key == 2 ? 149.9875 : 150.0125)) < 0.00002);
                    }
            }
    }
    scan_engine_stop(); settings_set_p25_step(0);
}

LS_CASE(p25_tune_keyboard_raster_cycle_scan_controls_and_lr_clamp)
{
    ls_action_register("p25.freq", "f", LS_CAP_TUNE, a_deck_freq, "test raster");
    no_traffic(); P25.lora_rx = P25.lr_iq_active = false;
    const tui_rect pane = {0, 6, 52, 64};
    draw_decode_now(pane); scan_engine_stop(); settings_set_p25_step(0);
    const uint32_t choices[] = {25000,100000,1000000,5000,6250,10000,12500};
    for (unsigned i = 0; i < 7; ++i) {
        LS_CHECK(ls_scr_p25.key(LS_TK_CHAR, '/'));
        LS_EQ_UINT(settings_get_p25_step(), choices[i]);
    }
    s_tune_freq_hz = 150000000;
    LS_CHECK(ls_scr_p25.key(LS_TK_LEFT, 0));
    LS_CHECK(fabs(s_tuned_mhz - 149.9875) < 0.00002);
    LS_CHECK(ls_scr_p25.key(LS_TK_RIGHT, 0));
    LS_CHECK(fabs(s_tuned_mhz - 150.0125) < 0.00002);
    for (int d = 0; d < 2; ++d) {
        scan_engine_stop();
        LS_CHECK(ls_scr_p25.key(LS_TK_CHAR, d ? '.' : ','));
        LS_CHECK(scan_engine_active());
        LS_EQ_INT(scan_engine_seek_direction(), d ? 1 : -1);
        LS_CHECK(ls_scr_p25.key(LS_TK_CHAR, 'H')); LS_CHECK(scan_engine_manual_hold());
        LS_CHECK(ls_scr_p25.key(LS_TK_CHAR, 'H')); LS_CHECK(!scan_engine_manual_hold());
        LS_CHECK(ls_scr_p25.key(LS_TK_CHAR, 'K'));
        LS_CHECK(ls_scr_p25.key(LS_TK_CHAR, 'N'));
        LS_CHECK(ls_scr_p25.key(LS_TK_CHAR, 'X')); LS_CHECK(!scan_engine_active());
    }
    P25.lora_rx = P25.lr_iq_active = true; s_tune_freq_hz = 104000000;
    LS_CHECK(ls_scr_p25.key(LS_TK_LEFT, 0)); LS_CHECK(fabs(s_tuned_mhz - 104.0) < 0.00002);
    P25.lora_rx = P25.lr_iq_active = false; s_tune_freq_hz = 24000000;
    LS_CHECK(ls_scr_p25.key(LS_TK_LEFT, 0)); LS_CHECK(fabs(s_tuned_mhz - 24.0) < 0.00002);
    s_tune_freq_hz = 1766000000;
    LS_CHECK(ls_scr_p25.key(LS_TK_RIGHT, 0)); LS_CHECK(fabs(s_tuned_mhz - 1766.0) < 0.00002);
    settings_set_p25_step(0);
}

/* Real display panes: card and TUNE thumb geometry, on both receive paths. */
LS_CASE(p25_vfo_cards_match_fm_height_on_all_four_panes_and_both_radios)
{
    const tui_rect panes[] = {{0, 6, 52, 64}, {0, 6, 52, 61},
                              {0, 2, 118, 28}, {0, 2, 115, 27}};
    for (int radio = 0; radio < 2; ++radio)
        for (int p = 0; p < 4; ++p)
            for (int scanning = 0; scanning < 2; ++scanning) {
                no_traffic(); P25.lora_rx = P25.lr_iq_active = radio != 0;
                scan_engine_stop();
                if (scanning) scan_engine_start();
                draw_decode_now(panes[p]);
                const int height = p == 2 ? 9 : 8;
                for (int card = 0; card < (radio ? 1 : 2); ++card) {
                    const int parts[] = {LS_RADIO_PART_DOWN, LS_RADIO_PART_UP,
                                         LS_RADIO_PART_BAR};
                    for (int part = 0; part < 3; ++part) {
                        int x, y, w, h;
                        LS_CHECK(ls_scr_p25_control_rect(card, parts[part], &x, &y, &w, &h));
                        LS_CHECK(w >= (part == 2 ? 1 : 8) && h >= 3);
                        LS_EQ_INT(h, height - 3);
                        LS_CHECK(x >= panes[p].x && y >= panes[p].y &&
                                 x + w <= panes[p].x + panes[p].w &&
                                 y + h <= panes[p].y + panes[p].h);
                    }
                }
                const int title = rect_row_of(panes[p], scanning ? "SCAN  " : "TUNE  ");
                LS_CHECK(title >= 0);
                if (title < 0) continue;
                int keys = 0;
                for (int x = panes[p].x; x < panes[p].x + panes[p].w;) {
                    if (g_back[(title + 1) * W + x].ch != '=') { x++; continue; }
                    int start = x;
                    while (x < panes[p].x + panes[p].w &&
                           g_back[(title + 1) * W + x].ch == '=') x++;
                    LS_CHECK(x - start >= 8);
                    const int h = p < 2 ? 5 : 3;
                    for (int col = start; col < x; ++col)
                        LS_EQ_INT(g_back[(title + h) * W + col].ch, '=');
                    keys++;
                }
                LS_EQ_INT(keys, 5);
            }
    scan_engine_stop(); P25.lora_rx = P25.lr_iq_active = false;
}

LS_CASE(p25_portrait_full_activity_keeps_all_receiver_health_fields)
{
    const tui_rect panes[] = {{0, 6, 52, 64}, {0, 6, 52, 61}};
    for (int radio = 0; radio < 2; ++radio)
        for (int p = 0; p < 2; ++p) {
            no_traffic(); P25.lora_rx = P25.lr_iq_active = radio != 0;
            scan_engine_stop();
            for (int i = 0; i < 4; ++i)
                LS_CHECK(p25_tg_observed_record(TUNED_HZ, 0x293, 1000 + i,
                         P25_TG_SEEN_LCW, 1000 + i));
            draw_decode_now(panes[p]);
            const char *fields[] = {"ACTIVITY", "TALKGROUP", "RECEIVER HEALTH",
                                    "BCH OK", "AUDIO drops", "DECODE 0.0", "VOLUME"};
            for (unsigned i = 0; i < sizeof(fields)/sizeof(fields[0]); ++i)
                LS_CHECK(rect_has(panes[p], fields[i]));
        }
    no_traffic(); P25.lora_rx = P25.lr_iq_active = false;
}
