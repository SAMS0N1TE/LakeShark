/* HOME: the directory, and the state of the radio behind it. */

#include "../../ls_tui_screen.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "../../ls_app.h"
#include "../../ls_icons.h"
#include "../../ls_tui_ui.h"
#include "../../ls_userapp.h"

/* Fully qualified: a bare "perf.h" resolved to something else on this
   include path and every call fell through as an implicit declaration. */
#include "core/perf.h"
#include "radio/radio_health.h"
#include "radio/radio_endpoint.h"

#define MAX_TILES LS_TUI_MAX_SCREENS
#define PAGE_TILES 6
static int s_group, s_tile_page;
static const char *const GROUPS[] = {"RADIO", "FIELD", "SYSTEM", "USER"};

static int group_of(const ls_app_t *a)
{
    if (a->cat == LS_APP_USER) return 3;
    if (!strcmp(a->id, "map") || !strcmp(a->id, "gps") || !strcmp(a->id, "journal") || !strcmp(a->id, "rec")) return 1;
    if (!strcmp(a->id, "set") || !strcmp(a->id, "diag") || !strcmp(a->id, "radios") || !strcmp(a->id, "link")) return 2;
    return 0;
}

static int  s_sel;

static int build_tiles(ls_tile_t *out, const ls_app_t **apps, int cap)
{
    int n = 0;
    static const ls_app_cat_t ORDER[] = { LS_APP_MAIN, LS_APP_EXTRA, LS_APP_USER };
    for (unsigned c = 0; c < sizeof(ORDER) / sizeof(ORDER[0]); c++) {
        for (int i = 0; i < ls_app_count() && n < cap; i++) {
            const ls_app_t *a = ls_app_at(i);
            if (!a || !a->id || a->cat != ORDER[c] || group_of(a) != s_group) continue;
            /* HOME is not in its own directory. */
            if (a->screen && a->id && strcmp(a->id, "home") == 0) continue;
            apps[n] = a;
            out[n].name = a->name;
            out[n].sub  = a->sub;
            out[n].icon = a->icon;
            out[n].hue  = a->hue;
            out[n].live = a->live ? a->live() : false;
            n++;
        }
    }
    return n;
}

static void draw_status(tui_surface *sf, tui_rect a)
{
    char buf[48];
    const uint8_t val  = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t good = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t bad  = TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK);
    const uint8_t dim  = TUI_ATTR(TUI_WHITE, TUI_BLACK);

    ls_panel_box(sf, a, "RECEIVER", TUI_CYAN);

    int row = 1;
    radio_health_snapshot_t h;
    if (radio_health_get(LS_RADIO_ENDPOINT_RTL_USB, &h)) {
        ls_kv(sf, a, row++, "DONGLE", radio_health_state_name(h.state),
              h.state == RH_OK ? good : bad);
        snprintf(buf, sizeof(buf), "%lu B/s", (unsigned long)h.bytes_per_second);
        ls_kv(sf, a, row++, "STREAM", buf, h.bytes_per_second ? val : dim);
        if (h.recoveries) {
            snprintf(buf, sizeof(buf), "%u", (unsigned)h.recoveries);
            ls_kv(sf, a, row++, "RECOVER", buf, bad);
        }
    } else {
        ls_kv(sf, a, row++, "DONGLE", "absent", bad);
    }

    snprintf(buf, sizeof(buf), "%d/s", perf_get_msgs_per_sec());
    ls_kv(sf, a, row++, "DECODE", buf, val);
    snprintf(buf, sizeof(buf), "%d err", perf_get_crc_err());
    ls_kv(sf, a, row++, "CRC", buf, perf_get_crc_err() ? dim : good);

    if (row < a.h - 3) row++;
    const size_t inter = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    snprintf(buf, sizeof(buf), "%u KB", (unsigned)(inter / 1024));
    ls_kv(sf, a, row++, "INTERNAL", buf, inter < 40000 ? bad : val);
    snprintf(buf, sizeof(buf), "%u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    ls_kv(sf, a, row++, "PSRAM", buf, val);

    const int64_t up = esp_timer_get_time() / 1000000;
    snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", (long long)(up / 3600),
             (long long)((up / 60) % 60), (long long)(up % 60));
    ls_kv(sf, a, row++, "UPTIME", buf, dim);

    const char *e = ls_userapp_last_error();
    if (e && row < a.h - 1)
        tui_put_str(sf, a, a.x + 2, a.y + a.h - 2, e,
                    TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK));
}

static void draw(tui_surface *sf, tui_rect area)
{
    ls_tile_t tiles[MAX_TILES];
    const ls_app_t *apps[MAX_TILES];
    const int n = build_tiles(tiles, apps, MAX_TILES);
    if (s_sel >= n) s_sel = n ? n - 1 : 0;

    ls_btn_t groups[5];
    for (int i = 0; i < 4; i++) groups[i] = (ls_btn_t){GROUPS[i], NULL, "rfsu"[i], s_group == i, false};
    groups[4] = (ls_btn_t){"MORE", NULL, ']', false, n <= PAGE_TILES};
    const int bar_h = ls_btn_raised_height(area, 5);
    ls_btn_bar_raised(sf, tui_rect_make(area.x, area.y, area.w, bar_h), groups, 5, -1);
    const int pages = (n + PAGE_TILES - 1) / PAGE_TILES;
    if (s_tile_page >= pages) s_tile_page = 0;
    int first = s_tile_page * PAGE_TILES;
    int shown = n - first; if (shown > PAGE_TILES) shown = PAGE_TILES;
    if (s_sel < first || s_sel >= first + shown) s_sel = first;
    tui_rect body = tui_rect_make(area.x, area.y + bar_h, area.w, area.h - bar_h);
    if (ls_tui_is_wide()) {
        int status_w = body.w / 3; if (status_w > 34) status_w = 34;
        tui_rect right = tui_rect_make(body.x + body.w - status_w, body.y, status_w, body.h);
        body.w -= status_w;
        draw_status(sf, right);
    }
    ls_tile_grid(sf, body, tiles + first, shown, s_sel - first);
    if (!shown) ls_panel_notice(sf, body, GROUPS[s_group], "No apps in this group", "User apps load from the SD card");

}

static bool key(ls_tk_t k, char ch)
{
    ls_tile_t tiles[MAX_TILES];
    const ls_app_t *apps[MAX_TILES];
    const int n = build_tiles(tiles, apps, MAX_TILES);
    int cols = 1, rows = 1;
    ls_tile_shape(&cols, &rows);
    if (cols < 1) cols = 1;

    char group_key=ch>='A' && ch<='Z'?ch+'a'-'A':ch;
    if (k == LS_TK_CHAR && group_key && strchr("rfsu", group_key)) { s_group = (int)(strchr("rfsu", group_key) - "rfsu"); s_tile_page = s_sel = 0; return true; }
    if (k == LS_TK_CHAR && ch == ']') { s_tile_page = (s_tile_page + 1) % ((n + PAGE_TILES - 1) / PAGE_TILES > 0 ? (n + PAGE_TILES - 1) / PAGE_TILES : 1); s_sel = s_tile_page * PAGE_TILES; return true; }
    switch (k) {
    case LS_TK_LEFT:
        if (s_sel > 0) { s_sel--; s_tile_page = s_sel / PAGE_TILES; }
        return true;
    case LS_TK_RIGHT:
        if (s_sel + 1 < n) { s_sel++; s_tile_page = s_sel / PAGE_TILES; return true; }
        return true;
    case LS_TK_UP:
        if (s_sel - cols >= 0) { s_sel -= cols; s_tile_page = s_sel / PAGE_TILES; }
        return true;
    case LS_TK_DOWN:
        if (s_sel + cols < n) { s_sel += cols; s_tile_page = s_sel / PAGE_TILES; }
        return true;
    case LS_TK_ENTER:
        if (s_sel < n) {
            for (int i = 0; i < ls_app_count(); i++)
                if (ls_app_at(i) == apps[s_sel]) { ls_app_open(i); break; }
        }
        return true;
    case LS_TK_CHAR:
        /* Type the first letter of an app name to jump to it. Cheap, and it
           is the one keyboard idiom every launcher has. */
        for (int i = 0; i < n; i++) {
            const char a = tiles[i].name ? tiles[i].name[0] : 0;
            const char b = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
            if (a == b) { s_sel = i; s_tile_page = s_sel / PAGE_TILES; return true; }
        }
        return false;
    default:
        return false;
    }
}

/* A miss is a miss, and on this screen that matters twice over. */

static bool touch(int col, int row)
{
    const int group = ls_btn_hit(col, row);
    if (group >= 0 && group < 4) { s_group = group; s_tile_page = s_sel = 0; return true; }
    if (group == 4) return key(LS_TK_CHAR, ']');
    const int hit = ls_tile_hit(col, row);
    if (hit < 0) return true;
    const int i = hit + s_tile_page * PAGE_TILES;

    ls_tile_t tiles[MAX_TILES];
    const ls_app_t *apps[MAX_TILES];
    const int n = build_tiles(tiles, apps, MAX_TILES);
    if (i >= n) return true;

    /* First tap selects, second tap opens - but only when the first
       tap moved the selection. Tapping the tile you are already on opens it,
       which is what a finger expects; tapping a different one shows you what
       you are about to open first. */
    if (s_sel != i) { s_sel = i; return true; }
    for (int a = 0; a < ls_app_count(); a++)
        if (ls_app_at(a) == apps[i]) { ls_app_open(a); break; }
    return true;
}

const ls_tui_screen_t ls_scr_home = {
    .name = "HOME",
    .hint = "R RADIO  F FIELD  S SYSTEM  U USER  ] more",
    .enter = NULL,
    .leave = NULL,
    .draw = draw,
    .key = key,
    .touch = touch,
};
