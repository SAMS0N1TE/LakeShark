/* WARDRIVE: Wi-Fi, Bluetooth, cells and drones on the move, every one logged
   with where and when, in one session with one START.

   LIVE   the drive as it happens: speed in big stroke digits, a heading tape,
          counters by kind and by Wi-Fi security, and the names of new
          networks raining down as they are found, newest underneath.
   3D     the drive in lap3d: the ribbon of the route, a pillar per network.
   LIST   strongest or newest, by kind; a row opens its address and where.
   MAP    MAP with the drive drawn over it (the RF SURVEY layer's frame),
          framed to fit the whole drive.
   REPLAY a saved drive played back through all of the above (wd_replay.h):
          R, OPTIONS > REPLAY A DRIVE, or the REPLAY chip on the idle page.

   A session keeps running when the app is left (a worker task does the
   scanning and the writing). STOP writes the GPX and closes the WiGLE CSV in
   /sdcard/wardrive; OPTIONS sends both to the locker.

   Nothing moves on a timer: a drop falls because a network was just found,
   the arcs step and the 3D ring goes out because a scan finished, the
   digits and the tape follow the GPS. */
#include "../../wardrive/wd_core.h"
#include "../../wardrive/wd_feed.h"
#include "../../wardrive/wd_view3d.h"
#include "../../wardrive/wd_replay.h"
#include "../../wardrive/wd_page.h"
#include "../../ls_plate.h"
#include "../../ls_calls.h"
#include "../../wardrive/wd_sig.h"
#include "../../wardrive/wd_radio.h"
#include "../../wardrive/wd_atlas.h"
#include "../../ls_lsx_svc.h"
#include "../../ls_drag_scroll.h"
#include "../../ls_picker.h"
#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_deskui.h"
#include "../../ls_text.h"
#include "../../ls_options.h"
#include "../../ls_app.h"
#include "../../ls_braille.h"
#include "../../ls_rfmap.h"
#include "../../ls_map_ink.h"
#include "../../ls_map.h"
#include "ls_location.h"
#include "core/settings.h"
#include "core/ls_tz.h"
#include "esp_timer.h"
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

/* everything sits on the plates' slate ground (ls_plate.h) */
#define G       LS_PLATE_GROUND
#define INK(c)  ls_ink_hue(TUI_ATTR((c), G))
#define LIT(c)  INK((c) | TUI_BRIGHT)
#define FALL_MS 1600u

enum { PG_LIVE, PG_3D, PG_LIST, PG_HUNT, PG_RADIO, PG_ATLAS, PG_N };
/* the pages that live in their own files (wd_page.h), by PG_ - PG_HUNT */
static const wd_page_t *const XPAGE[PG_N - PG_HUNT] = { &wd_page_hunt, &wd_page_radio, &wd_page_atlas };
static const wd_page_t *xpage(int p) { return p >= PG_HUNT && p < PG_N ? XPAGE[p - PG_HUNT] : NULL; }
enum { F_ALL, F_WIFI, F_OPEN, F_BLE, F_CELL, F_DRONE, F_RF, F_AIR, F_LORA, F_HITS, F_N };
/* tags that make a device a target (what HUNT lists), not just a note */
#define WD_HIT_TAGS ((uint16_t)~(WD_T_REGULAR | WD_T_NEWHERE))

static int s_page;
static bool s_src[WD_KINDS] = { true, true, true, true, true, true, true };
static int s_pace = WD_PACE_NORMAL, s_units, s_only_fix;
static int s_filter, s_sort, s_sel, s_top, s_rows_shown;
static int s_sel_dev = -1;           /* the device the selection is on, kept across re-sorts */
static ls_drag_scroll_t s_drag;
static tui_rect s_bar;               /* the list's scroll bar */
static char s_say[64];               /* a one-line answer to the last key (MAP with nothing to show) */
static uint32_t s_say_ms;
static tui_rect s_replay_chip, s_rp_bar, s_rp_btn[4], s_show_chip;
/* The 3D page's keys: a D-pad (tilt up/down, turn left/right, FOLLOW in the
   middle), the zooms over it, FIT ALL and CHASE under it, then BACK, LIGHT
   MAP and HIDE. HIDE gives the view the page until SHOW KEYS. */
enum { K_BACK = -1, K_HIDE = -2 };
static const int GRID[] = {
    WD3D_OUT,  WD3D_UP,     WD3D_IN,
    WD3D_LEFT, WD3D_FOLLOW, WD3D_RIGHT,
    WD3D_FIT,  WD3D_DOWN,   WD3D_CHASE,
    K_BACK,    WD3D_LIGHT,  K_HIDE,
};
#define GRID_N ((int)(sizeof(GRID) / sizeof(GRID[0])))
/* Colour by what a key does: zoom blue, tilt magenta, turn yellow, where the
   camera goes (FOLLOW, FIT ALL, CHASE) green, leaving the view (BACK, HIDE)
   red, the map's look cyan. A lit key is its bright shade. */
static const uint8_t GRID_HUE[] = {
    TUI_BLUE,   TUI_MAGENTA, TUI_BLUE,
    TUI_YELLOW, TUI_GREEN,   TUI_YELLOW,
    TUI_GREEN,  TUI_MAGENTA, TUI_GREEN,
    TUI_RED,    TUI_CYAN,    TUI_RED,
};
_Static_assert(sizeof(GRID_HUE) == sizeof(GRID) / sizeof(GRID[0]), "a colour for every key");
static bool s_keys_hidden;
static EXT_RAM_BSS_ATTR uint16_t s_order[WD_CAP];
static int s_norder;
static uint32_t s_order_ms, s_step_ms;
static bool s_perf;
static tui_rect s_3d, s_list, s_chip[F_N + 1];
static EXT_RAM_BSS_ATTR wd_stats_t st;                       /* this frame's copy */
static EXT_RAM_BSS_ATTR wd_drop_t drops[WD_DROPS];
static int ndrops;

static uint32_t real_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
/* The session's clock: the board's, or a replay's own. */
static uint32_t now_ms(void) { return wd_replay_now(real_ms()); }
static uint8_t a_text(void)   { return ls_ink(LS_INK_TEXT, TUI_ATTR(TUI_WHITE, TUI_BLACK)); }
static uint8_t a_strong(void) { return ls_ink(LS_INK_STRONG, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK)); }
static uint8_t a_dim(void)    { return ls_ink(LS_INK_DIM, LS_ATTR_FAINT); }
static uint8_t a_rule(void)   { return ls_ink(LS_INK_RULE, LS_ATTR_FAINT); }
static uint8_t a_accent(void) { return ls_ink(LS_INK_ACCENT, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK)); }

static void put(tui_surface *sf, tui_rect clip, int x, int y, const char *s, uint8_t at)
{
    if (y >= clip.y && y < clip.y + clip.h) tui_put_str(sf, clip, x, y, s, at);
}

static uint8_t kind_hue(int kind, int sec)
{
    if (kind == WD_WIFI) {
        static const uint8_t H[WD_SECS] = { TUI_GREEN, TUI_YELLOW, TUI_YELLOW, TUI_CYAN, TUI_BLUE };
        return H[sec >= 0 && sec < WD_SECS ? sec : 3];
    }
    static const uint8_t K[WD_KINDS] = { TUI_GREEN, TUI_MAGENTA, TUI_WHITE, TUI_RED, TUI_YELLOW, TUI_CYAN, TUI_GREEN };
    return kind >= 0 && kind < WD_KINDS ? K[kind] : TUI_WHITE;
}
static char kind_glyph(int kind) { return kind >= 0 && kind < WD_KINDS ? "WBCDRAL"[kind] : '?'; }
static const char *sec_word(int sec) { static const char *const W[] = { "OPEN", "WEP", "WPA", "WPA2", "WPA3" }; return W[sec % WD_SECS]; }

static void clock_text(char *out, size_t n, uint32_t ms)
{
    const unsigned s = ms / 1000;
    snprintf(out, n, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
}

/* Units are kept across a restart. Until someone picks them, a board whose
   GPS puts it in the US or the UK shows mph and miles, any other km/h. */
#define UNITS_UNSET 2u
static bool s_units_chosen;
static bool imperial_place(double lat, double lon)
{
    return (lat > 24 && lat < 50 && lon > -125 && lon < -66) ||      /* lower 48 */
           (lat > 51 && lat < 72 && lon > -170 && lon < -129) ||     /* Alaska */
           (lat > 18 && lat < 23 && lon > -161 && lon < -154) ||     /* Hawaii */
           (lat > 49.8 && lat < 61 && lon > -8.7 && lon < 1.9);      /* UK */
}
static void units_settle(void)
{
    static bool read;
    if (s_units_chosen) return;
    if (!read) {
        read = true;
        const uint32_t v = settings_get_rec("wd_units", UNITS_UNSET);
        if (v != UNITS_UNSET) { s_units = v ? 1 : 0; s_units_chosen = true; return; }
    }
    const wd_stats_t *st = wd_stats();
    if (st->fix.fix) s_units = imperial_place(st->fix.lat, st->fix.lon);
}

static float speed_shown(float mps) { return s_units ? mps * 2.23694f : mps * 3.6f; }
static const char *speed_unit(void) { return s_units ? "mph" : "km/h"; }
static float dist_shown(float m) { return s_units ? m / 1609.344f : m / 1000.0f; }
static const char *dist_unit(void) { return s_units ? "mi" : "km"; }

/* A signal level as rising braille columns: -100 dBm empty, -40 full. */
static void meter(tui_surface *sf, tui_rect clip, int x, int y, int cells, int rssi, uint8_t at)
{
    const float f = fmaxf(0, fminf(1, (rssi + 100) / 60.0f));
    const int cols = cells * 2, lit = (int)lroundf(f * cols);
    for (int c = 0; c < cells; c++) {
        uint8_t bits = 0;
        for (int half = 0; half < 2; half++) {
            const int k = c * 2 + half;
            if (k < lit) bits |= ls_braille_column(1 + k * 4 / cols, half);
            else bits |= ls_braille_dot(0, half);
        }
        ls_braille_put(sf, clip, x + c, y, (int16_t)(0x2800 | bits), at);
    }
}

/* ------------------------------------------------------------ session */

static void say_now(const char *t) { snprintf(s_say, sizeof(s_say), "%s", t); s_say_ms = real_ms(); }
static void map_layer(const ls_rfmap_frame_t *f);

static void session_start(void)
{
    if (wd_replay_on()) wd_replay_close();
    const uint32_t now = real_ms();
    bool src[WD_KINDS];
    for (int k = 0; k < WD_KINDS; k++) src[k] = s_src[k] && wd_feed_has(k);
    wd_lock();
    const bool ok = wd_begin(now, src, s_only_fix);
    wd_unlock();
    if (!ok) return;
    wd_feed_start(src, s_pace);
    (void)wd_feed_spawn();
    /* The drive goes on MAP as it is driven, with or without M: MAP in a
       desk window beside this one shows it live. */
    ls_rfmap_set_extra(map_layer);
    s_sel = s_top = 0;
    s_order_ms = 0;
}

static void session_stop(void)
{
    wd_lock();
    wd_end(real_ms());
    wd_request(WD_REQ_EXPORT);
    wd_unlock();
    wd_feed_stop();
    (void)wd_feed_spawn();
}

static void session_toggle(void)
{
    if (wd_replay_on()) { wd_replay_close(); s_sel = s_top = 0; s_sel_dev = -1; s_order_ms = 0; return; }
    if (wd_stats()->running) session_stop(); else session_start();
}

/* Running in the background: HOME's lamp. A replay is not a drive. */
bool ls_wardrive_running(void) { return wd_stats()->running && !wd_stats()->replay; }

/* ------------------------------------------------------------ REPLAY */

static char s_logs[WD_REPLAY_LIST][WD_REPLAY_NAME];
static int s_nlogs;
static void page(int p);
static void replay_picked(int i)
{
    if (i < 0 || i >= s_nlogs) return;
    char why[64];
    /* The 3D view and its map hold megabytes a long drive needs (more the
       longer it was looked around): give them back first. While the drive
       is read the 3D page draws only the notice, so it takes them back after. */
    wd3d_leave();
    if (!wd_replay_open(s_logs[i], real_ms(), why, sizeof(why))) { say_now(why); return; }
    ls_rfmap_set_extra(NULL);
    s_sel = s_top = 0; s_sel_dev = -1; s_order_ms = 0;
    page(PG_3D);
}
/* The drives as a table: when (local time, in the zone SET > Time zone
   has, with its daylight time on the day), how long, how far, how many
   finds, how big. Newest first, as the stems sort. */
static wd_drive_info_t s_drive[WD_REPLAY_LIST];

/* "TUE NOV 24  8:57 PM", or "TUE NOV 24 20:57" on a 24 h clock: the time
   always from column 11. */
static void when_text(char *out, size_t n, uint32_t utc)
{
    static const char *const DAY[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
    static const char *const MON[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
    if (!utc) { snprintf(out, n, "no clock"); return; }
    struct tm tm;
    if (!ls_tz_local((time_t)utc, &tm)) { snprintf(out, n, "?"); return; }
    if (ls_tz_24h()) {
        snprintf(out, n, "%s %s %2d %02d:%02d", DAY[tm.tm_wday % 7], MON[tm.tm_mon % 12], tm.tm_mday, tm.tm_hour, tm.tm_min);
        return;
    }
    const int h = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
    snprintf(out, n, "%s %s %2d %2d:%02d %s", DAY[tm.tm_wday % 7], MON[tm.tm_mon % 12], tm.tm_mday, h, tm.tm_min,
             tm.tm_hour >= 12 ? "PM" : "AM");
}

/* The zone's abbreviation at `utc` ("EST", "EDT", "UTC"). */
static void when_zone(char *out, size_t n, uint32_t utc)
{
    struct tm tm;
    if (!ls_tz_local((time_t)utc, &tm)) { snprintf(out, n, "UTC"); return; }
    ls_tz_abbr(&tm, out, n);
}

/* Over the list: the drive under the cursor in full (its date big, from
   and to, how long and far, what was found, its folder), and the column
   names at the bottom, over the rows they name. */
static void replay_header(tui_surface *sf, tui_rect a)
{
    char t[72];
    const int cy = a.y + a.h - 1;
    static const char cols[] = "LONG     KM FINDS SIZE";
    if (ls_tui_is_wide()) {
        /* the card sits beside the list: name the columns in words */
        tui_put_str(sf, a, a.x + 2, cy, "rows: when, length, km, finds, size", LS_ATTR_FAINT);
    } else {
        char zone[12], head[24];
        when_zone(zone, sizeof(zone), s_nlogs > 0 && s_drive[0].start_utc ? s_drive[0].start_utc : 0);
        snprintf(head, sizeof(head), "WHEN (%s)", zone);
        tui_put_str(sf, a, a.x + 2, cy, head, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        tui_put_str(sf, a, a.x + a.w - 2 - (int)sizeof(cols) + 1, cy, cols, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    }
    const int i = ls_picker_current();
    if (i < 0 || i >= s_nlogs || a.h < 9) return;
    const wd_drive_info_t *d = &s_drive[i];
    int y = a.y + 1;
    if (d->start_utc) {
        /* the day, big: "NOV 24" */
        char w[32];
        when_text(w, sizeof(w), d->start_utc);              /* "TUE NOV 24  8:57 PM" */
        snprintf(t, sizeof(t), "%.3s %.2s", w + 4, w + 8);
        if (t[4] == ' ') memmove(t + 4, t + 5, strlen(t + 5) + 1);
        ls_calls_big_text(sf, a, a.x + 2, y, t, TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
        char w2[32] = "";
        if (d->secs) when_text(w2, sizeof(w2), d->start_utc + d->secs);
        snprintf(t, sizeof(t), "%.3s %s%s%s", w, w + 11, w2[0] ? "  to " : "", w2[0] ? w2 + 11 : "");
        tui_put_str(sf, a, a.x + 2, y + 4, t, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
        when_zone(t, sizeof(t), d->start_utc);
        tui_put_str(sf, a, a.x + a.w - 2 - (int)strlen(t), y + 4, t, LS_ATTR_FAINT);
    }
    y += 6;
    if (y + 2 >= cy) return;
    snprintf(t, sizeof(t), "%s%s", d->secs ? "" : "length unknown", "");
    if (d->secs) {
        if (d->secs < 3600) snprintf(t, sizeof(t), "%u min", (unsigned)((d->secs + 30) / 60));
        else snprintf(t, sizeof(t), "%u h %02u min", (unsigned)(d->secs / 3600), (unsigned)(d->secs / 60 % 60));
    }
    char k[24] = "";
    if (d->km > 0) snprintf(k, sizeof(k), "   %.1f km", (double)d->km);
    char f[40] = "";
    if (d->finds) snprintf(f, sizeof(f), "   %u found%s", d->finds, d->targets ? "" : "");
    char line[96];
    snprintf(line, sizeof(line), "%s%s%s", t, k, f);
    tui_put_str(sf, a, a.x + 2, y, line, TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
    if (d->targets) {
        snprintf(t, sizeof(t), " %u TARGETS ", d->targets);
        tui_put_str(sf, a, a.x + a.w - 2 - (int)strlen(t), y, t, TUI_ATTR(TUI_BLACK, TUI_RED | TUI_BRIGHT));
    }
    snprintf(line, sizeof(line), "%s/  %s%s", s_logs[i], d->track ? "track" : "no track", d->log ? " + log" : "");
    tui_put_str(sf, a, a.x + 2, y + 1, line, LS_ATTR_FAINT);
}

static void drive_detail(const wd_drive_info_t *d, char *det, size_t n)
{
    char len[8], km[8], fd[8], sz[8];
    if (!d->summary && !d->secs) snprintf(len, sizeof(len), "..");
    else if (!d->secs) snprintf(len, sizeof(len), "--");
    else if (d->secs < 3600) snprintf(len, sizeof(len), "%um", (unsigned)((d->secs + 30) / 60));
    else snprintf(len, sizeof(len), "%uh%02u", (unsigned)(d->secs / 3600), (unsigned)(d->secs / 60 % 60));
    if (d->km <= 0) snprintf(km, sizeof(km), d->summary ? "--" : "..");
    else snprintf(km, sizeof(km), d->km < 10 ? "%.1f" : "%.0f", (double)d->km);
    if (d->finds) snprintf(fd, sizeof(fd), "%u", d->finds);
    else snprintf(fd, sizeof(fd), d->summary ? "--" : "..");
    if (d->bytes < 1024 * 1024) snprintf(sz, sizeof(sz), "%ldK", (d->bytes + 1023) / 1024);
    else snprintf(sz, sizeof(sz), "%.1fM", (double)d->bytes / (1024.0 * 1024.0));
    snprintf(det, n, "%4s %6s %5s %4s", len, km, fd, sz);
}

/* Old drives without a drive.txt: summed a slice a frame while the list is
   open ("..." until then), so a 4 MB log never stalls the screen. */
static int s_sum_at = -1;
static void replay_summarize_step(void)
{
    if (s_sum_at < 0) return;
    if (!ls_picker_is(replay_picked)) { s_sum_at = -1; wd_drive_summarize(NULL, 0, NULL); return; }   /* its file closes */
    while (s_sum_at < s_nlogs && s_drive[s_sum_at].summary) s_sum_at++;
    if (s_sum_at >= s_nlogs) { s_sum_at = -1; return; }
    wd_drive_info_t d;
    if (wd_drive_summarize(s_logs[s_sum_at], 48 * 1024, &d) != 1) return;
    s_drive[s_sum_at] = d;
    char det[LS_PICKER_DETAIL];
    drive_detail(&d, det, sizeof(det));
    ls_picker_set_detail(s_sum_at, det);
    s_sum_at++;
}

static void replay_pick(void)
{
    if (wd_stats()->running && !wd_stats()->replay) { say_now("STOP the drive first, then replay"); return; }
    s_nlogs = wd_replay_list(s_logs, s_drive, WD_REPLAY_LIST);
    ls_picker_open("REPLAY A DRIVE", replay_picked);
    ls_picker_header(replay_header);
    if (!s_nlogs) ls_picker_empty_reason(wd_feed_card() ? "No saved drives in /sdcard/wardrive yet" : "No card in the board");
    for (int i = 0; i < s_nlogs; i++) {
        char when[32], det[LS_PICKER_DETAIL];
        when_text(when, sizeof(when), s_drive[i].start_utc);
        drive_detail(&s_drive[i], det, sizeof(det));
        ls_picker_add(when, det);
    }
    s_sum_at = 0;
}

/* ------------------------------------------------------------ MAP layer */

/* The drive on MAP, drawn into its ink canvas with the RF SURVEY heat: the
   route as a yellow line, each network a dot in its colour. */
static void map_layer(const ls_rfmap_frame_t *f)
{
    wd_lock();
    int px = 0, py = 0;
    bool have = false;
    const int nt = wd_trail_count();
    for (int i = 0; i < nt; i++) {
        const wd_pt_t *p = wd_trail_at(i);
        int x, y;
        if (!f->dot(p->lat, p->lon, &x, &y)) { have = false; continue; }
        if (have) ls_ink_line(px, py, x, y, TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK), 3, 0);
        px = x; py = y; have = true;
    }
    const int nd = wd_dev_count();
    for (int i = 0; i < nd; i++) {
        const wd_dev_t *e = wd_dev_at(i);
        int x, y;
        if (!(e->flags & WD_F_PLACED) || !f->dot(e->lat, e->lon, &x, &y)) continue;
        const int sec = e->kind == WD_WIFI ? wd_sec_class(e->auth) : 0;
        ls_ink_dot(x, y, TUI_ATTR(kind_hue(e->kind, sec) | TUI_BRIGHT, TUI_BLACK), e->kind == WD_DRONE ? 5 : 2);
    }
    wd_unlock();
}

/* MAP with the drive on it, framed so the whole drive is in view: the
   route's and the finds' extent, at the zoom where it fills most of the
   screen. With nothing placed yet there is nothing to show, and it says so
   here instead of opening a map with nothing of the drive on it. */
static void open_map(void)
{
    const ls_app_t *m = ls_app_by_id("map");
    if (!m || !m->screen) return;
    double lo_lat = 90, hi_lat = -90, lo_lon = 180, hi_lon = -180;
    int n = 0;
    wd_lock();
    for (int i = 0; i < wd_trail_count(); i++) {
        const wd_pt_t *p = wd_trail_at(i);
        lo_lat = fmin(lo_lat, p->lat); hi_lat = fmax(hi_lat, p->lat);
        lo_lon = fmin(lo_lon, p->lon); hi_lon = fmax(hi_lon, p->lon); n++;
    }
    for (int i = 0; i < wd_dev_count(); i++) {
        const wd_dev_t *e = wd_dev_at(i);
        if (!(e->flags & WD_F_PLACED)) continue;
        lo_lat = fmin(lo_lat, e->lat); hi_lat = fmax(hi_lat, e->lat);
        lo_lon = fmin(lo_lon, e->lon); hi_lon = fmax(hi_lon, e->lon); n++;
    }
    const bool fix = st.origin && st.fix_ms && st.fix.fix;
    wd_unlock();
    if (!n && !fix) {
        say_now(st.running ? "MAP: nothing placed yet, waiting for a GPS fix" : "MAP: START a drive or REPLAY one first");
        return;
    }
    if (!n) { lo_lat = hi_lat = st.fix.lat; lo_lon = hi_lon = st.fix.lon; }
    const double lat = (lo_lat + hi_lat) / 2, lon = (lo_lon + hi_lon) / 2;
    /* metres across, then the zoom whose 256-dot tiles put that in ~250 dots */
    const double span = fmax(fmax((hi_lat - lo_lat) * 111320.0, (hi_lon - lo_lon) * 111320.0 * cos(lat * M_PI / 180)), 150.0);
    int z = (int)floor(log2(156543.0 * cos(lat * M_PI / 180) * 250.0 / span));
    z = z < 3 ? 3 : z > 17 ? 17 : z;
    char label[40];
    wd_replay_info_t ri;
    wd_replay_info(&ri);
    if (ri.on) snprintf(label, sizeof(label), "REPLAY %.18s  %u found", ri.name, st.total[0] + st.total[1] + st.total[2] + st.total[3]);
    else snprintf(label, sizeof(label), "WARDRIVE %.1f %s  %u found", (double)dist_shown(st.dist_m), dist_unit(),
                  st.total[0] + st.total[1] + st.total[2] + st.total[3]);
    ls_rfmap_set_extra(map_layer);
    ls_rfmap_set_extra_label(label);
    ls_map_follow_set(false);
    ls_map_center(lat, lon);
    ls_map_zoom_by(z - ls_map_zoom());
    ls_tui_screen_show(ls_tui_screen_index_of(m->screen));
}

/* ------------------------------------------------------------ LIVE */

/* The app's own picture: a car with an aerial, its arcs one step per scan. */
static void car_art(tui_surface *sf, tui_rect a, int x, int y, unsigned arcs, bool on)
{
    static const char *const CAR[] = {
        "           |   )  )  )",
        "      _____|___________",
        "     /  __    __    __ \\____",
        "    |  |  |  |  |  |  |  [] \\",
        "    |__|__|__|__|__|__|______|",
        "  ====(@)============(@)=====",
        "   .  :  .  :  .  :  .  :  .",
    };
    for (int r = 0; r < 7; r++)
        for (int c = 0; CAR[r][c]; c++) {
            const char ch = CAR[r][c];
            if (ch == ' ') continue;
            uint8_t at = LIT(TUI_YELLOW);
            if (r == 0 && ch == ')') {
                const int n = (c - 15) / 3;            /* which arc, inner first */
                at = on && (unsigned)n < arcs ? LIT(TUI_CYAN) : a_dim();
            } else if (ch == '|' && r == 0) at = LIT(TUI_WHITE);
            else if (ch == '@' || ch == '(' || ch == ')') at = LIT(TUI_WHITE);
            else if (ch == '=' || r == 6) at = a_rule();
            else if (ch == '[' || ch == ']') at = LIT(TUI_CYAN);
            tui_put_char(sf, a, x + c, y + r, ch, at);
        }
}

/* What is on the card, counted when the screen opens and after a drive ends
   (not per frame: a directory read is I/O). Names are the session stems. */
static struct { int logs, tracks; uint32_t stop_ms; bool known; char newest[28]; } s_card;

static void card_scan(void)
{
    memset(&s_card, 0, sizeof(s_card));
    s_card.known = true;
    s_card.stop_ms = st.stop_ms;
    /* drives, each in its folder (old loose ones moved in); the newest by when */
    (void)wd_drives_tidy();
    static char stems[WD_REPLAY_LIST][WD_STEM];
    const int n = wd_drives(stems, WD_REPLAY_LIST);
    s_card.logs = n;
    if (n) {
        wd_drive_info_t d;
        wd_drive_info(stems[0], &d);
        s_card.tracks = d.track;
        when_text(s_card.newest, sizeof(s_card.newest), d.start_utc);
    }
}

/* The empty middle of the idle page: what the GPS has right now (its own
   numbers, no animation) and what the card holds. */
static void idle_extras(tui_surface *sf, tui_rect a, int *py)
{
    int y = *py;
    const int bottom = a.y + a.h - 1;
    ls_location_t loc;
    ls_location_get(&loc);
    if (y + 3 <= bottom) {
        y++;
        tui_hline(sf, a, a.x + 1, y++, a.w - 2, '-', a_rule());
        put(sf, a, a.x + 2, y, "SKY", a_strong());
        const int cells = 12, lit = loc.sats > cells ? cells : loc.sats;
        char bar[16];
        for (int i = 0; i < cells; i++) bar[i] = i < lit ? '|' : '.';
        bar[cells] = 0;
        put(sf, a, a.x + 18, y, bar, loc.fix ? LIT(TUI_GREEN) : a_dim());
        char t[40];
        if (loc.fix) snprintf(t, sizeof(t), " %u sats  %s", (unsigned)loc.sats, loc.dim == 3 ? "3D" : "2D");
        else snprintf(t, sizeof(t), " %s", loc.state == LS_LOC_NO_GPS ? "no receiver" : loc.state == LS_LOC_OFF ? "GPS off" : "searching");
        put(sf, a, a.x + 18 + cells, y++, t, a_text());
        if (loc.fix && y <= bottom) {
            snprintf(t, sizeof(t), "HDOP %.1f  alt %.0f m", (double)loc.hdop, (double)loc.alt_m);
            put(sf, a, a.x + 18, y++, t, a_dim());
        }
    }
    if (wd_feed_card() && y + 2 <= bottom) {
        if (!s_card.known || s_card.stop_ms != st.stop_ms) card_scan();
        char t[48];
        snprintf(t, sizeof(t), "%d drive%s, each in its folder", s_card.logs, s_card.logs == 1 ? "" : "s");
        put(sf, a, a.x + 2, y, "FILES", a_strong());
        put(sf, a, a.x + 18, y++, t, a_text());
        if (s_card.newest[0] && y <= bottom) {
            put(sf, a, a.x + 2, y, "NEWEST", a_strong());
            put(sf, a, a.x + 18, y++, s_card.newest, a_accent());
        }
        if (s_card.logs > 0 && y <= bottom) {
            static const char chip[] = " REPLAY A DRIVE IN 3D (D) ";
            put(sf, a, a.x + 18, y, chip, ls_ink(LS_INK_KEY, TUI_ATTR(TUI_BLACK, TUI_CYAN)));
            s_replay_chip = tui_rect_make(a.x + 18, y, (int)sizeof(chip) - 1, 1);
            y++;
        }
    }
    *py = y;
}

/* The last drive's counts as bars against its biggest one. */
static void last_bars(tui_surface *sf, tui_rect a, int *py)
{
    unsigned top = 1;
    for (int k = 0; k < WD_KINDS; k++) if (st.total[k] > top) top = st.total[k];
    const int w = a.w - 24 > 20 ? 20 : a.w - 24;
    if (w < 4) return;
    for (int k = 0; k < WD_KINDS && *py <= a.y + a.h - 1; k++) {
        if (!st.total[k]) continue;
        char t[16];
        const int n = (int)((unsigned long long)st.total[k] * w / top);
        put(sf, a, a.x + 2, *py, wd_kind_short(k), a_dim());
        for (int i = 0; i < w; i++)
            tui_put_char(sf, a, a.x + 9 + i, *py, i < n ? '=' : '.', i < n ? LIT(kind_hue(k, 3)) : a_rule());
        snprintf(t, sizeof(t), "%u", st.total[k]);
        put(sf, a, a.x + 10 + w, (*py)++, t, a_text());
    }
}

/* How a drive goes, drawn once in the room that is left. Static: it says
   where each key leads, it does not move. */
static void idle_flow(tui_surface *sf, tui_rect a, int *py)
{
    static const char *const STEP[4] = { "START", "DRIVE", "STOP", "OPTIONS" };
    static const char *const NOTE[4] = { "scans", "logs", "GPX+CSV", "to locker" };
    static const uint8_t HUE[4] = { TUI_GREEN, TUI_CYAN, TUI_RED, TUI_YELLOW };
    const int bw = 9, gap = 3;
    if (a.w < 4 * bw + 3 * gap + 2 || a.y + a.h - 1 - *py < 8) return;
    int y = *py + 1;
    tui_hline(sf, a, a.x + 1, y++, a.w - 2, '-', a_rule());
    const int x0 = a.x + (a.w - (4 * bw + 3 * gap)) / 2;
    y++;
    for (int i = 0; i < 4; i++) {
        const int x = x0 + i * (bw + gap);
        const uint8_t at = LIT(HUE[i]);
        char top[12];
        memset(top, '-', bw); top[0] = top[bw - 1] = '+'; top[bw] = 0;
        put(sf, a, x, y, top, at);
        put(sf, a, x, y + 2, top, at);
        tui_put_char(sf, a, x, y + 1, '|', at);
        tui_put_char(sf, a, x + bw - 1, y + 1, '|', at);
        const int n = (int)strlen(STEP[i]);
        put(sf, a, x + (bw - n) / 2, y + 1, STEP[i], a_strong());
        const int m = (int)strlen(NOTE[i]);
        put(sf, a, x + (bw - m) / 2 > a.x ? x + (bw - m) / 2 : x, y + 3, NOTE[i], a_dim());
        if (i < 3) put(sf, a, x + bw, y + 1, "-->", a_rule());
    }
    y += 5;
    put(sf, a, a.x + 2, y++, "LIVE the dash  3D the ribbon  LIST each address", a_dim());
    put(sf, a, a.x + 2, y++, "MAP draws the drive over the survey layer", a_dim());
    *py = y;
}

static void idle_page(tui_surface *sf, tui_rect a)
{
    int y = a.y + 1;
    const bool wide = a.w >= 92;
    if (ls_tui_compact()) {
        /* A desk window: the car on black glass, the particulars in a white well. */
        const tui_rect g = ls_dui_glass(sf, wide ? tui_rect_make(a.x, a.y, 36, a.h) : tui_rect_make(a.x + (a.w - 36) / 2, a.y, 36, 10));
        car_art(sf, g, g.x + 2, g.y + (wide ? g.h / 3 - 3 : 1), 3, false);
        a = ls_dui_well(sf, wide ? tui_rect_make(a.x + 37, a.y, a.w - 37, a.h) : tui_rect_make(a.x, a.y + 10, a.w, a.h - 10), NULL);
        y = a.y;
    } else {
    const int ax = wide ? a.x + 3 : a.x + (a.w - 30) / 2;
    car_art(sf, a, ax > a.x ? ax : a.x, wide ? a.y + 3 : y, 3, false);
    if (wide) a = tui_rect_make(a.x + 38, a.y, a.w - 38, a.h);
    else y += 8;
    }
    put(sf, a, a.x + 2, y++, "Wi-Fi, Bluetooth, cells and drones, each", a_text());
    put(sf, a, a.x + 2, y++, "logged with where you were. One START.", a_text());
    y++;
    static const char *const WHAT[WD_KINDS] = { "networks, BSSID, security", "devices advertising",
                                               "LTE cells (CELL WATCH search)", "Remote ID + DRONES watch",
                                               "TPMS, sensors, remotes", "aircraft, ADS-B", "Meshtastic, MeshCore nodes" };
    for (int k = 0; k < WD_KINDS; k++) {
        const bool has = wd_feed_has(k), on = s_src[k] && has;
        char line[80];
        snprintf(line, sizeof(line), "[%c] %-10s", on ? 'x' : ' ', wd_kind_name(k));
        put(sf, a, a.x + 2, y, line, on ? LIT(kind_hue(k, 3)) : a_dim());
        put(sf, a, a.x + 18, y++, has ? WHAT[k] : "not on this board", on ? a_text() : a_dim());
    }
    char key[32];
    const ls_loc_state_t ls = ls_location_key(key, sizeof(key));
    put(sf, a, a.x + 2, ++y, "GPS", a_strong());
    put(sf, a, a.x + 18, y++, key, ls == LS_LOC_FIX ? LIT(TUI_GREEN) : ls == LS_LOC_NO_FIX ? LIT(TUI_YELLOW) : LIT(TUI_RED));
    put(sf, a, a.x + 2, y, "CARD", a_strong());
    put(sf, a, a.x + 18, y++, wd_feed_card() ? "/sdcard/wardrive" : "no card: nothing is saved", wd_feed_card() ? a_text() : LIT(TUI_YELLOW));
    {
        /* HOME: the home base's link (ls_lsx), when the field role runs */
        ls_lsx_home_status_t h;
        ls_lsx_home_status(&h);
        char t[64];
        uint8_t at = a_dim();
        if (!h.on) snprintf(t, sizeof(t), "home sync off ('lsx field on')");
        else if (h.syncing) { snprintf(t, sizeof(t), "%.12s syncing %d%%  %s", h.home, h.progress_pct, h.mode ? h.mode : ""); at = LIT(TUI_CYAN); }
        else if (h.in_range) { snprintf(t, sizeof(t), "%.12s in range %d dBm", h.home, h.rssi); at = LIT(TUI_GREEN); }
        else if (h.last_sync_ago_s != UINT32_MAX) {
            const uint32_t m = h.last_sync_ago_s / 60;
            if (m < 120) snprintf(t, sizeof(t), "last sync %lu min ago", (unsigned long)m);
            else snprintf(t, sizeof(t), "last sync %lu h ago", (unsigned long)(m / 60));
            at = a_text();
        } else snprintf(t, sizeof(t), "out of range, never synced");
        put(sf, a, a.x + 2, y, "HOME", a_strong());
        put(sf, a, a.x + 18, y++, t, at);
    }
    idle_extras(sf, a, &y);
    if (st.ever) {
        char t[16], line[96];
        y++;
        tui_hline(sf, a, a.x + 1, y++, a.w - 2, '-', a_rule());
        clock_text(t, sizeof(t), st.stop_ms - st.start_ms);
        snprintf(line, sizeof(line), "LAST DRIVE  %s  %.1f %s", t, (double)dist_shown(st.dist_m), dist_unit());
        put(sf, a, a.x + 2, y++, line, a_strong());
        snprintf(line, sizeof(line), "%u Wi-Fi  %u BT  %u cells  %u drones  %u rows",
                 st.total[WD_WIFI], st.total[WD_BLE], st.total[WD_CELL], st.total[WD_DRONE], st.rows);
        put(sf, a, a.x + 2, y++, line, a_text());
        for (int f = 0; f < 2; f++) {
            const char *p = f ? st.gpx : st.csv, *b = strrchr(p, '/');
            if (!p[0]) continue;
            snprintf(line, sizeof(line), "%s %s", f ? "TRACK" : "LOG  ", b ? b + 1 : p);
            put(sf, a, a.x + 2, y++, line, a_accent());
        }
        if (st.note[0]) put(sf, a, a.x + 2, y++, st.note, a_dim());
        last_bars(sf, a, &y);
    }
    idle_flow(sf, a, &y);
}

/* REC, the time, the arcs, GPS and the card on one row. */
static void band(tui_surface *sf, tui_rect a, int y)
{
    char t[16], key[32];
    int x = a.x + 1;
    tui_fill(sf, tui_rect_make(a.x, y, a.w, 1), ' ', TUI_ATTR(TUI_WHITE, G));
    if (st.replay) { put(sf, a, x, y, " REPLAY ", ls_ink(LS_INK_KEY_ON, TUI_ATTR(TUI_BLACK, TUI_CYAN | TUI_BRIGHT))); x += 9; }
    else { put(sf, a, x, y, " REC ", ls_ink(LS_INK_KEY_ON, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_RED))); x += 6; }
    clock_text(t, sizeof(t), now_ms() - st.start_ms);
    put(sf, a, x, y, t, a_strong());
    x += (int)strlen(t) + 1;
    for (unsigned k = 0; k < 3; k++) tui_put_char(sf, a, x + (int)k, y, ')', k < 1 + st.scans % 3 ? LIT(TUI_CYAN) : a_dim());
    x += 5;
    const ls_loc_state_t ls = ls_location_key(key, sizeof(key));
    if (st.wifi_fix) {
        /* the known access points are steering, the GPS is not */
        snprintf(key, sizeof(key), "WIFI FIX +-%.0fm (%u)", (double)st.wifi_err_m, (unsigned)st.wifi_used);
        put(sf, a, x, y, key, LIT(TUI_CYAN));
    } else if (ls == LS_LOC_FIX && st.fix_weak) {
        /* a position, not trusted yet: the track and the finds wait for it */
        snprintf(key, sizeof(key), st.settle ? "GPS SETTLING %u/3" : "GPS WEAK", (unsigned)st.settle);
        put(sf, a, x, y, key, LIT(TUI_YELLOW));
    } else put(sf, a, x, y, key, ls == LS_LOC_FIX ? LIT(TUI_GREEN) : ls == LS_LOC_NO_FIX ? LIT(TUI_YELLOW) : LIT(TUI_RED));
    x += (int)strlen(key) + 2;
    const char *card = st.card > 0 ? (st.csv[0] ? "LOG" : "CARD") : st.card == 0 ? "NO CARD" : "CARD ERR";
    if (x + (int)strlen(card) < a.x + a.w) put(sf, a, a.x + a.w - 1 - (int)strlen(card), y, card, st.card > 0 ? a_dim() : LIT(TUI_YELLOW));
}

/* A seven-segment figure in braille dots, `k` rows to a half: lit segments in
   `at`, the rest faint behind them, the way a VFD shows its unlit ones. */
static void segments(tui_surface *sf, tui_rect a, int x, int y, int k, char c, uint8_t at)
{
    static const uint8_t SEG[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    const uint8_t on = c >= '0' && c <= '9' ? SEG[c - '0'] : c == '-' ? 0x40 : 0;
    const uint8_t ghost = LS_ATTR_FAINT;
    /* A lit segment is two dots thick, an unlit one a single row of dots. */
#define SEG_PUT(px, py, bit, lit, unlit) ls_braille_put(sf, a, (px), (py), (on & (bit)) ? (lit) : (unlit), (on & (bit)) ? at : ghost)
    for (int i = 1; i <= k + 1; i++) {
        SEG_PUT(x + i, y, 0x01, 0x281B, 0x2809);              /* top    */
        SEG_PUT(x + i, y + k, 0x40, 0x2836, 0x2812);          /* middle */
        SEG_PUT(x + i, y + 2 * k, 0x08, 0x28E4, 0x28C0);      /* bottom */
    }
    for (int r = 1; r <= k; r++) {
        SEG_PUT(x, y + r, 0x20, 0x28B8 | 0x47, 0x28B8);       /* left, upper  */
        SEG_PUT(x + k + 2, y + r, 0x02, 0x2847 | 0xB8, 0x2847);
        SEG_PUT(x, y + k + r, 0x10, 0x28B8 | 0x47, 0x28B8);   /* left, lower  */
        SEG_PUT(x + k + 2, y + k + r, 0x04, 0x2847 | 0xB8, 0x2847);
    }
#undef SEG_PUT
}

/* Speed in big stroke digits (twice the size where there is room), and the
   drive's distance, average and fix beside them. Returns the rows used. */
static int speed_block(tui_surface *sf, tui_rect a, int x, int y, bool big)
{
    char s[8];
    const bool fix = st.fix.fix && st.fix_ms;
    if (fix) snprintf(s, sizeof(s), "%3.0f", (double)speed_shown(st.speed));
    else snprintf(s, sizeof(s), " --");
    const int k = big ? 4 : 2, gw = k + 4, gh = 2 * k + 1;
    int cx = x;
    if (ls_tui_compact()) {     /* a desk window: the speed on a VFD */
        ls_dui_vfd(sf, tui_rect_make(x, y, 3 * gw, gh), s, LS_DUI_VFD_GREEN);
        cx += 3 * gw;
    } else
    for (const char *c = s; *c; c++, cx += gw)
        segments(sf, a, cx, y, k, *c, fix ? LIT(TUI_WHITE) : a_dim());
    put(sf, a, cx, y + gh - 1, speed_unit(), a_dim());
    const int tx = cx + 6;
    char d[40];
    snprintf(d, sizeof(d), "%.2f %s", (double)dist_shown(st.dist_m), dist_unit());
    put(sf, a, tx, y, "DISTANCE", a_dim());
    put(sf, a, tx, y + 1, d, a_strong());
    const uint32_t el = now_ms() - st.start_ms;
    snprintf(d, sizeof(d), "%.0f %s", el > 60000 ? (double)speed_shown(st.dist_m * 1000.0f / (float)el) : 0.0, speed_unit());
    put(sf, a, tx, y + (big ? 3 : 2), "AVERAGE", a_dim());
    put(sf, a, tx, y + (big ? 4 : 3), d, a_text());
    if (big) {
        put(sf, a, tx, y + 6, "FIX", a_dim());
        snprintf(d, sizeof(d), "%u sats  %.1f", (unsigned)st.fix.sats, (double)st.fix.hdop);
        put(sf, a, tx, y + 7, fix ? d : "no fix", fix ? a_text() : LIT(TUI_YELLOW));
        snprintf(d, sizeof(d), "%u rows", st.rows);
        put(sf, a, tx, y + 9, d, st.rows ? a_accent() : a_dim());
    } else {
        snprintf(d, sizeof(d), "%u sats  %u rows", (unsigned)st.fix.sats, st.rows);
        put(sf, a, tx, y + 4, fix ? d : "no fix", fix ? a_dim() : LIT(TUI_YELLOW));
    }
    return gh;
}

/* A strip of the compass, 120 degrees wide, centred on the heading. */
static void tape(tui_surface *sf, tui_rect a, int x, int y, int w)
{
    static const char *const P[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    const float h = st.heading;
    if (h < 0) { put(sf, a, x, y, "heading --  (moves with the fix or the compass)", a_dim()); return; }
    const float span = 120.0f;
    char row[160];
    if (w > (int)sizeof(row) - 1) w = (int)sizeof(row) - 1;
    for (int i = 0; i < w; i++) row[i] = ' ';
    row[w] = 0;
    for (int i = 0; i < w; i++) {
        const float d0 = h + (i - w / 2) * span / w, d1 = h + (i + 1 - w / 2) * span / w;
        for (int m = (int)ceilf(d0 / 15.0f) * 15; m < d1; m += 15) {
            const int deg = ((m % 360) + 360) % 360;
            row[i] = deg % 45 ? '.' : ':';
            if (deg % 45 == 0) {
                const char *p = P[deg / 45];
                const int l = (int)strlen(p), at = i - l / 2;
                for (int k = 0; k < l; k++) if (at + k >= 0 && at + k < w) row[at + k] = p[k];
            }
        }
    }
    for (int i = 0; i < w; i++) {
        const char c = row[i];
        tui_put_char(sf, a, x + i, y, c, c == 'N' && (i == 0 || row[i - 1] != 'S') && (i + 1 >= w || (row[i + 1] != 'E' && row[i + 1] != 'W')) ?
                     LIT(TUI_RED) : (c >= 'A' && c <= 'Z') ? LIT(TUI_YELLOW) : a_rule());
    }
    char t[16];
    snprintf(t, sizeof(t), "^ %03.0f", (double)h);
    put(sf, a, x + w / 2, y + 1, t, a_strong());
}

static void counters(tui_surface *sf, tui_rect a, int x, int y, int w, uint32_t now)
{
    char s[80];
    snprintf(s, sizeof(s), "WI-FI %u", st.total[WD_WIFI]);
    put(sf, a, x, y, s, a_strong());
    unsigned per_min = 0;
    wd_lock();
    per_min = wd_new_last_minute(now);
    unsigned hist[30], top = 1;
    for (int i = 0; i < 30; i++) { hist[i] = wd_new_in_minute(now, 29 - i); if (hist[i] > top) top = hist[i]; }
    wd_unlock();
    snprintf(s, sizeof(s), "+%u/min", per_min);
    put(sf, a, x + 12, y, s, per_min ? LIT(TUI_GREEN) : a_dim());
    /* New finds a minute, the last half hour, as braille columns. */
    const int sx = x + 22, cells = w - 23 < 15 ? w - 23 : 15;
    for (int c = 0; c < cells; c++) {
        uint8_t bits = 0;
        for (int half = 0; half < 2; half++) {
            const unsigned v = hist[30 - cells * 2 + c * 2 + half];
            bits |= v ? ls_braille_column(1 + (v * 3 + top - 1) / top, half) : ls_braille_dot(0, half);
        }
        ls_braille_put(sf, a, sx + c, y, (int16_t)(0x2800 | bits), INK(TUI_GREEN));
    }
    const unsigned total = st.total[WD_WIFI] ? st.total[WD_WIFI] : 1;
    const int bw = w - 13 > 30 ? 30 : w - 13;
    for (int k = 0; k < WD_SECS; k++) {
        const int yy = y + 1 + k;
        put(sf, a, x + 1, yy, sec_word(k), LIT(kind_hue(WD_WIFI, k)));
        const int lit = (int)ceilf((float)st.sec[k] * bw / total);
        for (int i = 0; i < bw; i++)
            tui_put_char(sf, a, x + 6 + i, yy, i < lit ? '=' : '-', i < lit ? INK(kind_hue(WD_WIFI, k)) : a_rule());
        snprintf(s, sizeof(s), "%5u", st.sec[k]);
        put(sf, a, x + 6 + bw + 1, yy, s, st.sec[k] ? a_text() : a_dim());
    }
    int cx = x;
    const int yy = y + 1 + WD_SECS;
    for (int k = WD_BLE; k < WD_KINDS; k++) {
        if (k > WD_DRONE && !st.total[k] && !st.src[k]) continue;
        snprintf(s, sizeof(s), "%s %u", wd_kind_short(k), st.total[k]);
        if (cx + (int)strlen(s) > x + w) break;        /* stay inside the plate */
        put(sf, a, cx, yy, s, st.src[k] ? LIT(kind_hue(k, 0)) : a_dim());
        cx += (int)strlen(s) + 3;
    }
}

/* New names fall down their lane from the moment they are found, the last
   letter leading, and are gone when they land. */
static void rain(tui_surface *sf, tui_rect r, uint32_t now)
{
    if (r.w < 4 || r.h < 4) return;
    ls_ink_keep(r);
    tui_fill(sf, r, ' ', TUI_ATTR(TUI_WHITE, G));
    for (int x = 0; x < r.w; x++) tui_put_char(sf, r, r.x + x, r.y + r.h - 1, x % 4 ? '_' : '.', a_rule());
    int falling = 0;
    for (int i = ndrops - 1; i >= 0; i--) {             /* oldest first: newer ones draw on top */
        const wd_drop_t *d = &drops[i];
        const uint32_t age = now - d->ms;
        if (age >= FALL_MS + 400) continue;
        const int lane = (int)((d->lane * 37u + (unsigned)d->ms / 7u) % (unsigned)r.w);
        const int bottom = r.h - 2;
        const uint8_t hue = (d->tags & WD_HIT_TAGS) ? TUI_RED : kind_hue(d->kind, d->sec);   /* a target in red */
        if (age >= FALL_MS) {                          /* landed: a splash, then nothing */
            tui_put_char(sf, r, r.x + lane, r.y + r.h - 1, age < FALL_MS + 200 ? '*' : 'o', LIT(hue));
            if (lane > 0) tui_put_char(sf, r, r.x + lane - 1, r.y + r.h - 1, '\\', INK(hue));
            if (lane + 1 < r.w) tui_put_char(sf, r, r.x + lane + 1, r.y + r.h - 1, '/', INK(hue));
            continue;
        }
        falling++;
        const char *name = d->name[0] ? d->name : d->kind == WD_WIFI ? "<hidden>" : d->kind == WD_BLE ? "BT" : "?";
        int len = (int)strlen(name);
        if (len > bottom) len = bottom;
        const float t = (float)age / FALL_MS;
        const int head = (int)(t * t * bottom);        /* falls faster as it goes */
        for (int j = 0; j < len; j++) {
            const int row = head - (len - 1 - j);
            if (row < 0 || row > bottom) continue;
            const unsigned char ch = (unsigned char)name[j];
            const uint8_t at = j == len - 1 ? LIT(TUI_WHITE) : j >= len - 4 ? LIT(hue) : INK(hue);
            tui_put_char(sf, r, r.x + lane, r.y + row, ch >= 32 && ch < 127 ? (char)ch : '?', at);
        }
    }
    if (!falling) {
        char w[48];
        if (!st.fix.fix) snprintf(w, sizeof(w), "waiting for a GPS fix");
        else if (!ndrops) snprintf(w, sizeof(w), "listening");
        else {
            const unsigned s = (now - drops[0].ms) / 1000;
            if (s < 60) snprintf(w, sizeof(w), "last find %lds ago", ls_age_q((long)s));
            else snprintf(w, sizeof(w), "last find %um ago", s / 60);
        }
        put(sf, r, r.x + (r.w - (int)strlen(w)) / 2, r.y + r.h / 2, w, a_dim());
    }
}

/* The newest finds, one a row. */
static void latest(tui_surface *sf, tui_rect r, uint32_t now)
{
    for (int i = 0; i < ndrops && i < r.h; i++) {
        const wd_drop_t *d = &drops[i];
        const int y = r.y + i;
        const uint8_t hue = (d->tags & WD_HIT_TAGS) ? TUI_RED : kind_hue(d->kind, d->sec);   /* a target in red */
        const bool fresh = now - d->ms < FALL_MS;
        tui_put_char(sf, r, r.x, y, fresh ? '+' : ' ', LIT(TUI_GREEN));
        tui_put_char(sf, r, r.x + 1, y, kind_glyph(d->kind), LIT(hue));
        char s[48];
        snprintf(s, sizeof(s), "%4d", d->rssi);
        put(sf, r, r.x + 3, y, s, a_text());
        meter(sf, r, r.x + 8, y, 2, d->rssi, INK(hue));
        put(sf, r, r.x + 11, y, d->kind == WD_WIFI ? sec_word(d->sec) : wd_kind_tag(d->kind), INK(hue));
        const char *name = d->name[0] ? d->name : d->kind == WD_WIFI ? "<hidden>" : "(no name)";
        snprintf(s, sizeof(s), "%.*s", r.w - 17 > 0 ? (r.w - 17 < 40 ? r.w - 17 : 40) : 0, name);
        put(sf, r, r.x + 16, y, s, fresh ? a_strong() : a_text());
        const unsigned age = (now - d->ms) / 1000;
        if (age < 60) snprintf(s, sizeof(s), "%us", age);
        else snprintf(s, sizeof(s), "%um", age / 60);
        if (r.w > 40) put(sf, r, r.x + r.w - (int)strlen(s), y, s, a_dim());
    }
}

static void live_page(tui_surface *sf, tui_rect a, uint32_t now)
{
    if (!st.running) {
        const tui_rect in = ls_plate(sf, a, a, TUI_YELLOW, "WARDRIVE", st.ever ? "LAST DRIVE" : "READY", st.ever ? TUI_CYAN : TUI_GREEN);
        idle_page(sf, tui_rect_make(in.x - 1, in.y, in.w + 2, in.h));
        return;
    }
    const bool wide = a.w >= 92 && a.h < 40;
    tui_rect left = a, right = a;
    if (wide) { left.w = 52; right.x = a.x + 53; right.w = a.w - 53; }
    /* A landscape camera hole at the right edge: the NEW panel stops one cell short of it. */
    tui_rect hole;
    if (wide && ls_tui_cutout(&hole) && hole.x > right.x && hole.x < right.x + right.w) right.w = hole.x - 1 - right.x;
    band(sf, a, a.y);
    int y = a.y + 1;
    /* DRIVE: speed, distance, the heading tape. */
    const bool big = !wide && a.h >= 52;
    const int gh = big ? 10 : 5;
    const tui_rect drive = tui_rect_make(left.x, y, left.w, gh + 5);
    ls_plate(sf, a, drive, TUI_YELLOW, "DRIVE", NULL, 0);
    speed_block(sf, drive, drive.x + 2, drive.y + 1, big);
    tape(sf, drive, drive.x + 2, drive.y + gh + 2, drive.w - 4);
    y += drive.h;
    /* FOUND: the counters. */
    const tui_rect found = tui_rect_make(left.x, y, left.w, WD_SECS + 4);
    ls_plate(sf, a, found, TUI_GREEN, "FOUND", NULL, 0);
    counters(sf, found, found.x + 2, found.y + 1, found.w - 4, now);
    y += found.h;
    if (st.card <= 0 && st.note[0]) put(sf, left, left.x + 2, y++, st.note, LIT(TUI_YELLOW));
    /* NEW: names raining down as they are found, and the newest listed. */
    char cap[32];
    snprintf(cap, sizeof(cap), "NEW  %u/min", wd_new_last_minute(now));
    const tui_rect np = wide ? tui_rect_make(right.x, right.y + 1, right.w, right.h - 1)
                             : tui_rect_make(a.x, y, a.w, a.y + a.h - y);
    ls_plate(sf, a, np, TUI_CYAN, cap, NULL, 0);
    const int inner = np.h - 2, lh = inner / 2 < 12 ? inner / 2 : 12;
    rain(sf, tui_rect_make(np.x + 1, np.y + 1, np.w - 2, inner - lh), now);
    if (ls_tui_compact()) {
        const tui_rect w = ls_dui_well(sf, tui_rect_make(np.x + 1, np.y + 1 + inner - lh, np.w - 2, lh), NULL);
        latest(sf, tui_rect_make(w.x + 1, w.y, w.w - 2, w.h), now);
    } else
    latest(sf, tui_rect_make(np.x + 2, np.y + 1 + inner - lh, np.w - 4, lh), now);
}

/* ------------------------------------------------------------ 3D */

static void page_3d(tui_surface *sf, tui_rect a, uint32_t now)
{
    if (ls_tui_compact()) a = ls_dui_glass(sf, a);   /* a desk window: the view on black glass */
    s_3d = a;
    {
        wd_replay_info_t ri;
        wd_replay_info(&ri);
        if (ri.loading) {
            char t[48];
            snprintf(t, sizeof(t), "%.27s  %d%%", ri.name, ri.read_pct);
            ls_panel_notice(sf, a, "REPLAY", "Reading the drive", t);
            return;
        }
    }
    wd_replay_info_t ri;
    wd_replay_info(&ri);
    /* the top row says where and what; the replay's bar and keys take the
       bottom two; the view and its keys get the rest */
    /* the top row says where, what and how fast; the view gets the rest */
    const tui_rect view = tui_rect_make(a.x, a.y + 1, a.w, a.h - 1);
    if (!wd3d_draw(sf, view, now)) {
        ls_panel_notice(sf, a, "3D", "Not enough memory for the 3D view", "Close other apps and try again");
        return;
    }
    char s[96];
    tui_fill(sf, tui_rect_make(a.x, a.y, a.w, 1), ' ', TUI_ATTR(TUI_WHITE, G));
    int x = a.x;
    if (ri.on) {
        snprintf(s, sizeof(s), " REPLAY %.14s ", ri.name);
        put(sf, a, x, a.y, s, TUI_ATTR(TUI_BLACK, TUI_CYAN | TUI_BRIGHT));
        x += (int)strlen(s) + 1;
    }
    /* the speed, live or as the replay drove it */
    if (st.fix.fix && st.fix_ms && st.speed >= 0) {
        snprintf(s, sizeof(s), "%.0f %s", (double)speed_shown(st.speed), speed_unit());
        put(sf, a, x, a.y, s, TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, G));
        x += (int)strlen(s) + 2;
        if (st.heading >= 0) {
            snprintf(s, sizeof(s), "%03.0f", (double)st.heading);
            put(sf, a, x, a.y, s, TUI_ATTR(TUI_WHITE, G));
            x += (int)strlen(s) + 2;
        }
    } else {
        put(sf, a, x, a.y, "NO FIX", TUI_ATTR(TUI_YELLOW, G));
        x += 8;
    }
    /* SHOW: what the pillars are, a tap steps it */
    char lab[16];
    const char *sh = wd3d_label(WD3D_SHOW, lab, sizeof(lab));
    snprintf(s, sizeof(s), " SHOW %s ", sh);
    const int cw = (int)strlen(s), cx = a.x + a.w - cw;
    put(sf, a, cx, a.y, s, ls_ink(LS_INK_KEY, TUI_ATTR(TUI_BLACK, wd3d_lit(WD3D_SHOW) ? TUI_CYAN | TUI_BRIGHT : TUI_WHITE)));
    s_show_chip = tui_rect_make(cx, a.y, cw, 1);
    snprintf(s, sizeof(s), "W%u B%u ", st.total[WD_WIFI], st.total[WD_BLE]);
    int right = cx;             /* where the top row's right-hand items begin */
    if (x + (int)strlen(s) <= cx) {
        right = cx - (int)strlen(s);
        put(sf, a, right, a.y, s, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, G));
    }
    if (!st.running) {
        /* A drive that would not open says why in the middle for a few
           seconds; after that, and whenever there is a scene to look at, the
           note and the REPLAY key live on the top row so nothing sits on the
           view. The middle is only used when the view is empty. */
        static char err_seen[48];
        static uint32_t err_ms;
        const char *err = wd_replay_error();
        if (!err || !*err) err_seen[0] = 0;
        else if (strncmp(err, err_seen, sizeof(err_seen) - 1) != 0) {
            snprintf(err_seen, sizeof(err_seen), "%s", err);
            err_ms = now;
        }
        const bool fresh_err = err_seen[0] && now - err_ms < 5000;
        const bool scene = st.ever || wd_trail_count() > 0;
        if (!scene || fresh_err) {
            char m[72];
            if (err_seen[0]) snprintf(m, sizeof(m), " %.20s: %.26s ", ri.name, err_seen);   /* fits the 50-cell view */
            else snprintf(m, sizeof(m), "%s", " START a drive to see it here ");
            put(sf, a, a.x + (a.w - (int)strlen(m)) / 2, a.y + a.h / 2 - 1, m, TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, G));
            static const char chip[] = " REPLAY A SAVED DRIVE (D) ";
            const int ccx = a.x + (a.w - (int)sizeof(chip) + 1) / 2;
            put(sf, a, ccx, a.y + a.h / 2 + 1, chip, ls_ink(LS_INK_KEY, TUI_ATTR(TUI_BLACK, TUI_CYAN)));
            s_replay_chip = tui_rect_make(ccx, a.y + a.h / 2 + 1, (int)sizeof(chip) - 1, 1);
        } else {
            /* top row: [D REPLAY] then ENDED or the reason, as room allows */
            static const char chip[] = " D REPLAY ";
            const int cl = (int)sizeof(chip) - 1;
            int room = right - x;
            if (room >= cl) {
                put(sf, a, x, a.y, chip, ls_ink(LS_INK_KEY, TUI_ATTR(TUI_BLACK, TUI_CYAN)));
                s_replay_chip = tui_rect_make(x, a.y, cl, 1);
                x += cl + 1;
                room = right - x - 1;
                char m[40];
                snprintf(m, sizeof(m), "%s", err_seen[0] ? err_seen : "ENDED");
                if (room > 2) {
                    if ((int)strlen(m) > room) m[room] = 0;
                    put(sf, a, x, a.y, m, TUI_ATTR(err_seen[0] ? TUI_RED | TUI_BRIGHT : TUI_YELLOW, G));
                }
            } else {
                s_replay_chip = tui_rect_make(0, 0, 0, 0);
            }
        }
    } else if (!st.origin) {
        const char *m = " waiting for a GPS fix ";
        put(sf, a, a.x + (a.w - (int)strlen(m)) / 2, a.y + a.h / 2, m, TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, G));
    }
}

/* ------------------------------------------------------------ LIST */

static bool wanted(const wd_dev_t *e)
{
    switch (s_filter) {
    case F_WIFI: return e->kind == WD_WIFI;
    case F_OPEN: return e->kind == WD_WIFI && wd_sec_class(e->auth) == WD_SEC_OPEN;
    case F_BLE: return e->kind == WD_BLE;
    case F_CELL: return e->kind == WD_CELL;
    case F_DRONE: return e->kind == WD_DRONE;
    case F_RF: return e->kind == WD_RF;
    case F_AIR: return e->kind == WD_AIR;
    case F_LORA: return e->kind == WD_LORA;
    case F_HITS: return (e->tags & WD_HIT_TAGS) != 0;
    default: return true;
    }
}

static int by_order(const void *pa, const void *pb)
{
    const wd_dev_t *a = wd_dev_at(*(const uint16_t *)pa), *b = wd_dev_at(*(const uint16_t *)pb);
    if (s_sort) return a->first_ms < b->first_ms ? 1 : a->first_ms > b->first_ms ? -1 : 0;
    return b->rssi - a->rssi;
}

static void order(uint32_t now)
{
    if (s_order_ms && now - s_order_ms < 700) return;
    s_order_ms = now ? now : 1;
    wd_lock();
    s_norder = 0;
    const int n = wd_dev_count();
    for (int i = 0; i < n; i++) if (wanted(wd_dev_at(i))) s_order[s_norder++] = (uint16_t)i;
    qsort(s_order, (size_t)s_norder, sizeof(s_order[0]), by_order);
    wd_unlock();
    /* Strongest first re-sorts as signals change: the selection stays on
       the network it was on, not on the row number. */
    if (s_sel_dev >= 0) {
        for (int i = 0; i < s_norder; i++)
            if (s_order[i] == s_sel_dev) {
                const int moved = i - s_sel;
                s_sel = i;
                s_top += moved;      /* and its row stays where it was on screen */
                if (s_top < 0) s_top = 0;
                break;
            }
    }
    if (s_sel >= s_norder) s_sel = s_norder ? s_norder - 1 : 0;
}

static void sel_set(int i)
{
    s_sel = i < 0 ? 0 : i >= s_norder ? (s_norder ? s_norder - 1 : 0) : i;
    s_sel_dev = s_norder ? s_order[s_sel] : -1;
}

static void list_rows(tui_surface *sf, tui_rect lr, uint32_t now, tui_rect dr);

static void list_page(tui_surface *sf, tui_rect a, uint32_t now)
{
    order(now);
    static const char *const CH[F_N] = { "ALL", "WI-FI", "OPEN", "BT", "CELL", "DRONE", "RF", "AIR", "LORA", "TARGETS" };
    int x = a.x + 1, cy = a.y;
    if (ls_tui_compact()) {
        /* A desk window: the kinds as tabs, the sort as a key beside them,
           the list in a white well and the picked one in a group box. */
        int16_t tx[F_N], tw[F_N];
        const tui_rect page = ls_dui_tabs(sf, a, CH, F_N, s_filter, tx, tw);
        for (int f = 0; f < F_N; f++) s_chip[f] = tui_rect_make(tx[f], a.y + 1, tx[f] >= 0 ? tw[f] : 0, 1);
        const ls_btn_t sk = {"SORT", s_sort ? "NEWEST" : "STRONGEST", 'n', false, false, false};
        const int kw = ls_key1_width(&sk);
        ls_key1_draw(sf, a, a.x + a.w - kw - 1, a.y + 1, &sk, false);
        s_chip[F_N] = tui_rect_make(a.x + a.w - kw - 1, a.y + 1, kw, 1);
        const tui_rect in = tui_rect_make(page.x + 1, page.y, page.w - 2, page.h);
        const tui_rect lw = ls_dui_well(sf, tui_rect_make(in.x, in.y, in.w, in.h - 5), NULL);
        const tui_rect dg = ls_dui_group(sf, tui_rect_make(in.x, in.y + in.h - 5, in.w, 5), "Selected");
        list_rows(sf, lw, now, tui_rect_make(dg.x + 1, dg.y, dg.w - 2, 3));
        return;
    }
    for (int f = 0; f < F_N; f++) {
        char s[16];
        snprintf(s, sizeof(s), " %s ", CH[f]);
        const int w = (int)strlen(s);
        if (x + w > a.x + a.w - 1) { x = a.x + 1; cy++; }       /* the chips wrap onto a second row */
        const uint8_t idle = f == F_HITS ? LIT(TUI_RED) : a_text();
        put(sf, a, x, cy, s, f == s_filter ? ls_ink(LS_INK_SELECT, TUI_ATTR(TUI_BLACK, f == F_HITS ? TUI_RED : TUI_CYAN)) : idle);
        s_chip[f] = tui_rect_make(x, cy, w, 1);
        x += w + 1;
    }
    const char *srt = s_sort ? " NEWEST " : " STRONGEST ";
    const int sw = (int)strlen(srt);
    const int sx = x + sw < a.x + a.w ? a.x + a.w - sw - 1 : x;
    const int sy = x + sw < a.x + a.w ? cy : cy + 1;
    put(sf, a, sx, sy, srt, ls_ink(LS_INK_KEY, TUI_ATTR(TUI_BLACK, TUI_WHITE)));
    s_chip[F_N] = tui_rect_make(sx, sy, sw, 1);
    const int top = sy + 2;
    const int detail = 3;
    list_rows(sf, tui_rect_make(a.x, top, a.w, a.y + a.h - top - detail - 1), now, tui_rect_make(a.x, a.y + a.h - detail, a.w, detail));
}

/* The rows in `lr`, the picked one's particulars in the three rows of `dr`. */
static void list_rows(tui_surface *sf, tui_rect lr, uint32_t now, tui_rect dr)
{
    const tui_rect a = tui_rect_make(lr.x, lr.y, lr.w, dr.y + dr.h - lr.y);
    const int top = lr.y;
    s_list = lr;
    s_rows_shown = s_list.h > 0 ? s_list.h : 0;
    if (!s_norder) {
        put(sf, a, a.x + 2, top + 1, st.ever ? "nothing of this kind heard" : "START a drive and what it hears is listed here", a_dim());
        return;
    }
    /* A finger dragged up or down carries the list (the selection stays in
       view); otherwise the list follows the selection. */
    const int last_top = s_norder > s_rows_shown ? s_norder - s_rows_shown : 0;
    if (ls_drag_scroll(&s_drag, s_list, 1, &s_top, s_norder, s_rows_shown)) {
        if (s_sel < s_top) sel_set(s_top);
        else if (s_sel >= s_top + s_rows_shown) sel_set(s_top + s_rows_shown - 1);
    } else {
        if (s_sel < s_top) s_top = s_sel;
        if (s_sel >= s_top + s_rows_shown) s_top = s_sel - s_rows_shown + 1;
    }
    if (s_top > last_top) s_top = last_top;
    if (s_top < 0) s_top = 0;
    const bool wide = a.w >= 80;
    const int namew = a.w - (wide ? 52 : 25) - 1;     /* one column for the scroll bar */
    /* The scroll bar: where this screenful sits in the whole list. */
    s_bar = tui_rect_make(0, -1, 0, 0);
    if (s_norder > s_rows_shown && s_rows_shown > 2) {
        const int bx = a.x + a.w - 1, h = s_rows_shown;
        int th = h * s_rows_shown / s_norder;
        if (th < 1) th = 1;
        const int ty = last_top ? (h - th) * s_top / last_top : 0;
        for (int r = 0; r < h; r++)
            tui_put_char(sf, a, bx, top + r, r >= ty && r < ty + th ? '#' : '|',
                         r >= ty && r < ty + th ? LIT(TUI_CYAN) : a_rule());
        s_bar = tui_rect_make(bx - 1, top, 2, h);
        char cnt[24];
        snprintf(cnt, sizeof(cnt), "%d-%d/%d", s_top + 1, s_top + s_rows_shown < s_norder ? s_top + s_rows_shown : s_norder, s_norder);
        put(sf, a, a.x + a.w - 1 - (int)strlen(cnt), top - 1, cnt, a_dim());
    }
    wd_lock();
    for (int r = 0; r < s_rows_shown && s_top + r < s_norder; r++) {
        const wd_dev_t *e = wd_dev_at(s_order[s_top + r]);
        if (!e) continue;
        const int y = top + r;
        const bool sel = s_top + r == s_sel;
        const int sec = e->kind == WD_WIFI ? wd_sec_class(e->auth) : 0;
        const uint8_t hue = kind_hue(e->kind, sec);
        if (sel) tui_fill(sf, tui_rect_make(a.x, y, a.w, 1), ' ', ls_ink(LS_INK_SELECT_DIM, TUI_ATTR(TUI_WHITE, TUI_BLUE)));
        const uint8_t bg = sel ? TUI_BLUE : G;
        tui_put_char(sf, a, a.x + 1, y, kind_glyph(e->kind), TUI_ATTR(hue | TUI_BRIGHT, bg));
        char s[64];
        snprintf(s, sizeof(s), "%-*.*s", namew > 0 ? namew : 0, namew > 0 ? namew : 0,
                 e->name[0] ? e->name : e->kind == WD_WIFI ? "<hidden>" : "(no name)");
        put(sf, a, a.x + 3, y, s, sel ? TUI_ATTR(TUI_WHITE | TUI_BRIGHT, bg) : a_text());
        int cx = a.x + 3 + (namew > 0 ? namew : 0) + 1;
        /* a target says what it is where the security would be */
        const uint16_t hit = e->tags & WD_HIT_TAGS;
        if (hit) {
            char tw[8];
            snprintf(tw, sizeof(tw), "%.6s", wd_tag_word(hit));
            put(sf, a, cx, y, tw, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, sel ? TUI_BLUE : TUI_RED));
        } else
            put(sf, a, cx, y, e->kind == WD_WIFI ? wd_auth_short(e->auth) : wd_kind_tag(e->kind),
                TUI_ATTR(hue | TUI_BRIGHT, bg));
        cx += 7;
        if (wide) {
            snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x", e->addr[0], e->addr[1], e->addr[2], e->addr[3], e->addr[4], e->addr[5]);
            put(sf, a, cx, y, s, TUI_ATTR(TUI_BLACK | TUI_BRIGHT, bg));
            cx += 18;
            snprintf(s, sizeof(s), e->kind == WD_WIFI && e->channel ? "ch%-3u" : "     ", (unsigned)e->channel);
            put(sf, a, cx, y, s, TUI_ATTR(TUI_WHITE, bg));
            cx += 6;
        }
        snprintf(s, sizeof(s), "%4d", e->rssi);
        put(sf, a, cx, y, s, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, bg));
        meter(sf, a, cx + 5, y, 2, e->rssi, TUI_ATTR(hue, bg));
        const unsigned age = (now - e->last_ms) / 1000;
        if (age < 60) snprintf(s, sizeof(s), "%3us", age);
        else if (age < 3600) snprintf(s, sizeof(s), "%3um", age / 60);
        else snprintf(s, sizeof(s), "%3uh", age / 3600);
        put(sf, a, cx + 8, y, s, TUI_ATTR(TUI_BLACK | TUI_BRIGHT, bg));
    }
    /* The selected one, in full. */
    const wd_dev_t *e = s_sel < s_norder ? wd_dev_at(s_order[s_sel]) : NULL;
    const int dy = dr.y;
    if (!ls_tui_compact()) tui_hline(sf, a, a.x, dy - 1, a.w, '-', a_rule());
    if (e) {
        char s[120];
        snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x  %s  heard %lux",
                 e->addr[0], e->addr[1], e->addr[2], e->addr[3], e->addr[4], e->addr[5],
                 e->kind == WD_WIFI ? wd_auth_name(e->auth) : e->kind == WD_BLE ? "Bluetooth LE" : e->kind == WD_CELL ? "LTE cell" : e->kind == WD_DRONE ? "drone link" : wd_kind_name(e->kind),
                 (unsigned long)e->seen);
        put(sf, a, a.x + 1, dy, s, a_text());
        if (e->sig) {
            /* what the signature table says it is, over the line's end */
            const wd_sig_info_t *si = wd_sig_at(e->sig - 1);
            if (si && si->label) {
                char lb[28];
                snprintf(lb, sizeof(lb), " %.24s ", si->label);
                const int lx = a.x + a.w - 1 - (int)strlen(lb);
                if (lx > a.x) put(sf, a, lx, dy - 1, lb, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_RED));
            }
        }
        char f[16], l[16];
        clock_text(f, sizeof(f), e->first_ms - st.start_ms);
        clock_text(l, sizeof(l), e->last_ms - st.start_ms);
        snprintf(s, sizeof(s), "first %s  last %s  best %d dBm", f, l, e->rssi);
        put(sf, a, a.x + 1, dy + 1, s, a_dim());
        if (e->flags & WD_F_PLACED) snprintf(s, sizeof(s), "strongest at %.5f, %.5f", e->lat, e->lon);
        else snprintf(s, sizeof(s), "no position (heard without a fix)");
        put(sf, a, a.x + 1, dy + 2, s, a_accent());
    }
    wd_unlock();
}

/* ------------------------------------------------------------ OPTIONS */

static const char *const PACE_N[] = { "FAST", "NORMAL", "SAVER" };
static const char *const UNIT_N[] = { "KM/H", "MPH" };
static const char *const POCKET_N[] = { "OFF", "30 S", "1 MIN", "2 MIN" };
static const uint32_t POCKET_MS[] = { 0, 30000, 60000, 120000 };
static int s_pocket_auto;
/* RADIO's SWAP in OPTIONS: AUTO, then the jobs each radio can pin */
static const char *const PIN_LORA_N[] = { "AUTO", "TPMS", "ISM", "MESH" };
static const int PIN_LORA_J[] = { -1, WD_JOB_TPMS, WD_JOB_ISM, WD_JOB_MESH };
static const char *const PIN_SDR_N[] = { "AUTO", "TPMS", "ISM", "ADS-B" };
static const int PIN_SDR_J[] = { -1, WD_JOB_TPMS, WD_JOB_ISM, WD_JOB_AIR };
static int pin_index(const int *J, int job)
{
    for (int i = 0; i < 4; i++) if (J[i] == job) return i;
    return 0;
}

static void apply_sources(void)
{
    if (!wd_stats()->running) return;
    bool src[WD_KINDS];
    for (int k = 0; k < WD_KINDS; k++) src[k] = s_src[k] && wd_feed_has(k);
    wd_lock();
    wd_set_sources(src, s_only_fix);
    wd_unlock();
    wd_feed_stop();
    wd_feed_start(src, s_pace);
}

static int opt_get(const ls_opt_t *o)
{
    if (o->arg < WD_KINDS) return s_src[o->arg] && wd_feed_has(o->arg);
    if (o->arg == 13) return s_pocket_auto;
    if (o->arg == 20) return pin_index(PIN_LORA_J, wd_radio_pinned(WD_RJ_LORA));
    if (o->arg == 21) return pin_index(PIN_SDR_J, wd_radio_pinned(WD_RJ_SDR));
    return o->arg == 10 ? s_pace : o->arg == 11 ? s_units : s_only_fix;
}
static void opt_set(const ls_opt_t *o, int v)
{
    if (o->arg < WD_KINDS) { s_src[o->arg] = v; apply_sources(); return; }
    if (o->arg == 20 || o->arg == 21) {
        const int i = v >= 0 && v < 4 ? v : 0;
        wd_radio_pin(o->arg == 20 ? WD_RJ_LORA : WD_RJ_SDR, (o->arg == 20 ? PIN_LORA_J : PIN_SDR_J)[i]);
        return;
    }
    if (o->arg == 13) {
        s_pocket_auto = v >= 0 && v < 4 ? v : 0;
        settings_set_rec("wd_pocket", (uint32_t)s_pocket_auto);
        ls_tui_pocket_auto(POCKET_MS[s_pocket_auto]);
        return;
    }
    if (o->arg == 10) { s_pace = v; wd_feed_pace(v); }
    else if (o->arg == 11) { s_units = v; s_units_chosen = true; settings_set_rec("wd_units", v ? 1u : 0u); }
    else { s_only_fix = v; apply_sources(); }
}
static const char *why_source(const ls_opt_t *o) { return wd_feed_has(o->arg) ? NULL : "not on this board"; }
static void act_pocket(const ls_opt_t *o) { (void)o; ls_tui_pocket_set(true); }
static void act_export(const ls_opt_t *o)
{
    (void)o;
    wd_lock(); wd_request(WD_REQ_EXPORT); wd_unlock();
    (void)wd_feed_spawn();
}
/* The hub's status line is shared (CHAT, LOCKER, reports): SEND TO LOCKER
   reads it only once it has sent something itself. */
static bool s_locker_used;
static void act_locker(const ls_opt_t *o)
{
    (void)o;
    s_locker_used = true;
    wd_lock(); wd_request(WD_REQ_EXPORT | WD_REQ_LOCKER); wd_unlock();
    (void)wd_feed_spawn();
}
static const char *why_saved(const ls_opt_t *o) { (void)o; return wd_stats()->ever && !wd_stats()->replay ? NULL : "drive first"; }
static void act_replay(const ls_opt_t *o) { (void)o; replay_pick(); }
static const char *why_replay(const ls_opt_t *o) { (void)o; return wd_stats()->running && !wd_stats()->replay ? "STOP first" : NULL; }
static void show_files(const ls_opt_t *o, char *out, size_t n)
{
    (void)o;
    const wd_stats_t *s = wd_stats();
    snprintf(out, n, "%s", s->note[0] ? s->note : s->stamp[0] ? s->stamp : "/sdcard/wardrive");
}
static void show_locker(const ls_opt_t *o, char *out, size_t n)
{
    (void)o;
    bool busy = false;
    if (!s_locker_used || !wd_feed_hub(out, n, &busy) || !out[0]) snprintf(out, n, "CSV + GPX");
}

static const ls_opt_t SOURCES[] = {
    {.label = "WI-FI",     .kind = LS_OPT_TOGGLE, .arg = WD_WIFI,  .get = opt_get, .set = opt_set, .why_not = why_source},
    {.label = "BLUETOOTH", .kind = LS_OPT_TOGGLE, .arg = WD_BLE,   .get = opt_get, .set = opt_set, .why_not = why_source},
    {.label = "CELL",      .kind = LS_OPT_TOGGLE, .arg = WD_CELL,  .get = opt_get, .set = opt_set, .why_not = why_source},
    {.label = "DRONES",    .kind = LS_OPT_TOGGLE, .arg = WD_DRONE, .get = opt_get, .set = opt_set, .why_not = why_source},
    {.label = "SUB-GHZ",   .kind = LS_OPT_TOGGLE, .arg = WD_RF,    .get = opt_get, .set = opt_set, .why_not = why_source},
    {.label = "AIRCRAFT",  .kind = LS_OPT_TOGGLE, .arg = WD_AIR,   .get = opt_get, .set = opt_set, .why_not = why_source},
    {.label = "LORA",      .kind = LS_OPT_TOGGLE, .arg = WD_LORA,  .get = opt_get, .set = opt_set, .why_not = why_source},
};
static const ls_opt_t RADIOS[] = {
    {.label = "LORA CHIP", .kind = LS_OPT_CYCLE, .arg = 20, .names = PIN_LORA_N, .n = 4, .get = opt_get, .set = opt_set},
    {.label = "SDR",       .kind = LS_OPT_CYCLE, .arg = 21, .names = PIN_SDR_N, .n = 4, .get = opt_get, .set = opt_set},
};
static const ls_opt_ctx_t RADIOS_CTX = {.name = "RADIO JOBS", .job = -1, .radio = LS_RSEL_NONE, LS_OPT_ROWS(RADIOS)};
static const ls_opt_ctx_t SOURCES_CTX = {.name = "SOURCES", .job = -1, .radio = LS_RSEL_NONE, LS_OPT_ROWS(SOURCES)};
static const ls_opt_t OPTIONS[] = {
    {.label = "REPLAY A DRIVE",   .kind = LS_OPT_ACTION, .act = act_replay, .why_not = why_replay},
    {.label = "SOURCES",          .kind = LS_OPT_MENU, .sub = &SOURCES_CTX},
    {.label = "RADIO JOBS",       .kind = LS_OPT_MENU, .sub = &RADIOS_CTX},
    {.label = "SCAN PACE",        .kind = LS_OPT_CYCLE, .arg = 10, .names = PACE_N, .n = 3, .get = opt_get, .set = opt_set},
    {.label = "UNITS",            .kind = LS_OPT_CYCLE, .arg = 11, .names = UNIT_N, .n = 2, .get = opt_get, .set = opt_set},
    {.label = "ONLY WITH A FIX",  .kind = LS_OPT_TOGGLE, .arg = 12, .get = opt_get, .set = opt_set},
    {.label = "POCKET LOCK",      .kind = LS_OPT_ACTION, .act = act_pocket},
    {.label = "AUTO POCKET LOCK", .kind = LS_OPT_CYCLE, .arg = 13, .names = POCKET_N, .n = 4, .get = opt_get, .set = opt_set},
    {.label = "SAVE CSV + GPX",   .kind = LS_OPT_ACTION, .act = act_export, .show = show_files, .why_not = why_saved},
    {.label = "SEND TO LOCKER",   .kind = LS_OPT_ACTION, .act = act_locker, .show = show_locker, .why_not = why_saved},
};
static const ls_opt_ctx_t CTX = {.name = "WARDRIVE", .job = -1, .radio = LS_RSEL_NONE, .back_to_screen = true, LS_OPT_ROWS(OPTIONS)};

/* ------------------------------------------------------------ screen */

/* The 3D page's keys and, while a replay plays, its transport and seek bar,
   from the bottom of `*ap` up; `*ap` is left with what the view gets. */
static void keys_3d(tui_surface *sf, tui_rect *ap, bool wide)
{
    /* The 3D page's own keys take the bar's place, big enough for a
       thumb: the view's controls and the way back, then the replay's. */
    /* The keys run to the bottom of the panel: the grid fits the outer
       keys' sides to its rounded corners (ls_btn_bar_grid_slot). */
    if (s_keys_hidden) {
        /* HIDE: the view gets the page; one key brings the keys back */
        static const ls_btn_t k1[1] = { { "SHOW KEYS", NULL, 'h', false, false, false } };
        static const uint8_t h1[1] = { TUI_RED };
        const int kb = ls_tui_compact() ? 1 : wide ? 2 : 4;
        ls_btn_bar_grid_slot(sf, tui_rect_make(ap->x, ap->y + ap->h - kb, ap->w, kb), k1, 1, -1, LS_BTN_SLOT_SCREEN, 1, h1);
        ap->h -= kb;
        s_rp_bar = tui_rect_make(0, -1, 0, 0);
        return;
    }
    char lab[GRID_N][16];
    ls_btn_t v[GRID_N];
    for (int i = 0; i < GRID_N; i++) {
        const int k = GRID[i];
        if (k == K_BACK) { v[i] = (ls_btn_t){ "BACK", NULL, 0, false, false, false }; continue; }
        if (k == K_HIDE) { v[i] = (ls_btn_t){ "HIDE", NULL, 'h', false, false, false }; continue; }
        const char *l = wd3d_label(k, lab[i], sizeof(lab[i]));
        if (l != lab[i]) snprintf(lab[i], sizeof(lab[i]), "%s", l);
        v[i] = (ls_btn_t){ lab[i], NULL, 0, wd3d_lit(k), false, false };
    }
    /* a D-pad, three to a row and four rows, each key a full third of the
       width (no margins either side) and six rows tall in portrait */
    const int vb = ls_tui_compact() ? 4 : wide ? 8 : 24;
    ls_btn_bar_grid_slot(sf, tui_rect_make(ap->x, ap->y + ap->h - vb, ap->w, vb), v, GRID_N, -1, LS_BTN_SLOT_SCREEN, GRID_N / 3,
                         GRID_HUE);
    ap->h -= vb;
    wd_replay_info_t ri;
    wd_replay_info(&ri);
    if (ri.on) {
        ls_btn_t r[4] = {
            { "SLOWER", NULL, ',', false, ri.speed <= 1, false },
            { ri.done ? "AGAIN" : ri.paused ? "PLAY" : "PAUSE", NULL, ' ', ri.paused, false, false },
            { "FASTER", NULL, '.', false, ri.speed >= 32, false },
            { "END", "REPLAY", 'x', false, false, false },
        };
        const int rb = ls_tui_compact() ? 1 : wide ? 3 : 4;
        ls_btn_bar_raised_slot(sf, tui_rect_make(ap->x, ap->y + ap->h - rb, ap->w, rb), r, 4, -1, LS_BTN_SLOT_QUICK);
        ap->h -= rb;
        /* the seek bar over them, with where and how fast */
        const int by = ap->y + ap->h - 1, bw = ap->w - 18 > 8 ? ap->w - 18 : ap->w - 2;
        tui_fill(sf, tui_rect_make(ap->x, by, ap->w, 1), ' ', TUI_ATTR(TUI_WHITE, G));
        const int lit = ri.len_s ? (int)((uint64_t)ri.pos_s * (unsigned)bw / ri.len_s) : bw;
        for (int i = 0; i < bw; i++)
            tui_put_char(sf, *ap, ap->x + 1 + i, by, i < lit ? '=' : '-', i < lit ? TUI_ATTR(TUI_CYAN | TUI_BRIGHT, G) : TUI_ATTR(TUI_BLUE, G));
        if (lit < bw) tui_put_char(sf, *ap, ap->x + 1 + lit, by, '>', TUI_ATTR(TUI_WHITE | TUI_BRIGHT, G));
        s_rp_bar = tui_rect_make(ap->x + 1, by, bw, 1);
        char t1[16], t2[16], t[40];
        clock_text(t1, sizeof(t1), ri.pos_s * 1000u);
        clock_text(t2, sizeof(t2), ri.len_s * 1000u);
        snprintf(t, sizeof(t), "%dx %s", ri.speed, t1);
        if (bw + 3 + (int)strlen(t) <= ap->w) tui_put_str(sf, *ap, ap->x + bw + 2, by, t, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, G));
        (void)t2;
        ap->h -= 1;
    } else s_rp_bar = tui_rect_make(0, -1, 0, 0);
}

static void draw(tui_surface *sf, tui_rect a)
{
    units_settle();                 /* the first fix decides, until someone picks */
    {
        static bool was;
        wd_replay_tick(real_ms());
        wd_replay_info_t ri;
        wd_replay_info(&ri);
        if (was && !ri.loading && !ri.on && wd_replay_error()[0]) say_now(wd_replay_error());
        if (was && !ri.loading && ri.on && ri.full) say_now("long drive: replaying the part that fits");
        was = ri.loading;
    }
    const uint32_t now = now_ms();
    s_replay_chip = tui_rect_make(0, -1, 0, 0);
    /* No worker (the simulator, or a task that could not start): step here. */
    if (!wd_feed_worker_alive() && wd_busy() && now - s_step_ms >= 250) { s_step_ms = now; wd_step(now); }
    wd_lock();
    st = *wd_stats();
    ndrops = 0;
    for (const wd_drop_t *d; ndrops < WD_DROPS && (d = wd_drop_at(ndrops)); ndrops++) drops[ndrops] = *d;
    wd_unlock();
    if (a.w < 30 || a.h < 16) { ls_panel_notice(sf, a, NULL, "More screen space needed", "Use a smaller font"); return; }
    const bool wide = ls_tui_is_wide();
    /* nine keys: three rows of three in portrait, one row on a wide screen */
    const int bar_rows = ls_tui_compact() || wide ? 1 : 3;
    const int bar_h = ls_tui_compact() ? 1 : wide ? 3 : 9;
    char list_count[12];
    unsigned all = 0;
    for (int k = 0; k < WD_KINDS; k++) all += st.total[k];
    if (all) snprintf(list_count, sizeof(list_count), "%u", all);
    else snprintf(list_count, sizeof(list_count), "--");
    ls_btn_t b[] = {
        {st.replay ? "END" : st.running ? "STOP" : "START", st.replay ? "REPLAY" : st.running ? "REC" : "DRIVE", 's', st.running, false, false},
        {"LIVE", "DASH", '1', s_page == PG_LIVE, false, false},
        {"3D", "VIEW", '2', s_page == PG_3D, false, false},
        {"LIST", list_count, '3', s_page == PG_LIST, false, false},
        {"HUNT", "TARGETS", '4', s_page == PG_HUNT, false, false},
        {"RADIO", "JOBS", '5', s_page == PG_RADIO, false, false},
        {"ATLAS", "ROUTES", '6', s_page == PG_ATLAS, false, false},
        {"MAP", "LAYER", 'm', false, !st.origin && !st.ever, false},
        ls_opt_button(&CTX),
    };
    static const uint8_t hues[9] = { TUI_RED, 0, 0, 0, TUI_YELLOW, 0, 0, 0, 0 };
    const bool say_on = s_say[0] && real_ms() - s_say_ms < 4000;
    if (ls_tui_compact()) {
        /* A desk window: the keys are the toolbar under the menu bar, the
           pages draw under it, the state goes to the status bar. The 3D
           page keeps its own keys (they take the toolbar's place, as on the
           handheld), at the bottom of the window. */
        tui_fill(sf, a, ' ', a_text());
        if (s_page == PG_3D) {
            keys_3d(sf, &a, wide);
        } else {
            /* nine keys on one row: the one-word names (the LIST count
               stays), so all of them fit a Medium window */
            ls_btn_t bt[9];
            for (int i = 0; i < 9; i++) { bt[i] = b[i]; if (i != 3) bt[i].value = NULL; }
            ls_btn_bar_raised_hued(sf, tui_rect_make(a.x, a.y, a.w, 1), bt, 9, -1, hues);
            a = tui_rect_make(a.x + 1, a.y + 1, a.w - 2, a.h - 1);
        }
        char t[72], c[16];
        clock_text(c, sizeof(c), st.running ? now - st.start_ms : st.stop_ms - st.start_ms);
        if (say_on) snprintf(t, sizeof(t), "%s", s_say);
        else if (st.replay) snprintf(t, sizeof(t), "Replaying a drive, %u found", all);
        else if (st.running) snprintf(t, sizeof(t), "Recording %s  %u found  %u rows", c, all, st.rows);
        else if (st.ever) snprintf(t, sizeof(t), "Stopped; last drive %s, %u found", c, all);
        else snprintf(t, sizeof(t), "Ready: START logs what is heard, and where");
        ls_tui_status_set(t, NULL);
        if (!say_on) s_say[0] = 0;
    } else {
    if (s_page == PG_3D) {
        keys_3d(sf, &a, wide);
    } else {
        ls_btn_bar_grid_slot(sf, tui_rect_make(a.x, a.y + a.h - bar_h, a.w, bar_h), b, 9, -1, LS_BTN_SLOT_SCREEN, bar_rows, hues);
        a.h -= bar_h + 1;
    }
    if (say_on) {
        /* the answer to the last key, on the bottom row of the page */
        char t[70];
        snprintf(t, sizeof(t), " %s ", s_say);
        tui_put_str(sf, a, a.x + (a.w - (int)strlen(t)) / 2 > a.x ? a.x + (a.w - (int)strlen(t)) / 2 : a.x, a.y + a.h - 1, t,
                    TUI_ATTR(TUI_BLACK, TUI_YELLOW | TUI_BRIGHT));
        a.h--;
    } else s_say[0] = 0;
    }
    if (s_page != PG_3D) wd3d_leave();
    replay_summarize_step();
    switch (s_page) {
    case PG_3D: page_3d(sf, a, now); break;
    case PG_LIST: {
        char lt[24];
        snprintf(lt, sizeof(lt), "LIST  %d", s_norder);
        const tui_rect in = ls_plate(sf, a, a, TUI_CYAN, lt, NULL, 0);
        list_page(sf, tui_rect_make(in.x - 1, in.y, in.w + 2, in.h), now);
        break;
    }
    case PG_HUNT: case PG_RADIO: case PG_ATLAS: xpage(s_page)->draw(sf, a, now); break;
    default: live_page(sf, a, now); break;
    }
}

static void page(int p)
{
    if (p != s_page && p != PG_3D) wd3d_leave();
    const wd_page_t *old = xpage(s_page);
    if (p != s_page && old && old->leave) old->leave();
    s_page = p;
}

static bool key(ls_tk_t k, char ch)
{
    const uint32_t now = now_ms();
    if (ls_opt_key(&CTX, ch)) return true;
    if (ch == 's' || ch == 'S') { session_toggle(); return true; }
    if (ch == '1') { page(PG_LIVE); return true; }
    if (ch == '2') { page(PG_3D); return true; }
    if (ch == '3') { page(PG_LIST); return true; }
    if (ch == '4') { page(PG_HUNT); return true; }
    if (ch == '5') { page(PG_RADIO); return true; }
    if (ch == '6') { page(PG_ATLAS); return true; }
    if (xpage(s_page) && xpage(s_page)->key(k, ch, now)) return true;
    if (ch == 'm' || ch == 'M') { open_map(); return true; }
    /* r is RADIO in every app (the shared rule): here the RADIO page, where
       each radio's job is chosen; saved drives moved to d */
    if (ch == 'r' || ch == 'R') { page(PG_RADIO); return true; }
    if (ch == 'd' || ch == 'D') { replay_pick(); return true; }
    if (wd_replay_on()) {
        const uint32_t r = real_ms();
        if (ch == ' ' || (k == LS_TK_ENTER && s_page != PG_LIST)) { wd_replay_pause(r); return true; }
        if (ch == ',' || ch == '<') { wd_replay_speed(-1, r); return true; }
        if (ch == '.' || ch == '>') { wd_replay_speed(+1, r); return true; }
        if (ch == '[' || ch == ']') {
            wd_replay_info_t ri; wd_replay_info(&ri);
            const float f = ri.len_s ? (float)ri.pos_s / ri.len_s : 0;
            wd_replay_seek(f + (ch == '[' ? -0.1f : 0.1f), r);
            return true;
        }
        if (ch == 'x' || ch == 'X') { session_toggle(); return true; }
    }
    if (k == LS_TK_ESC || ch == 'b' || ch == 'B') {
        if (s_page != PG_LIVE) { page(PG_LIVE); return true; }
        return false;
    }
    if (s_page == PG_3D) {
        if (ch == 'p' || ch == 'P') { s_perf = !s_perf; return true; }
        if (ch == 'h' || ch == 'H') { s_keys_hidden = !s_keys_hidden; return true; }
        return wd3d_key(k, ch, now);
    }
    if (s_page == PG_LIST) {
        if (k == LS_TK_UP) { sel_set(s_sel - 1); return true; }
        if (k == LS_TK_DOWN) { sel_set(s_sel + 1); return true; }
        if (k == LS_TK_PGUP) { sel_set(s_sel - s_rows_shown); return true; }
        if (k == LS_TK_PGDN) { sel_set(s_sel + s_rows_shown); return true; }
        if (k == LS_TK_LEFT || k == LS_TK_RIGHT) {
            s_filter = (s_filter + (k == LS_TK_LEFT ? F_N - 1 : 1)) % F_N;
            s_sel = s_top = 0; s_sel_dev = -1; s_order_ms = 0;
            return true;
        }
        if (ch == 'n' || ch == 'N') { s_sort = !s_sort; s_order_ms = 0; return true; }
    }
    if (k == LS_TK_ENTER && s_page == PG_LIVE) { session_toggle(); return true; }
    return false;
}

static bool touch(int x, int y)
{
    const uint32_t now = now_ms();
    const int hit = ls_btn_hit(x, y);
    if (s_page == PG_3D) {
        if (s_keys_hidden) {
            if (hit == 0) { s_keys_hidden = false; return true; }
        } else if (hit >= 0 && hit < GRID_N) {
            const int k = GRID[hit];
            if (k == K_BACK) page(PG_LIVE);
            else if (k == K_HIDE) s_keys_hidden = true;
            else wd3d_act(k);
            return true;
        }
        if (s_show_chip.h > 0 && y == s_show_chip.y && x >= s_show_chip.x && x < s_show_chip.x + s_show_chip.w) { wd3d_act(WD3D_SHOW); return true; }
        const int rp = ls_btn_hit_slot(x, y, LS_BTN_SLOT_QUICK);
        const uint32_t r = real_ms();
        if (rp == 0) { wd_replay_speed(-1, r); return true; }
        if (rp == 1) { wd_replay_pause(r); return true; }
        if (rp == 2) { wd_replay_speed(+1, r); return true; }
        if (rp == 3) { session_toggle(); return true; }
        if (s_rp_bar.h > 0 && y == s_rp_bar.y && x >= s_rp_bar.x && x < s_rp_bar.x + s_rp_bar.w) {
            wd_replay_seek((float)(x - s_rp_bar.x) / (float)(s_rp_bar.w > 1 ? s_rp_bar.w - 1 : 1), r);
            return true;
        }
        if (s_replay_chip.h > 0 && y == s_replay_chip.y && x >= s_replay_chip.x && x < s_replay_chip.x + s_replay_chip.w) { replay_pick(); return true; }
        return wd3d_tap(x, y, s_3d, now);
    }
    if (hit == 0) { session_toggle(); return true; }
    if (hit == 1) { page(PG_LIVE); return true; }
    if (hit == 2) { page(PG_3D); return true; }
    if (hit == 3) { page(PG_LIST); return true; }
    if (hit == 4) { page(PG_HUNT); return true; }
    if (hit == 5) { page(PG_RADIO); return true; }
    if (hit == 6) { page(PG_ATLAS); return true; }
    if (hit == 7) { open_map(); return true; }
    if (hit == 8) { ls_opt_open(&CTX); return true; }
    if (xpage(s_page) && xpage(s_page)->touch(x, y, now)) return true;
    if (s_replay_chip.h > 0 && y == s_replay_chip.y && x >= s_replay_chip.x && x < s_replay_chip.x + s_replay_chip.w) { replay_pick(); return true; }
    if (s_page == PG_3D && wd_replay_on()) {
        const uint32_t r = real_ms();
        if (s_rp_bar.h > 0 && y == s_rp_bar.y && x >= s_rp_bar.x && x < s_rp_bar.x + s_rp_bar.w) {
            wd_replay_seek((float)(x - s_rp_bar.x) / (float)(s_rp_bar.w > 1 ? s_rp_bar.w - 1 : 1), r);
            return true;
        }
        for (int i = 0; i < 4; i++) {
            const tui_rect b = s_rp_btn[i];
            if (b.h <= 0 || y != b.y || x < b.x || x >= b.x + b.w) continue;
            if (i == 0) wd_replay_speed(-1, r);
            else if (i == 1) wd_replay_pause(r);
            else if (i == 2) wd_replay_speed(+1, r);
            else session_toggle();
            return true;
        }
    }
    if (s_page == PG_3D) return wd3d_tap(x, y, s_3d, now);
    if (s_page == PG_LIST) {
        for (int f = 0; f <= F_N; f++) {
            const tui_rect r = s_chip[f];
            if (y == r.y && x >= r.x && x < r.x + r.w) {
                if (f == F_N) s_sort = !s_sort;
                else { s_filter = f; s_sel = s_top = 0; s_sel_dev = -1; }
                s_order_ms = 0;
                return true;
            }
        }
        if (ls_drag_scroll_tap_is_lift(&s_drag)) return true;
        /* A tap on the scroll bar pages to that part of the list. */
        if (s_bar.h > 0 && y >= s_bar.y && y < s_bar.y + s_bar.h && x >= s_bar.x) {
            sel_set((y - s_bar.y) * s_norder / s_bar.h);
            return true;
        }
        if (y >= s_list.y && y < s_list.y + s_list.h && s_top + (y - s_list.y) < s_norder) {
            sel_set(s_top + (y - s_list.y));
            return true;
        }
    }
    return false;
}

/* The home base's link reads and merges WARDRIVE's atlas (ls_lsx_svc.h);
   its calls come from the link's task with no wardrive lock held. */
static uint32_t lsx_seq(void *c) { (void)c; return wd_atlas_seq(); }
static size_t lsx_delta(void *c, uint32_t since, uint8_t *o, size_t n, uint32_t *next)
{
    (void)c;
    return wd_atlas_delta(since, o, n, next);
}
static int lsx_apply(void *c, const uint8_t *b, size_t n, uint32_t origin)
{
    (void)c;
    return wd_atlas_apply(b, n, origin);
}
static const ls_lsx_store_t WD_LSX = { NULL, lsx_seq, lsx_delta, lsx_apply };

static void enter(void)
{
    static bool attached;
    if (!attached) { attached = true; ls_lsx_set_store(&WD_LSX); wd_hunt_attach(); }
    ls_location_want();
    units_settle();
    wd_sig_init();
    const uint32_t pa = settings_get_rec("wd_pocket", 0);
    s_pocket_auto = pa < 4 ? (int)pa : 0;
    ls_tui_pocket_auto(POCKET_MS[s_pocket_auto]);
    s_order_ms = 0;
    s_card.known = false;
    if (wd_stats()->ever) ls_rfmap_set_extra(map_layer);   /* the last drive, on MAP too */
}

/* A replay only plays while WARDRIVE draws, and its events hold up to ~4 MB
   of PSRAM that MAP needs: leaving ends it (the drive's devices and track
   stay in the session, so MAP's layer still shows them). */
static void leave(void)
{
    const wd_page_t *xp = xpage(s_page);
    if (xp && xp->leave) xp->leave();
    ls_tui_pocket_auto(0);              /* auto lock is WARDRIVE's; the lock itself stays until unlocked */
    wd3d_leave();
    wd_replay_close();
    s_sum_at = -1;
    wd_drive_summarize(NULL, 0, NULL);  /* an old drive's summary left half read lets go of its file */
}

const ls_tui_screen_t ls_scr_wardrive = {
    .name = "WARDRIVE",
    .hint = "S start/stop  1 live  2 3D  3 list  4 hunt  5 radio  6 atlas  R radio  D drives  M map  O options",
    .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch,
};
