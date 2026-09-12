/* LS_TEST_SOURCES: the whole TUI stack with only the panel stubbed */

#include "ls_test.h"
#include "ls_tui.h"
#include "ls_tui_screen.h"
#include "ls_tui_chrome.h"
#include "ls_font.h"
#include "ls_panel.h"
#include "ls_theme.h"
#include "p25_state.h"
#include "apps/p25/p25_spectrum.h"
#include "apps/rec/rec_state.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------- panel stub -- */

#define NATIVE_W 568
#define NATIVE_H 1232
static uint16_t g_fb[NATIVE_W * NATIVE_H];

bool ls_panel_fb(ls_panel_fb_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->pixels = g_fb;
    out->width = NATIVE_W;
    out->height = NATIVE_H;
    return true;
}
void ls_panel_fb_present(void) { }

/* ---------------------------------------------------------------- fakes -- */

static rec_hub_status_t s_rec = {
    .phase = REC_CAPTURING,
    .freq_hz = 433920000u,
    .edges = 47,
    .mag_now = 900,
    .mag_thresh = 600,
    .bytes_sec = 48000,
    .captures = 3,
    .receiver_streaming = true,
};
static bool s_rec_armed = true;

void rec_get_hub_status(rec_hub_status_t *out) { if (out) *out = s_rec; }
uint64_t rec_dir_free_bytes(void) { return 1500ull * 1024 * 1024; }
const char *rec_dir(void) { return "/sdcard/rec"; }
bool rec_active(void) { return s_rec_armed; }
void rec_arm(void) { s_rec_armed = true; }
void rec_arm_request(void) { s_rec_armed = true; }
void rec_disarm(void) { s_rec_armed = false; }

p25_state_t P25;
scan_state_t SCAN;
uint32_t s_tune_freq_hz;

void p25_get_receiver_status(ls_iq_control_status_t *o)
{ if (o) memset(o, 0, sizeof(*o)); }
void p25_spectrum_enable(bool on) { (void)on; }
bool p25_spectrum_enabled(void) { return true; }
void p25_spectrum_init(void) { }
void p25_spectrum_invalidate(void) { }
bool p25_spectrum_tap_hz(int x, int w, uint32_t c, uint32_t s, uint32_t lo,
                         uint32_t hi, uint32_t *out)
{ (void)x;(void)w;(void)c;(void)s;(void)lo;(void)hi; if (out) *out = 851000000u; return true; }

static int s_phase;
bool p25_spectrum_read(float *out, int n, uint32_t now, uint32_t age,
                       p25_spectrum_snapshot_t *snap)
{
    (void)now; (void)age;
    if (snap) {
        memset(snap, 0, sizeof(*snap));
        snap->center_hz = 851012500u;
        snap->span_hz = 240000u;
        snap->fft_bins = (uint16_t)n;
    }
    for (int i = 0; i < n; i++) {
        float v = 0.10f + 0.05f * (float)((i * 7 + s_phase) % 5) / 5.0f;
        /* Three carriers, one drifting. */
        int c0 = n / 5, c1 = n / 2 + (s_phase % 7) - 3, c2 = 4 * n / 5;
        int d0 = i - c0, d1 = i - c1, d2 = i - c2;
        if (d0 > -3 && d0 < 3) v += 0.75f - 0.2f * (float)(d0 < 0 ? -d0 : d0);
        if (d1 > -2 && d1 < 2) v += 0.85f - 0.3f * (float)(d1 < 0 ? -d1 : d1);
        if (d2 > -4 && d2 < 4) v += 0.55f - 0.1f * (float)(d2 < 0 ? -d2 : d2);
        out[i] = v > 1.0f ? 1.0f : v;
    }
    s_phase++;
    return true;
}

uint32_t perf_get_crc_good(void)     { return 48213; }
uint32_t perf_get_crc_err(void)      { return 91; }
uint32_t perf_get_msgs_per_sec(void) { return 37; }

bool radio_health_get(const char *id, void *out)
{
    (void)id; (void)out;
    return false;                       /* no dongle attached */
}
const char *radio_health_state_name(int s) { (void)s; return "absent"; }

static int s_bright = 60, s_vol = 40, s_dimt = 60, s_boot = 1;
static bool s_autodim = true, s_usb;
int  settings_get_brightness(void) { return s_bright; }
void settings_set_brightness(int v) { s_bright = v; }
bool settings_get_autodim(void) { return s_autodim; }
void settings_set_autodim(bool v) { s_autodim = v; }
int  settings_get_autodim_timeout(void) { return s_dimt; }
void settings_set_autodim_timeout(int v) { s_dimt = v; }
/* SET goes through display_ctl now; on the bench it is the same fake. */
int  display_ctl_get_user(void) { return s_bright; }
void display_ctl_set_user(int v) { s_bright = v; }
bool display_ctl_autodim_enabled(void) { return s_autodim; }
void display_ctl_set_autodim(bool v) { s_autodim = v; }
int  display_ctl_autodim_timeout(void) { return s_dimt; }
void display_ctl_set_autodim_timeout(int v) { s_dimt = v; }
int  settings_get_volume(void) { return s_vol; }
void settings_set_volume(int v) { s_vol = v; }
int  settings_get_boot_sound(void) { return s_boot; }
void settings_set_boot_sound(int v) { s_boot = v; }
bool settings_get_usb_autoreboot(void) { return s_usb; }
void settings_set_usb_autoreboot(bool v) { s_usb = v; }
/* Both default on, the way the firmware's do. */
static bool s_alert_ring = true, s_alert_vibe = true;
static bool s_ant_ext;
bool settings_get_antenna_external(void) { return s_ant_ext; }
void settings_set_antenna_external(bool v) { s_ant_ext = v; }
bool settings_get_alert_ring(void) { return s_alert_ring; }
void settings_set_alert_ring(bool v) { s_alert_ring = v; }
bool settings_get_alert_vibe(void) { return s_alert_vibe; }
void settings_set_alert_vibe(bool v) { s_alert_vibe = v; }
int  settings_get_theme(void) { return 0; }
void settings_set_theme(int v) { (void)v; }
/* SET's Daylight row stores as well as applies. */
static bool s_daylight_stored;
bool settings_get_daylight(void) { return s_daylight_stored; }
void settings_set_daylight(bool v) { s_daylight_stored = v; }

/* ------------------------------------------------------------- helpers -- */

extern const ls_tui_screen_t ls_scr_p25, ls_scr_home, ls_scr_diag,
                             ls_scr_settings, ls_scr_rec,
                             ls_scr_fm, ls_scr_adsb;

static void register_once(void)
{
    if (ls_tui_screen_count()) return;
    ls_tui_screen_register(&ls_scr_home);
    ls_tui_screen_register(&ls_scr_p25);
    ls_tui_screen_register(&ls_scr_settings);
    /* DEMO is gone: it drew a synthetic band from a local noise model, and
       the branch that owns the app model replaced it with the shared
       waterfall, which draws whatever receiver is running. REC takes its
       place here because the point of this case is four different layouts
       rendered for real, and REC is a different shape from the other three. */
    ls_tui_screen_register(&ls_scr_rec);
}

/* Draw the router's whole frame - chrome and the active screen - and leave it
   in the grid for ls_tui_dump. */
static void frame(int times)
{
    tui_surface *sf = ls_tui_surface();
    for (int i = 0; i < times; i++) {
        ls_tui_router_draw(sf);
        ls_tui_present();
    }
}

/* Read one row of the grid back as text, so an assertion can say what it saw. */
static void row_text(int row, char *out, size_t len)
{
    tui_surface *sf = ls_tui_surface();
    int cols, rows;
    ls_tui_geometry(&cols, &rows, NULL, NULL);
    size_t n = 0;
    for (int x = 0; x < cols && n + 1 < len; x++) {
        char c = sf->back[(size_t)row * cols + x].ch;
        out[n++] = (c >= 0x20 && c < 0x7F) ? c : ' ';
    }
    out[n] = 0;
    (void)rows;
}

static bool row_has(int row, const char *needle)
{
    char buf[200];
    row_text(row, buf, sizeof(buf));
    return strstr(buf, needle) != NULL;
}

/* ---------------------------------------------------------------- cases -- */

LS_CASE(landscape_renders_a_screen_worth_looking_at)
{
    LS_CHECK(ls_tui_begin(1232, 568));
    register_once();
    ls_tui_status_set("851.0125 MHz", "KBD Terminal Bay");
    ls_tui_screen_show(1);              /* P25 */
    ls_scr_p25.key(LS_TK_RIGHT, 0);
    ls_scr_p25.key(LS_TK_RIGHT, 0);     /* the spectrum page */
    frame(40);

    printf("\n===== LANDSCAPE, P25 SIGNAL =====\n");
    ls_tui_dump();

    /* Row 0 is the status bar, row 1 the tab strip, the last the hints. */
    LS_CHECK_MSG(row_has(0, "LAKESHARK"), "no wordmark on the status row");
    LS_CHECK_MSG(row_has(0, "KBD"), "the right status is missing");
    LS_CHECK_MSG(row_has(1, "P25"), "the tab strip does not name P25");
    LS_CHECK_MSG(row_has(1, "HOME"), "the tab strip does not name HOME");

    int cols, rows;
    ls_tui_geometry(&cols, &rows, NULL, NULL);
    LS_CHECK_MSG(row_has(rows - 1, "F10"), "no key hints on the last row");

    /* Landscape reaches the two top corners with the status row and
       the two bottom ones with the hint row. Both keep their words out of
       the corner cells - and on this panel there are corner cells to keep
       out of, because a pad of zero would let this case prove nothing. */
    const int worded[2] = { 0, rows - 1 };
    for (int i = 0; i < 2; i++) {
        const int r = worded[i], pad = ls_tui_corner_pad(r);
        LS_CHECK_MSG(pad > 0,
                     "row %d has no corner padding on a rounded panel", r);
        char t[200];
        row_text(r, t, sizeof(t));
        for (int x = 0; x < pad; x++)
            LS_CHECK_MSG(t[x] == ' ' && t[cols - 1 - x] == ' ',
                         "a word in corner cell %d of row %d: '%s'", x, r, t);
    }
    ls_tui_end();
}

LS_CASE(portrait_renders_without_the_chrome_colliding)
{

    LS_CHECK(ls_tui_begin(568, 1232));
    register_once();
    ls_tui_status_set("851.0125 MHz", "KBD Terminal Bay");
    ls_tui_screen_show(1);
    frame(40);

    printf("\n===== PORTRAIT, P25 =====\n");
    ls_tui_dump();

    int cols, rows;
    ls_tui_geometry(&cols, &rows, NULL, NULL);

    /* The wordmark is what gets dropped when 48 columns runs out, and the
       screen name and the right status are what must survive. */
    LS_CHECK_MSG(row_has(0, "P25") || row_has(0, "KBD"),
                 "portrait status row lost both the name and the status");

    char last[200];
    row_text(rows - 1, last, sizeof(last));
    LS_CHECK_MSG(strstr(last, "F10") == NULL && strstr(last, "F11") == NULL,
                 "portrait still draws a key legend: '%s'", last);

    /* And the control that replaced it is on the status row.
       Help only: the turn control went, because the sensor turns
       the screen and F11 does it from the keyboard. */
    LS_CHECK_MSG(!row_has(0, "[R]"),
                 "portrait still draws a turn control in its status row");
    LS_CHECK_MSG(row_has(0, "[?]"),
                 "portrait has no help control in its status row");

    /* And not in the corner cells, which is where the row's first
       control began before the report that the top bar's words ran into the
       round corners. This is the real inset at the real panel size, so the
       padding here is the padding the board draws. */
    const int pad = ls_tui_corner_pad(0);
    LS_CHECK_MSG(pad > 0, "a rounded panel with no corner padding");
    char top[200];
    row_text(0, top, sizeof(top));
    for (int x = 0; x < pad; x++)
        LS_CHECK_MSG(top[x] == ' ' && top[cols - 1 - x] == ' ',
                     "a word in corner cell %d of the status row: '%s'",
                     x, top);
    /* The clock's slot first - blank here, where nothing has set
       the time - then [?]. */
    LS_CHECK_MSG(strncmp(top + pad + LS_TUI_CLOCK_W + 1, "[?]", 3) == 0,
                 "[?] is not one clock in from the corner padding: '%s'",
                 top);
    ls_tui_end();
}

LS_CASE(every_screen_renders_something)
{
    /* Walk the tab strip and dump each one. The assertion is weak on purpose
       - the value here is the output - but a screen that renders an empty
       body is worth failing on. */
    LS_CHECK(ls_tui_begin(1232, 568));
    register_once();
    ls_tui_status_set("851.0125 MHz", "KBD Terminal Bay");

    int cols, rows;
    ls_tui_geometry(&cols, &rows, NULL, NULL);
    tui_surface *sf = ls_tui_surface();

    for (int i = 0; i < ls_tui_screen_count(); i++) {
        ls_tui_screen_show(i);
        frame(20);

        printf("\n===== SCREEN %d =====\n", i);
        ls_tui_dump();

        /* Count non-blank cells in the body, below the chrome. */
        int ink = 0;
        for (int y = 2; y < rows - 1; y++)
            for (int x = 0; x < cols; x++)
                if (sf->back[(size_t)y * cols + x].ch != ' ') ink++;
        LS_CHECK_MSG(ink > 40, "screen %d drew only %d cells of body", i, ink);
    }
    ls_tui_end();
}

LS_CASE(the_help_overlay_fits_the_screen_it_is_drawn_on)
{
    /* A fixed 46 columns against portrait's 48. Seeing it is the point. */
    LS_CHECK(ls_tui_begin(568, 1232));
    register_once();
    ls_tui_screen_show(0);

    int cols, rows;
    ls_tui_geometry(&cols, &rows, NULL, NULL);

    ls_tui_router_touch(ls_tui_corner_pad(0) + LS_TUI_CLOCK_W + 1, 0);
    frame(2);

    printf("\n===== PORTRAIT, KEY LIST =====\n");
    ls_tui_dump();

    bool found = false;
    for (int y = 0; y < rows && !found; y++) found = row_has(y, "KEYS");
    LS_CHECK_MSG(found, "the key list did not appear");

    ls_tui_router_touch(1, 1);          /* close it */
    ls_tui_end();
}

/* ------------------------------------------------------------- daylight -- */

static bool grid_has(const char *needle)
{
    int cols, rows;
    ls_tui_geometry(&cols, &rows, NULL, NULL);
    for (int y = 0; y < rows; y++)
        if (row_has(y, needle)) return true;
    return false;
}

/* Every native pixel not under a cell, and how many of them are not `want`. */
static void margin_check(uint16_t want, int *margin, int *wrong)
{
    *margin = *wrong = 0;
    for (int y = 0; y < NATIVE_H; y++)
        for (int x = 0; x < NATIVE_W; x++) {
            if (ls_tui_pixel_to_cell(x, y, NULL, NULL)) continue;
            (*margin)++;
            if (g_fb[(size_t)y * NATIVE_W + x] != want) (*wrong)++;
        }
}

/* Every pixel of every presented blank cell on the ground, and how many are
   not `want`. */
static void blank_check(uint16_t want, int *blank, int *wrong)
{
    tui_surface *sf = ls_tui_surface();
    int cols, rows;
    ls_tui_geometry(&cols, &rows, NULL, NULL);
    *blank = *wrong = 0;
    for (int y = 0; y < NATIVE_H; y++)
        for (int x = 0; x < NATIVE_W; x++) {
            int c, r;
            if (!ls_tui_pixel_to_cell(x, y, &c, &r)) continue;
            const tui_cell *cell = &sf->back[(size_t)r * cols + c];
            if (cell->ch != ' ' || TUI_ATTR_BG(cell->attr) != TUI_BLACK) continue;
            (*blank)++;
            if (g_fb[(size_t)y * NATIVE_W + x] != want) (*wrong)++;
        }
}

LS_CASE(daylight_whitens_the_ground_and_the_margin_and_off_gives_black_back)
{
    static const char *const LABELS[] = {
        "Brightness", "Auto dim", "Dim after", "Volume", "Theme", "Daylight",
        "Font", "Boot sound", "Alert sound", "Vibrate", "USB autoreboot",
    };
    static const int SIZE[2][2] = { { 568, 1232 }, { 1232, 568 } };

    for (int s = 0; s < 2; s++) {
        const char *posture = s ? "landscape" : "portrait";
        ls_tui_set_theme(ls_tui_theme_at(0));
        ls_tui_set_daylight(false);
        LS_CHECK(ls_tui_begin(SIZE[s][0], SIZE[s][1]));
        register_once();
        ls_tui_screen_show(2);                          /* SET */
        frame(3);

        /* The real grid, not a test pane: every setting has its box. */
        for (unsigned i = 0; i < sizeof(LABELS) / sizeof(LABELS[0]); i++)
            LS_CHECK_MSG(grid_has(LABELS[i]), "%s SET has no '%s' box",
                         posture, LABELS[i]);

        int margin, wrong, blank, bwrong;
        margin_check(0x0000, &margin, &wrong);
        LS_CHECK_MSG(margin > 0, "%s: no margin outside the grid to test", posture);
        LS_EQ_INT(0, wrong);

        ls_tui_set_daylight(true);
        frame(2);
        margin_check(0xFFFF, &margin, &wrong);
        LS_CHECK_MSG(wrong == 0, "%s: %d of %d margin pixels are not white "
                     "in Daylight", posture, wrong, margin);
        blank_check(0xFFFF, &blank, &bwrong);
        LS_CHECK_MSG(blank > 0 && bwrong == 0, "%s: %d of %d blank-cell pixels "
                     "are not white in Daylight", posture, bwrong, blank);
        LS_CHECK(ls_tui_get_theme() == ls_tui_theme_at(0));
        LS_CHECK(ls_tui_active_theme() == &ls_theme_daylight);

        printf("\n===== %s, SET, DAYLIGHT =====\n", s ? "LANDSCAPE" : "PORTRAIT");
        ls_tui_dump();

        ls_tui_set_daylight(false);
        frame(2);
        margin_check(0x0000, &margin, &wrong);
        LS_CHECK_MSG(wrong == 0, "%s: %d margin pixels stayed white after "
                     "Daylight went off", posture, wrong);
        blank_check(0x0000, &blank, &bwrong);
        LS_EQ_INT(0, bwrong);
        ls_tui_end();
    }
}

/* Each 5- or 6-bit channel of `px` lies between the same channel of a and b. */
static bool between565(uint16_t px, uint16_t a, uint16_t b)
{
    static const int SHIFT[3] = { 11, 5, 0 }, MASK[3] = { 0x1F, 0x3F, 0x1F };
    for (int i = 0; i < 3; i++) {
        const int p = (px >> SHIFT[i]) & MASK[i];
        const int x = (a >> SHIFT[i]) & MASK[i], y = (b >> SHIFT[i]) & MASK[i];
        if (p < (x < y ? x : y) || p > (x > y ? x : y)) return false;
    }
    return true;
}

LS_CASE(a_glyph_blends_toward_its_own_ground_in_either_polarity)
{

    for (int day = 0; day < 2; day++) {
        ls_tui_set_theme(ls_tui_theme_at(0));
        ls_tui_set_daylight(day != 0);
        LS_CHECK(ls_tui_begin(568, 1232));
        tui_surface *sf = ls_tui_surface();
        const tui_rect all = tui_surface_rect(sf);
        tui_fill(sf, all, ' ', TUI_ATTR(TUI_WHITE, TUI_BLACK));
        tui_put_char(sf, all, 5, 5, 'W', TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK));
        ls_tui_present();

        const ls_tui_theme_t *t = ls_tui_active_theme();
        const uint16_t ink = t->palette[TUI_RED | TUI_BRIGHT], ground = t->palette[0];
        int n_ground = 0, n_ink = 0, n_between = 0, n_outside = 0;
        for (int y = 0; y < NATIVE_H; y++)
            for (int x = 0; x < NATIVE_W; x++) {
                int c, r;
                if (!ls_tui_pixel_to_cell(x, y, &c, &r) || c != 5 || r != 5) continue;
                const uint16_t px = g_fb[(size_t)y * NATIVE_W + x];
                if (px == ground)                   n_ground++;
                else if (px == ink)                 n_ink++;
                else if (between565(px, ink, ground)) n_between++;
                else                                n_outside++;
            }
        LS_CHECK_MSG(n_ground > 0 && n_ink > 0 && n_between > 0,
                     "%s: the glyph drew %d ground, %d ink and %d edge pixels",
                     day ? "Daylight" : "Terminal Bay", n_ground, n_ink, n_between);
        LS_CHECK_MSG(n_outside == 0,
                     "%s: %d pixels of the glyph are not between its ink and "
                     "its own ground", day ? "Daylight" : "Terminal Bay", n_outside);
        ls_tui_end();
    }
    ls_tui_set_daylight(false);
}

LS_CASE(the_blitter_keeps_the_chosen_theme_under_daylight)
{

    const ls_tui_theme_t *amber = ls_tui_theme_by_name("Amber");
    LS_CHECK(amber != NULL);
    if (!amber) return;

    ls_tui_set_theme(amber);
    ls_tui_set_daylight(true);
    LS_CHECK(ls_tui_daylight());
    LS_CHECK(ls_tui_get_theme() == amber);
    LS_CHECK(ls_tui_active_theme() == &ls_theme_daylight);

    /* F9 through the real router: the theme underneath steps, Daylight stays. */
    ls_tui_router_key(LS_TK_F9, 0);
    const ls_tui_theme_t *stepped = ls_tui_get_theme();
    LS_CHECK_MSG(stepped == ls_tui_theme_next(amber),
                 "F9 under Daylight did not step the theme underneath");
    LS_CHECK_MSG(ls_tui_daylight(), "F9 turned Daylight off");
    LS_CHECK(ls_tui_active_theme() == &ls_theme_daylight);

    ls_tui_set_daylight(false);
    LS_CHECK_MSG(ls_tui_active_theme() == stepped,
                 "Daylight off did not give back the theme stepped to under it");

    /* Handing Daylight to the theme setter is the toggle, not a choice. */
    ls_tui_set_theme(&ls_theme_daylight);
    LS_CHECK(ls_tui_daylight());
    LS_CHECK(ls_tui_get_theme() == stepped);

    ls_tui_set_daylight(false);
    ls_tui_set_theme(ls_tui_theme_at(0));
}
