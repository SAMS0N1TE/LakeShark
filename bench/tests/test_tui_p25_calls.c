/* P25 CALLS: the live face (ON AIR card, BUSIEST strip, HOLD / LOCKOUT)
   and the observed store's minute ring behind its sparklines. Drawn with
   the production font and blitter at the board's grids, portrait and both
   landscapes, with and without the TFT camera hole. */
#include "ls_test.h"
#include "ls_calls.h"
#include "call_archive.h"
#include "ls_picker.h"
#include "ls_tui.h"
#include "ls_panel.h"
#include "esp_timer.h"
#include "p25_state.h"
#include "p25_tg_observed.h"
#include "p25_program.h"
#include "../../components/apps/tui/screens/main/p25_calls_live.h"
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- P25 -- */

p25_state_t P25;
uint32_t s_tune_freq_hz = 851012500;
static int policy_calls, unlock_calls;
bool p25_ui_policy(p25_ui_policy_t kind, uint16_t value)
{
    policy_calls++;
    if (kind == P25_UI_HOLD_SET) { P25.hold_tg = value; return true; }
    if (kind == P25_UI_LOCKOUT_ADD && value && P25.lockout_count < 64) {
        P25.lockout[P25.lockout_count++] = value; return true;
    }
    return false;
}
bool p25_ui_lockout_remove(uint16_t tg)
{
    unlock_calls++;
    for (unsigned i = 0; i < P25.lockout_count; i++)
        if (P25.lockout[i] == tg) { P25.lockout[i] = P25.lockout[--P25.lockout_count]; return true; }
    return false;
}
const p25_program_t *p25_program_session(void) { return NULL; }
bool p25_p2_enabled(void) { return false; }
const char *p25_calls_host_alias(uint16_t tg)
{
    return tg == 1041 ? "FIRE DISP" : tg == 2104 ? "PD CENTRAL" : tg == 2051 ? "EMS NORTH" : NULL;
}

/* ------------------------------------------------------------ archive -- */

static bool wide;
static int tracks = 4;
bool ls_tui_is_wide(void) { return wide; }
bool ls_tui_keyboard_mode(void) { return false; }
const char *ls_tui_radio_claimed(void) { return "P25"; }
void ls_tui_radio_want(const char *mode) { (void)mode; }
void call_archive_init(void) {}
void call_archive_refresh(void) {}
bool call_archive_protect(const char *path, bool protect) { (void)path; (void)protect; return true; }
void call_archive_storage(call_archive_storage_t *out)
{ *out = (call_archive_storage_t){.total = 8000000000ULL, .free = 3900000000ULL,
    .archive = 4200000, .valid = true, .paused = false}; }
bool call_archive_busy(void) { return false; }
int call_archive_count(void) { return tracks; }
bool call_archive_entry(int index, call_entry_t *entry)
{
    static const unsigned TG[] = {2104, 1041, 1041, 2051};
    if (index < 0 || index >= tracks) return false;
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->path, sizeof(entry->path), "/sdcard/lakeshark/calls/20261028/0100%02d.wav", index);
    entry->meta.time = 1793174400 - index * 120;
    entry->meta.hz = 851262500; entry->meta.talkgroup = TG[index % 4];
    entry->meta.source = 220158 + index; entry->meta.rate = 8000;
    entry->duration_ms = 4000 + index * 3000; entry->bytes = 64000;
    return true;
}
bool call_archive_delete(const char *path) { (void)path; return true; }
static int option[CALL_OPTIONS];
int call_archive_option(call_option_t o) { return option[o]; }
void call_archive_set_option(call_option_t o, int v) { option[o] = v; }
const char *call_archive_talkgroups(void) { return ""; }
bool call_archive_set_talkgroups(const char *text) { (void)text; return true; }
void ls_opt_open(const ls_opt_ctx_t *ctx) { (void)ctx; }
void ls_picker_open(const char *title, ls_picker_done_t done) { (void)title; (void)done; }
bool ls_picker_add(const char *label, const char *detail) { (void)label; (void)detail; return true; }
void ls_picker_select(int index) { (void)index; }
void ls_picker_back(void (*back)(void)) { (void)back; }

/* -------------------------------------------------------------- panel -- */

static int native_w = 568, native_h = 1232;
static uint16_t pixels[1232 * 568];
bool ls_panel_fb(ls_panel_fb_t *out)
{
    memset(out, 0, sizeof(*out));
    out->pixels = pixels; out->width = native_w; out->height = native_h;
    return true;
}
void ls_panel_fb_present(void) {}
void ls_panel_fb_present_rows(int y0, int y1) { (void)y0; (void)y1; }

/* ------------------------------------------------------------ helpers -- */

#define UNTOUCHED ((int16_t)0x7ffe)
static int cols, rows;
static tui_surface *sf;

/* A board grid, portrait or landscape (cw or ccw), AMOLED or the TFT. */
static void begin(bool landscape, bool clockwise, bool tft)
{
    native_w = tft ? 540 : 568;
    native_h = tft ? 1168 : 1232;
    /* variants/t_display_p4.h: the HI8561's camera, native portrait px. */
    ls_tui_set_cutout(tft ? 270 : 0, tft ? 38 : 0, tft ? 28 : 0);
    ls_tui_set_rotation_cw(clockwise);
    wide = landscape;
    LS_CHECK(ls_tui_begin(landscape ? native_h : native_w, landscape ? native_w : native_h));
    ls_tui_geometry(&cols, &rows, NULL, NULL);
    sf = ls_tui_surface();
}
static void paint(tui_rect area)
{
    tui_frame_begin(sf);
    for (int i = 0; i < sf->w * sf->h; i++) sf->back[i] = (tui_cell){UNTOUCHED, 0};
    ls_calls_draw(sf, area);
}
static bool find(tui_rect area, const char *text, int *x, int *y)
{
    const int n = (int)strlen(text);
    for (int row = area.y; row < area.y + area.h; row++)
        for (int col = area.x; col + n <= area.x + area.w; col++) {
            int i = 0;
            while (i < n && sf->back[row * sf->w + col + i].ch == text[i]) i++;
            if (i == n) { if (x) *x = col; if (y) *y = row; return true; }
        }
    return false;
}
/* A key: its label with only spaces between it and a '|' on each side. */
static bool find_key(tui_rect area, const char *label, int *x, int *y)
{
    const int n = (int)strlen(label);
    for (int row = area.y; row < area.y + area.h; row++)
        for (int col = area.x + 1; col + n < area.x + area.w; col++) {
            int i = 0;
            while (i < n && sf->back[row * sf->w + col + i].ch == label[i]) i++;
            if (i != n) continue;
            int l = col - 1, r = col + n;
            while (l > area.x && sf->back[row * sf->w + l].ch == ' ') l--;
            while (r < area.x + area.w - 1 && sf->back[row * sf->w + r].ch == ' ') r++;
            if (sf->back[row * sf->w + l].ch == '|' && sf->back[row * sf->w + r].ch == '|') {
                if (x) *x = col + n / 2;
                if (y) *y = row;
                return true;
            }
        }
    return false;
}
static int written_outside(tui_rect area)
{
    int n = 0;
    for (int row = 0; row < sf->h; row++)
        for (int col = 0; col < sf->w; col++)
            if (!tui_rect_contains(area, col, row) &&
                sf->back[row * sf->w + col].ch != UNTOUCHED) n++;
    return n;
}
/* Cells inside the camera hole that carry anything but blank. */
static int written_in_hole(void)
{
    tui_rect hole;
    if (!ls_tui_cutout(&hole)) return 0;
    int n = 0;
    for (int row = hole.y; row < hole.y + hole.h; row++)
        for (int col = hole.x; col < hole.x + hole.w; col++) {
            const int16_t ch = sf->back[row * sf->w + col].ch;
            if (ch != UNTOUCHED && ch != ' ') n++;
        }
    return n;
}

/* A call on air, as the decoder would leave the state mid-transmission. */
static void on_air(uint16_t tg, uint32_t unit, bool encrypted)
{
    P25.receive_state = encrypted ? P25_RX_ENCRYPTED : P25_RX_AUDIO;
    P25.grant_on_traffic = true;
    P25.grant_talkgroup = tg;
    P25.dsd_tg = tg;
    P25.dsd_src = (int)unit;
    P25.p25_enc_muted = encrypted;
    P25.voice_active_until_us = encrypted ? 0 : esp_timer_get_time() + 500000;
    P25.voice_peak = encrypted ? 0 : 12000;
    P25.iq_level = 0.6f;
}
static void off_air(void)
{
    P25.receive_state = P25_RX_CONTROL_LOCKED;
    P25.grant_on_traffic = false;
    P25.voice_active_until_us = 0;
    P25.p25_enc_muted = false;
}
static void seed_store(uint32_t now_ms)
{
    LS_CHECK(p25_tg_observed_clear());
    for (int m = 0; m < 12; m++)
        for (int k = 0; k <= m % 4; k++) {
            p25_tg_observed_record(851262500, 0x293, 1041, P25_TG_SEEN_LCW,
                                   now_ms - (11 - m) * 60000u - 1000u * k);
            if (m % 3 == 0)
                p25_tg_observed_record(851262500, 0x293, 2104, P25_TG_SEEN_GRANT,
                                       now_ms - (11 - m) * 60000u - 1000u * k);
        }
    p25_tg_observed_record(851512500, 0x293, 2051, P25_TG_SEEN_GRANT, now_ms - 90000u);
}

/* ------------------------------------------------------------- the ring -- */

LS_CASE(observed_minute_ring_counts_clears_and_ages_out)
{
    p25_tg_observed_snapshot_t v;
    LS_CHECK(p25_tg_observed_clear());
    const uint32_t t = 30u * 60000u;               /* minute 30 */
    for (int i = 0; i < 3; i++) p25_tg_observed_record(155000000, 1, 7, P25_TG_SEEN_LCW, t + i);
    p25_tg_observed_record(155000000, 1, 7, P25_TG_SEEN_LCW, t + 60000u);   /* minute 31 */
    /* A second channel is another row; its minutes are its own. */
    p25_tg_observed_record(156000000, 1, 7, P25_TG_SEEN_LCW, t + 60000u);
    LS_CHECK(p25_tg_observed_read(&v));
    LS_EQ_INT(v.count, 2);
    LS_EQ_UINT(p25_tg_observed_minute_hits(&v.rows[0], t + 60000u, 0), 1);
    LS_EQ_UINT(p25_tg_observed_minute_hits(&v.rows[0], t + 60000u, 1), 3);
    LS_EQ_UINT(p25_tg_observed_minute_hits(&v.rows[0], t + 60000u, 2), 0);
    /* Read later: the same minutes, further back, and nothing since. */
    LS_EQ_UINT(p25_tg_observed_minute_hits(&v.rows[0], t + 5 * 60000u, 5), 3);
    LS_EQ_UINT(p25_tg_observed_minute_hits(&v.rows[0], t + 5 * 60000u, 0), 0);
    /* Older than the ring, past its last index, or no row: nothing. */
    LS_EQ_UINT(p25_tg_observed_minute_hits(&v.rows[0], t + 40 * 60000u, 10), 0);
    LS_EQ_UINT(p25_tg_observed_minute_hits(&v.rows[0], t + 60000u, P25_TG_OBSERVED_MINUTES), 0);
    LS_EQ_UINT(p25_tg_observed_minute_hits(NULL, t, 0), 0);
    /* Fifteen minutes on, minute 31 is the oldest the ring holds; one
       minute more and it is cleared, not read back as a new minute. */
    p25_tg_observed_record(155000000, 1, 7, P25_TG_SEEN_LCW, t + 16 * 60000u);
    LS_CHECK(p25_tg_observed_read(&v));
    LS_EQ_UINT(p25_tg_observed_minute_hits(&v.rows[0], t + 16 * 60000u, 0), 1);
    LS_EQ_UINT(p25_tg_observed_minute_hits(&v.rows[0], t + 16 * 60000u, 15), 1);
    unsigned between = 0;
    for (unsigned ago = 1; ago < 15; ago++)
        between += p25_tg_observed_minute_hits(&v.rows[0], t + 16 * 60000u, ago);
    LS_EQ_UINT(between, 0);
    p25_tg_observed_record(155000000, 1, 7, P25_TG_SEEN_LCW, t + 17 * 60000u);
    LS_CHECK(p25_tg_observed_read(&v));
    LS_EQ_UINT(p25_tg_observed_minute_hits(&v.rows[0], t + 17 * 60000u, 15), 0);
    LS_EQ_UINT(p25_tg_observed_minute_hits(&v.rows[0], t + 17 * 60000u, 1), 1);
    /* A bucket saturates instead of wrapping to zero. */
    for (int i = 0; i < 300; i++) p25_tg_observed_record(155000000, 1, 9, P25_TG_SEEN_LCW, t);
    LS_CHECK(p25_tg_observed_read(&v));
    LS_EQ_UINT(p25_tg_observed_minute_hits(&v.rows[2], t, 0), 255);
    LS_EQ_UINT(v.rows[2].hits, 300);
}

/* ------------------------------------------------------------ the call -- */

LS_CASE(live_card_follows_a_call_start_end_and_keeps_the_last)
{
    memset(&P25, 0, sizeof(P25));
    ls_shim_time_set(20LL * 60 * 1000000);
    off_air();
    p25_calls_live_tick();
    uint16_t tg = 0; uint32_t unit = 0, ms = 0; bool live = true;
    on_air(1041, 220158, false);
    p25_calls_live_tick();
    ls_shim_time_advance(4000000);
    P25.voice_active_until_us = esp_timer_get_time() + 500000;
    p25_calls_live_tick();
    LS_CHECK(p25_calls_live_now(&tg, &unit, &live, &ms));
    LS_EQ_INT(tg, 1041); LS_EQ_UINT(unit, 220158); LS_CHECK(live);
    LS_EQ_UINT(ms, 4000);
    /* A gap shorter than the end of a transmission is the same call. */
    off_air();
    ls_shim_time_advance(1500000);
    p25_calls_live_tick();
    LS_CHECK(p25_calls_live_now(NULL, NULL, &live, NULL) && live);
    ls_shim_time_advance(1000000);
    p25_calls_live_tick();
    LS_CHECK(p25_calls_live_now(&tg, NULL, &live, &ms));
    LS_CHECK(!live); LS_EQ_INT(tg, 1041); LS_EQ_UINT(ms, 4000);
    /* A different talkgroup is a new call; the clock starts again. */
    on_air(2104, 3301118, false);
    p25_calls_live_tick();
    LS_CHECK(p25_calls_live_now(&tg, &unit, &live, &ms));
    LS_EQ_INT(tg, 2104); LS_CHECK(live); LS_EQ_UINT(ms, 0);
    off_air();
    ls_shim_time_advance(3000000);
    p25_calls_live_tick();
}

/* ---------------------------------------------------------- the layout -- */

static void layout_case(bool landscape, bool clockwise, bool tft, bool encrypted)
{
    begin(landscape, clockwise, tft);
    memset(&P25, 0, sizeof(P25));
    ls_shim_time_set(20LL * 60 * 1000000);
    seed_store(20u * 60000u);
    P25.p25_enc_tg_count = 1; P25.p25_enc_tg[0].talkgroup = 2051;
    P25.dsd_has_sync = true;
    p25_calls_live_register();
    on_air(encrypted ? 2051 : 1041, 220158, encrypted);
    for (int i = 0; i < 30; i++) {
        p25_calls_live_tick();
        ls_shim_time_advance(100000);
        if (!encrypted) P25.voice_active_until_us = esp_timer_get_time() + 500000;
    }
    /* The area CALLS gets: below the status row and nav bar, above the hint. */
    const tui_rect area = landscape ? tui_rect_make(0, 2, cols, rows - 3)
                                    : tui_rect_make(0, 6, cols, rows - 7);
    ls_calls_open("P25");
    paint(area);
    const char *tag = landscape ? (clockwise ? "landscape" : "landscape ccw") : "portrait";
    LS_CHECK_MSG(written_outside(area) == 0, "%s %s drew outside its area", tag, tft ? "tft" : "amoled");
    if (tft && landscape) {
        /* The hole really is inside the CALLS area here, so the check below
           is about routing, not about missing it. */
        tui_rect hole;
        LS_CHECK(ls_tui_cutout(&hole));
        LS_CHECK(tui_rect_intersect(hole, area).w > 0);
    }
    LS_CHECK_MSG(written_in_hole() == 0, "%s: %d cells under the camera", tag, written_in_hole());
    LS_CHECK_MSG(find(area, encrypted ? "ENC" : "CLEAR", NULL, NULL), "%s: no clear/enc chip", tag);
    LS_CHECK_MSG(find(area, "ON AIR", NULL, NULL), "%s: no ON AIR card", tag);
    LS_CHECK_MSG(find(area, encrypted ? "SIGNAL" : "VOICE", NULL, NULL), "%s: no trace label", tag);
    LS_CHECK_MSG(find(area, "BUSIEST", NULL, NULL), "%s: no talkgroup strip", tag);
    LS_CHECK_MSG(find(area, "P25 CALLS", NULL, NULL), "%s: no title", tag);
    char hold[24], lock[24];
    const unsigned tg = encrypted ? 2051 : 1041;
    snprintf(hold, sizeof(hold), "HOLD %u", tg);
    snprintf(lock, sizeof(lock), "LOCKOUT %u", tg);
    static const char *const keys[] = {"PLAY", "STOP", "KEEP", "DELETE", "FILES", "OPTIONS", "BACK",
                                       "ALL", "P25", "FM", "TODAY", "NEWEST"};
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        LS_CHECK_MSG(find_key(area, keys[i], NULL, NULL), "%s: no %s key", tag, keys[i]);
    int x, y;
    /* HOLD and LOCKOUT act on the talkgroup the card shows, by a thumb. */
    LS_CHECK_MSG(find_key(area, hold, &x, &y), "%s: no %s key", tag, hold);
    policy_calls = 0;
    LS_CHECK(ls_calls_touch(x, y));
    LS_EQ_INT(P25.hold_tg, tg); LS_EQ_INT(policy_calls, 1);
    paint(area);
    LS_CHECK_MSG(find_key(area, "RELEASE", NULL, NULL) || find(area, "RELEASE", NULL, NULL),
                 "%s: hold does not offer release", tag);
    LS_CHECK(find(area, "RELEASE", &x, &y));
    LS_CHECK(ls_calls_touch(x, y));
    LS_EQ_INT(P25.hold_tg, 0);
    paint(area);
    LS_CHECK_MSG(find_key(area, lock, &x, &y), "%s: no %s key", tag, lock);
    LS_CHECK(ls_calls_touch(x, y));
    LS_EQ_INT(P25.lockout_count, 1); LS_EQ_INT(P25.lockout[0], tg);
    paint(area);
    LS_CHECK(find(area, "UNLOCK", &x, &y));
    unlock_calls = 0;
    LS_CHECK(ls_calls_touch(x, y));
    LS_EQ_INT(unlock_calls, 1); LS_EQ_INT(P25.lockout_count, 0);
    /* A strip row filters the recordings to its talkgroup. */
    paint(area);
    int sx, sy, bx, by;
    LS_CHECK(find(area, "BUSIEST", &bx, &by));
    const tui_rect strip = tui_rect_intersect(tui_rect_make(area.x, by + 1, bx + 40, 10), area);
    LS_CHECK_MSG(find(strip, "PD CENTRAL", &sx, &sy), "%s: PD CENTRAL not in the strip", tag);
    LS_CHECK(ls_calls_touch(sx, sy));
    paint(area);
    LS_CHECK_MSG(find_key(area, "TG 2104", NULL, NULL), "%s: strip tap did not filter", tag);
    LS_CHECK(written_in_hole() == 0);
    ls_calls_show_talkgroup(0);
    ls_calls_leave();
    off_air();
    ls_tui_end();
    ls_tui_set_cutout(0, 0, 0);
}

LS_CASE(live_calls_fit_portrait_and_landscape_amoled)
{
    layout_case(false, true, false, false);
    layout_case(true, true, false, false);
    layout_case(true, false, false, true);
}

LS_CASE(live_calls_route_around_the_tft_camera)
{
    layout_case(false, true, true, false);
    layout_case(true, true, true, false);
    layout_case(true, false, true, false);
    layout_case(true, false, true, true);
}

/* Without a live face (FM, or before P25 registers) CALLS is the archive:
   the selected call on stage with the player, and nothing live drawn. */
LS_CASE(archive_without_live_face_has_no_live_parts)
{
    begin(false, true, false);
    ls_calls_set_live("P25", NULL);
    const tui_rect area = tui_rect_make(0, 6, cols, rows - 7);
    ls_calls_open("FM");
    paint(area);
    LS_CHECK(!find(area, "ON AIR", NULL, NULL));
    LS_CHECK(!find(area, "BUSIEST", NULL, NULL));
    LS_CHECK(find(area, "CALL 1 OF 4", NULL, NULL));
    LS_CHECK(find_key(area, "PLAY", NULL, NULL));
    LS_CHECK(written_outside(area) == 0);
    ls_calls_leave();
    ls_tui_end();
}
