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

#define MAX_TILES 16

static int  s_sel;
static int  s_page;      /* 0 directory, 1 status - portrait only */

static int build_tiles(ls_tile_t *out, const ls_app_t **apps, int cap)
{
    int n = 0;
    static const ls_app_cat_t ORDER[] = { LS_APP_MAIN, LS_APP_EXTRA, LS_APP_USER };
    for (unsigned c = 0; c < sizeof(ORDER) / sizeof(ORDER[0]); c++) {
        for (int i = 0; i < ls_app_count() && n < cap; i++) {
            const ls_app_t *a = ls_app_at(i);
            if (!a || a->cat != ORDER[c]) continue;
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

    const bool wide = ls_tui_is_wide();

    if (wide) {
        /* Landscape fits both, so it shows both. A page turn to read the
           dongle state is a page turn nobody should have to make. */
        /* The status panel takes what it needs, the tiles take the rest. */

        int status_w = area.w / 3;
        if (status_w > 34) status_w = 34;
        tui_rect left = tui_rect_make(area.x, area.y, area.w - status_w,
                                      area.h);
        tui_rect right = tui_rect_make(area.x + left.w, area.y,
                                       status_w, area.h);
        ls_tile_grid(sf, left, tiles, n, s_sel);
        draw_status(sf, right);
        return;
    }

    if (s_page == 1) { draw_status(sf, area); return; }

    ls_tile_grid(sf, area, tiles, n, s_sel);
}

static bool key(ls_tk_t k, char ch)
{
    ls_tile_t tiles[MAX_TILES];
    const ls_app_t *apps[MAX_TILES];
    const int n = build_tiles(tiles, apps, MAX_TILES);
    int cols = 1, rows = 1;
    ls_tile_shape(&cols, &rows);
    if (cols < 1) cols = 1;

    switch (k) {
    case LS_TK_LEFT:
        if (!ls_tui_is_wide() && s_page == 1) { s_page = 0; return true; }
        if (s_sel > 0) s_sel--;
        return true;
    case LS_TK_RIGHT:
        if (s_sel + 1 < n) { s_sel++; return true; }
        if (!ls_tui_is_wide()) { s_page = 1; return true; }
        return true;
    case LS_TK_UP:
        if (s_sel - cols >= 0) s_sel -= cols;
        return true;
    case LS_TK_DOWN:
        if (s_sel + cols < n) s_sel += cols;
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
            if (a == b) { s_sel = i; return true; }
        }
        return false;
    default:
        return false;
    }
}

/* A miss is a miss, and on this screen that matters twice over. */

static bool touch(int col, int row)
{
    const int i = ls_tile_hit(col, row);
    if (i < 0) return true;

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

static void enter(void) { s_page = 0; }

const ls_tui_screen_t ls_scr_home = {
    .name = "HOME",
    .hint = "ARROWS pick  ENTER open  F1..F8 jump",
    .enter = enter,
    .leave = NULL,
    .draw = draw,
    .key = key,
    .touch = touch,
};
