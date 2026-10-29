/* HOME: the directory, and the state of the radio behind it. */

#include "../../ls_tui_screen.h"
#include "../../ls_i18n.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "../../ls_app.h"
#include "../../ls_icons.h"
#include "../../ls_tui_ui.h"
#include "../../ls_userapp.h"
#include "../../ls_value.h"

/* Fully qualified: a bare "perf.h" resolved to something else on this
   include path and every call fell through as an implicit declaration. */
#include "core/perf.h"
#include "radio/radio_health.h"
#include "radio/radio_endpoint.h"

extern const ls_tui_screen_t ls_scr_home;

#define MAX_TILES LS_TUI_MAX_SCREENS
/* Directory capacity is not a TUI stack budget. The UI task is the sole
   caller; drawing and input reuse the same zero-init PSRAM scratch. */
EXT_RAM_BSS_ATTR static ls_tile_t s_tiles[MAX_TILES];
EXT_RAM_BSS_ATTR static const ls_app_t *s_apps[MAX_TILES];
/* Last rendered capacity also drives keyboard and touch page offsets. */
static int s_page_tiles = 6;
#define PAGE_TILES s_page_tiles
static int s_group, s_tile_page;
static bool s_error_detail;
static int s_error_page, s_error_pages;
static tui_rect s_error_banner;

static const char *const GROUPS[] = {"RADIO", "FIELD", "SYSTEM", "USER"};

/* The directory, in the order it is shown. NULL closes a group: RADIO, then
   FIELD, then SYSTEM. A built-in missing from this list still appears, at
   the end of RADIO, so a new app is never lost for want of a line here. */
static const char *const LAYOUT[] = {
    "p25", "fm", "adsb", "falls", "mesh", "subghz", "digi", "labs", "rec", "radio", NULL,
    "map", "gps", "compass", "notes", "sweep", "drones", "cell", NULL,
    "set", "link", "radios", "update", "diag", "files", "music", NULL,
};

/* Folders: one tile on HOME for a lead app and the apps that belong with it.
   The children stay registered with their own screens, so anything that
   opens them by id or by screen index is unaffected; only HOME folds them. */
static const struct {
    const char *lead, *name, *sub;
    const char *kids[3];
} FOLDERS[] = {
    { "labs",    "LABS",    "lora + trials",   { "experiments", "mixrf", "meters" } },
    { "rec",     "REC",     "rec + rewind",    { "tmach" } },
    { "map",     "MAP",     "maps, regions",   { "tiles" } },
    { "compass", "COMPASS", "bearing, hunt",   { "fox", "rfsurvey" } },
    { "diag",    "DIAG",    "health, shell",   { "terminal" } },
};
#define N_FOLDERS (int)(sizeof(FOLDERS) / sizeof(FOLDERS[0]))

/* The open folder, or -1 for the group's own tiles. */
static int s_folder = -1;

/* Folded away, and reachable: a build without the lead shows the child. */
static bool in_folder(const char *id)
{
    for (int f = 0; f < N_FOLDERS; f++)
        for (int k = 0; k < 3; k++)
            if (FOLDERS[f].kids[k] && !strcmp(FOLDERS[f].kids[k], id))
                return ls_app_by_id(FOLDERS[f].lead) != NULL;
    return false;
}

static int folder_of(const char *id)
{
    for (int f = 0; f < N_FOLDERS; f++) if (!strcmp(FOLDERS[f].lead, id)) return f;
    return -1;
}

static int  s_sel;

/* A newer verified build is known (main/ls_ota.c publishes ota.notify). */
static bool update_known(void)
{
    ls_val_t v;
    return ls_value_read("ota.notify", &v, NULL) && v.kind == LS_VAL_INT && v.i != 0;
}

static void add_tile(ls_tile_t *out, const ls_app_t **apps, int *n, const ls_app_t *a, bool fold)
{
    ls_tile_t *t = &out[*n];
    apps[(*n)++] = a;
    *t = (ls_tile_t){ a->name, a->sub, a->icon, a->hue, a->live && a->live(),
                      !strcmp(a->id, "update") && update_known(), 0 };
    const int f = fold ? folder_of(a->id) : -1;
    if (f < 0) return;
    /* A folder says how much is behind it, and is live when anything in it
       is, so a running TIME MACHINE still shows on HOME. */
    for (int k = 0; k < 3; k++) {
        const ls_app_t *c = FOLDERS[f].kids[k] ? ls_app_by_id(FOLDERS[f].kids[k]) : NULL;
        if (!c) continue;
        t->more++;
        if (c->live && c->live()) t->live = true;
    }
    if (t->more) { t->name = FOLDERS[f].name; t->sub = FOLDERS[f].sub; }
}

static int build_tiles(ls_tile_t *out, const ls_app_t **apps, int cap)
{
    int n = 0;
    if (s_folder >= 0) {
        /* Inside a folder: the lead app first, then the rest of it. */
        const ls_app_t *a = ls_app_by_id(FOLDERS[s_folder].lead);
        if (a) add_tile(out, apps, &n, a, false);
        for (int k = 0; k < 3 && n < cap; k++) {
            a = FOLDERS[s_folder].kids[k] ? ls_app_by_id(FOLDERS[s_folder].kids[k]) : NULL;
            if (a) add_tile(out, apps, &n, a, false);
        }
        return n;
    }
    if (s_group < 3) {
        int g = 0;
        for (unsigned i = 0; i < sizeof(LAYOUT) / sizeof(LAYOUT[0]) && n < cap; i++) {
            if (!LAYOUT[i]) { g++; continue; }
            const ls_app_t *a = g == s_group ? ls_app_by_id(LAYOUT[i]) : NULL;
            if (a && a->cat != LS_APP_USER) add_tile(out, apps, &n, a, true);
        }
    }
    for (int i = 0; i < ls_app_count() && n < cap; i++) {
        const ls_app_t *a = ls_app_at(i);
        if (!a || !a->id || a->screen == &ls_scr_home) continue;
        if (a->cat == LS_APP_USER ? s_group != 3 : s_group != 0) continue;
        if (a->cat != LS_APP_USER) {
            /* Built-ins the layout or a folder already placed. */
            bool placed = in_folder(a->id);
            for (unsigned l = 0; l < sizeof(LAYOUT) / sizeof(LAYOUT[0]) && !placed; l++)
                placed = LAYOUT[l] && !strcmp(LAYOUT[l], a->id);
            if (placed) continue;
        }
        add_tile(out, apps, &n, a, false);
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
    static const char *const SDRS[] = {
        LS_RADIO_ENDPOINT_RTL_USB, LS_RADIO_ENDPOINT_HACKRF_USB
    };
    bool present = false;
    for (int i = 0; i < 2; i++) {
        if (!radio_health_get(SDRS[i], &h) || h.state == RH_ABSENT) continue;
        present = true;
        ls_kv(sf, a, row++, i ? "HACKRF" : "RTL-SDR",
              radio_health_state_name(h.state), h.state == RH_OK ? good : bad);
        snprintf(buf, sizeof(buf), ls_tr("%lu B/s"), (unsigned long)h.bytes_per_second);
        ls_kv(sf, a, row++, "STREAM", buf, h.bytes_per_second ? val : dim);
        if (h.recoveries) {
            snprintf(buf, sizeof(buf), "%u", (unsigned)h.recoveries);
            ls_kv(sf, a, row++, "RECOVER", buf, bad);
        }
    }
    if (!present) ls_kv(sf, a, row++, "SDR", "absent", bad);

    snprintf(buf, sizeof(buf), ls_tr("%d/s"), perf_get_msgs_per_sec());
    ls_kv(sf, a, row++, "ADS-B RATE", buf, val);
    snprintf(buf, sizeof(buf), ls_tr("%d err"), perf_get_crc_err());
    ls_kv(sf, a, row++, "ADS-B CRC TOTAL", buf, perf_get_crc_err() ? dim : good);

    if (row < a.h - 3) row++;
    const size_t inter = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    snprintf(buf, sizeof(buf), ls_tr("%u KB"), (unsigned)(inter / 1024));
    ls_kv(sf, a, row++, "INTERNAL", buf, inter < 40000 ? bad : val);
    snprintf(buf, sizeof(buf), ls_tr("%u KB"),
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

static void draw_error(tui_surface *sf, tui_rect area, const char *error)
{
    ls_panel_box(sf, area, "USER APP ERROR", TUI_RED);
    int width = area.w - 4;
    if (width > 63) width = 63;
    const int rows = area.h - 4;
    const int len = (int)strlen(error), page_chars = width * rows;
    s_error_pages = (len + page_chars - 1) / page_chars;
    if (s_error_page >= s_error_pages) s_error_page = 0;
    int at = s_error_page * page_chars;
    char line[64];
    for (int row = 0; row < rows && at < len; row++) {
        int n = len - at;
        if (n > width) n = width;
        if (n >= (int)sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, error + at, n); line[n] = 0; at += n;
        tui_put_str(sf, area, area.x + 2, area.y + 2 + row, line, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    }
    tui_put_str(sf, area, area.x + 2, area.y + area.h - 2,
                ls_tr(s_error_pages > 1 ? "Tap next / ESC back" : "Tap or ESC to return"), LS_ATTR_DIM);
}

static void draw(tui_surface *sf, tui_rect area)
{
    if(area.w<24 || area.h<12) {
        ls_btn_bar_raised_slot(sf,tui_rect_make(0,0,0,0),NULL,0,-1,LS_BTN_SLOT_QUICK);
        ls_panel_notice(sf,area,"HOME","Enlarge pane","");return;
    }
    const char *error = s_group == 3 ? ls_userapp_last_error() : NULL;
    s_error_banner = tui_rect_make(0,0,0,0);
    if (s_error_detail && error) {
        ls_btn_bar_raised_slot(sf,tui_rect_make(0,0,0,0),NULL,0,-1,LS_BTN_SLOT_QUICK);
        draw_error(sf, area, error); return;
    }
    ls_tile_t *tiles = s_tiles;
    const ls_app_t **apps = s_apps;
    const int n = build_tiles(tiles, apps, MAX_TILES);
    if (s_sel >= n) s_sel = n ? n - 1 : 0;

    ls_btn_t groups[4];
    const bool news = update_known();
    for (int i = 0; i < 4; i++) groups[i] = (ls_btn_t){GROUPS[i], ls_tui_is_wide()?(const char *const[]){"F1","F2","F3","F4"}[i]:NULL, 0, s_group == i, false, news && !strcmp(GROUPS[i], "SYSTEM")};
    /* An open folder swaps the group bar for the way back out of it. */
    char count[16];
    if (s_folder >= 0) {
        snprintf(count, sizeof(count), ls_tr("%d APPS"), n);
        groups[0] = (ls_btn_t){"< BACK", "ESC", 0, false, false, false};
        groups[1] = (ls_btn_t){FOLDERS[s_folder].name, count, 0, true, false, false};
    }
    const int nb = s_folder >= 0 ? 2 : 4;
    const int bar_h = ls_btn_raised_height(area, nb);
    ls_btn_bar_raised(sf, tui_rect_make(area.x, area.y, area.w, bar_h), groups, nb, -1);
    tui_rect body=tui_rect_make(area.x,area.y+bar_h,area.w,area.h-bar_h-1);
    int status_w = 0;
    if (ls_tui_is_wide()) {
        status_w = body.w / 3; if (status_w > 34) status_w = 34;
        body.w -= status_w;
    }
    if (error) {
        s_error_banner = tui_rect_make(body.x, body.y, body.w, 3);
        tui_put_str(sf, s_error_banner, body.x + 1, body.y, ls_tr("SD app error: tap / E"), TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
        tui_put_str(sf, s_error_banner, body.x + 1, body.y + 1, error,
                    TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK));
        body.y += 3; body.h -= 3;
    }
    /* Preserve the icon, label and comfortable touch area on every tile. */
    int fit_cols = body.w / (LS_ICON_COLS + 5);
    int fit_rows = body.h / (LS_ICON_ROWS + 4);
    if (fit_cols < 1) fit_cols = 1;
    if (fit_rows < 1) fit_rows = 1;
    const int page_h = ls_tui_is_wide()?4:6;
    if (n > fit_cols * fit_rows || n > 24) {
        body.h -= page_h;
        fit_rows = body.h / (LS_ICON_ROWS + 4);
        if (fit_rows < 1) fit_rows = 1;
    }
    const int previous_capacity=s_page_tiles;
    s_page_tiles = fit_cols * fit_rows;
    if (s_page_tiles > 24) s_page_tiles = 24;
    if(previous_capacity!=s_page_tiles) s_tile_page=s_sel/s_page_tiles;
    const int pages = (n + PAGE_TILES - 1) / PAGE_TILES;
    if (s_tile_page >= pages) s_tile_page = 0;
    int first = s_tile_page * PAGE_TILES;
    int shown = n - first; if (shown > PAGE_TILES) shown = PAGE_TILES;
    if (s_sel < first || s_sel >= first + shown) s_sel = first;
    char paging[24];
    snprintf(paging,sizeof(paging),ls_tr("PAGE %d/%d"),pages?s_tile_page+1:1,pages?pages:1);
    ls_btn_t pager[]={
        {"< PREV",ls_tui_is_wide()?"F5":NULL,0,false,pages<=1},
        {paging,pages<=1?"SINGLE PAGE":"APP PAGES",0,true,true},
        {"NEXT >",ls_tui_is_wide()?"F6":NULL,0,false,pages<=1}};
    ls_btn_bar_raised_slot(sf,pages>1?tui_rect_make(area.x,area.y+area.h-page_h,area.w,page_h):tui_rect_make(0,0,0,0),pager,pages>1?3:0,-1,LS_BTN_SLOT_QUICK);
    if (status_w) {
        /* A camera hole on the left edge gets the text panel, which bends
           round it; a tile under it would lose its picture. */
        tui_rect hole;
        const bool hole_left = ls_tui_cutout(&hole) && hole.x == 0 &&
                               hole.y < body.y + body.h &&
                               hole.y + hole.h > body.y;
        tui_rect right = tui_rect_make(body.x + body.w, body.y, status_w, body.h);
        if (hole_left) {
            /* Mirrored, with the same two columns of air the tiles leave
               on the other side. */
            right = tui_rect_make(body.x, body.y, status_w - 2, body.h);
            body.x += status_w;
        }
        draw_status(sf, right);
    }
    ls_tile_grid(sf, body, tiles + first, shown, s_sel - first);
    if (!shown) ls_panel_notice(sf, body, GROUPS[s_group], error ? "SD app rejected" : "No apps in this group", error ? "Tap error above for details" : "User apps load from the SD card");

}

/* A folder tile opens the folder; every other tile opens its app. */
static void open_tile(const ls_app_t *app, int more)
{
    if (more) { s_folder = folder_of(app->id); s_sel = s_tile_page = 0; return; }
    for (int i = 0; i < ls_app_count(); i++)
        if (ls_app_at(i) == app) { ls_app_open(i); break; }
}

static bool key(ls_tk_t k, char ch)
{
    if (s_error_detail) {
        if (k == LS_TK_UP && s_error_page > 0) s_error_page--;
        else if (k == LS_TK_DOWN && s_error_page + 1 < s_error_pages) s_error_page++;
        else if (k == LS_TK_ESC || k == LS_TK_ENTER) s_error_detail = false;
        return true;
    }
    if (s_group == 3 && k == LS_TK_CHAR && (ch == 'e' || ch == 'E') && ls_userapp_last_error()) {
        s_error_detail = true; s_error_page = 0; return true;
    }
    ls_tile_t *tiles = s_tiles;
    const ls_app_t **apps = s_apps;
    const int n = build_tiles(tiles, apps, MAX_TILES);
    int cols = 1, rows = 1;
    ls_tile_shape(&cols, &rows);
    if (cols < 1) cols = 1;

    if (k>=LS_TK_F1 && k<=LS_TK_F4) {
        s_group=k-LS_TK_F1; s_tile_page=s_sel=0; s_folder=-1; return true;
    }
    if (k==LS_TK_ESC && s_folder>=0) {
        /* Back on the folder's own tile, where the operator left. */
        const char *lead=FOLDERS[s_folder].lead;
        s_folder=-1; s_sel=0;
        const int m=build_tiles(tiles,apps,MAX_TILES);
        for(int i=0;i<m;i++) if(!strcmp(apps[i]->id,lead)) s_sel=i;
        s_tile_page=s_sel/PAGE_TILES; return true;
    }
    if (k==LS_TK_F5 || k==LS_TK_F6) {
        int pages=(n+PAGE_TILES-1)/PAGE_TILES;
        if(pages>1) s_tile_page=(s_tile_page+pages+(k==LS_TK_F5?-1:1))%pages;
        s_sel=s_tile_page*PAGE_TILES; return true;
    }
    if (k==LS_TK_CHAR && ch>='1' && ch<='9') {
        int target=s_tile_page*PAGE_TILES+ch-'1';
        if(target<n && ch-'1'<PAGE_TILES) { s_sel=target; return key(LS_TK_ENTER,0); }
        return true;
    }
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
        if (s_sel < n) open_tile(apps[s_sel], tiles[s_sel].more);
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
    if (s_error_detail) {
        if (s_error_page + 1 < s_error_pages) s_error_page++;
        else s_error_detail = false;
        return true;
    }
    if (s_error_banner.w && col >= s_error_banner.x && col < s_error_banner.x + s_error_banner.w &&
        row >= s_error_banner.y && row < s_error_banner.y + s_error_banner.h) {
        s_error_detail = true; s_error_page = 0; return true;
    }
    const int group = ls_btn_hit(col, row);
    if (s_folder >= 0 && group >= 0) return group ? true : key(LS_TK_ESC, 0);
    if (group >= 0 && group < 4) { s_group = group; s_tile_page = s_sel = 0; return true; }
    const int page=ls_btn_hit_slot(col,row,LS_BTN_SLOT_QUICK);
    if(page>=0) {if(page==0)return key(LS_TK_F5,0);if(page==2)return key(LS_TK_F6,0);return true;}
    const int hit = ls_tile_hit(col, row);
    if (hit < 0) return true;
    const int i = hit + s_tile_page * PAGE_TILES;

    ls_tile_t *tiles = s_tiles;
    const ls_app_t **apps = s_apps;
    const int n = build_tiles(tiles, apps, MAX_TILES);
    if (i >= n) return true;

    s_sel = i;
    open_tile(apps[i], tiles[i].more);
    return true;
}

const ls_tui_screen_t ls_scr_home = {
    .diff_switch = true,
    .name = "HOME",
    .hint = "F1-F4 group  1-9 open  F5/F6 page  ESC back",
    .enter = NULL,
    .leave = NULL,
    .draw = draw,
    .key = key,
    .touch = touch,
};
