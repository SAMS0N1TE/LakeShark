#include "AppMap.hpp"
#include "shell/ls_shell.hpp"

#include <cstdio>
#include <cstring>
#include <cmath>

extern "C" {
#include "app_registry.h"
#include "settings.h"
#include "lakeshark_backend.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "map_tiles.h"
#include "map_tile_status.h"
#include "map_wpt_select.h"
}

#include "map_tiles.h"

#include "sdr_ui/sdr_ui.h"
#include "ui/ls_ui.h"

#define COL_LABEL   SDR_LABEL
#define COL_TEXT    SDR_TEXT
#define COL_DIM     SDR_DIM
#define COL_CYAN    SDR_CYAN
#define COL_GREEN   SDR_GREEN
#define COL_AMBER   SDR_AMBER
#define COL_GOLD    SDR_GOLD
#define COL_RED     SDR_RED
#define COL_PANEL   SDR_PANEL

static const char *TAG_NS  = "sdr-tool";
static const char *WPT_KEY = "waypoints";

/**/
/* Range in nautical miles. NM because everything ADS-B reports is in NM and
   feet, and converting for display invites the kind of unit slip that is very
   hard to spot on a map. */
static const int RANGE_NM[] = { 5, 10, 25, 50, 100, 200 };
static const int RANGE_N    = (int)(sizeof(RANGE_NM) / sizeof(RANGE_NM[0]));

static map_wpt_t s_wpt[MAP_MAX_WPT];
static bool      s_wpt_loaded = false;

/**/

static void wpt_load(void)
{
    if (s_wpt_loaded) return;
    s_wpt_loaded = true;
    memset(s_wpt, 0, sizeof(s_wpt));
    nvs_handle_t h;
    if (nvs_open(TAG_NS, NVS_READWRITE, &h) != ESP_OK) return;
    size_t sz = sizeof(s_wpt);
    nvs_get_blob(h, WPT_KEY, s_wpt, &sz);
    nvs_close(h);
}

static void wpt_save(void)
{
    nvs_handle_t h;
    if (nvs_open(TAG_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, WPT_KEY, s_wpt, sizeof(s_wpt));
    nvs_commit(h);
    nvs_close(h);
}

/**/
/* Flat-earth projection, which is correct to well under a pixel at these
   ranges and avoids great-circle trig on every frame. 1 degree of latitude is
   60 NM; longitude shrinks by cos(lat), and NOT applying that would overstate
   east-west distance by ~18% at this latitude. */
static void ll_delta_nm(float lat0, float lon0, float lat, float lon,
                        float *dn, float *de)
{
    float dlat = lat - lat0;
    float dlon = lon - lon0;
    *dn = dlat * 60.0f;
    *de = dlon * 60.0f * cosf(lat0 * (float)M_PI / 180.0f);
}

static float nm_dist(float dn, float de) { return sqrtf(dn * dn + de * de); }

static int nm_bearing(float dn, float de)
{
    float b = atan2f(de, dn) * 180.0f / (float)M_PI;
    if (b < 0) b += 360.0f;
    return (int)(b + 0.5f);
}

static lv_obj_t *mono(lv_obj_t *parent, lv_color_t col)
{
    return sdr_label(parent, sdr_font_mono(), col);
}

static lv_obj_t *mk_dot(lv_obj_t *parent, lv_color_t col, int sz)
{
    lv_obj_t *d = lv_obj_create(parent);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, sz, sz);
    ls_ui_style_circle(d, LS_UI_COLOR_ACCENT, false);
    lv_obj_set_style_bg_color(d, col, 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    return d;
}

AppMap::AppMap() : LsApp("MAP", "map") {}
AppMap::~AppMap() = default;

bool AppMap::pause(void)      { if (_timer) lv_timer_pause(_timer);  return true; }
bool AppMap::background(void) { if (_timer) lv_timer_pause(_timer);  return true; }
bool AppMap::resume(void)     { if (_timer) lv_timer_resume(_timer); return true; }
bool AppMap::back(void)       { return exitToLauncher(); }

bool AppMap::close(void)
{
    if (_timer) { lv_timer_del(_timer); _timer = nullptr; }
    /* stop image reads before releasing their backing tile cache.
     * The shell deletes this now-hidden tree in the same GUI callback. */
    for (int i = 0; i < MAP_TILE_SLOTS; ++i) {
        if (_tile_img[i]) lv_obj_add_flag(_tile_img[i], LV_OBJ_FLAG_HIDDEN);
        lv_img_cache_invalidate_src(&_tile_dsc[i]);
        _tile_img[i] = nullptr;
        _tile_dsc[i].data = nullptr;
    }
    map_tiles_deinit();
    _tiles_ok = false;
    _tabview = nullptr;
    _plot = nullptr; _hdr = nullptr; _sel_lbl = nullptr; _range_lbl = nullptr;
    _wpt_tbl = nullptr; _wpt_info = nullptr; _home_dot = nullptr;
    _tile_lbl = nullptr; _last_tile_state = -1;
    for (int i = 0; i < 3; i++) _ring[i] = nullptr;
    for (int i = 0; i < LAKESHARK_ADSB_MAX; i++) _dot[i] = nullptr;
    for (int i = 0; i < MAP_MAX_WPT; i++) _wdot[i] = nullptr;
    return true;
}

bool AppMap::run(lv_obj_t *parent)
{
    wpt_load();
    _home_set = settings_get_home(&_home_lat, &_home_lon);

    ls_ui_screen_t screen;
    ls_ui_screen_create(parent, "MAP", true, LS_UI_COLOR_ID_GREEN, &screen);
    _screen_readout = screen.readout;
    _screen_lamp = screen.lamp;
    ls_ui_screen_set_readout(&screen, "NAV");
    _tabview = screen.tabs;

    buildMapTab(ls_ui_screen_add_tab(&screen, "MAP"));
    buildWptTab(ls_ui_screen_add_tab(&screen, "WPT"));

    _timer = lv_timer_create(timerCb, 1000, this);
    return true;
}

void AppMap::buildMapTab(lv_obj_t *parent)
{
    ls_ui_style_content(parent);

    _hdr = mono(parent, COL_LABEL);
    lv_obj_set_width(_hdr, lv_pct(100));
    lv_label_set_text(_hdr, "home unset");

    /* The plot itself. Square-ish so a mile north is a mile east. */
    _plot = lv_obj_create(parent);
    lv_obj_set_width(_plot, lv_pct(100));
    lv_obj_set_flex_grow(_plot, 1);
    ls_ui_style_plot(_plot);
    lv_obj_clear_flag(_plot, LV_OBJ_FLAG_SCROLLABLE);

    /**/
    /* Tile images are created FIRST so every overlay object below is later in
       the child list and therefore drawn on top. LVGL has no z-index; order is
       the z-order. */
    _tiles_ok = map_tiles_init();
    for (int i = 0; i < 9; i++) {
        lv_obj_t *im = lv_img_create(_plot);
        lv_obj_add_flag(im, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(im, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        _tile_img[i] = im;
        _tile_dsc[i].header.always_zero = 0;
        _tile_dsc[i].header.w  = MAP_TILE_PX;
        _tile_dsc[i].header.h  = MAP_TILE_PX;
        _tile_dsc[i].header.cf = LV_IMG_CF_TRUE_COLOR;
        _tile_dsc[i].data_size = MAP_TILE_PX * MAP_TILE_PX * 2;
        _tile_dsc[i].data      = nullptr;
    }

    for (int i = 0; i < 3; i++) {
        lv_obj_t *r = lv_obj_create(_plot);
        lv_obj_remove_style_all(r);
        ls_ui_style_circle(r, LS_UI_COLOR_DIM_TEXT, true);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        _ring[i] = r;
    }

    for (int i = 0; i < MAP_MAX_WPT; i++) _wdot[i] = mk_dot(_plot, COL_GOLD, 7);
    for (int i = 0; i < LAKESHARK_ADSB_MAX; i++) _dot[i] = mk_dot(_plot, COL_GREEN, 9);
    _home_dot = mk_dot(_plot, COL_CYAN, 11);

    /**/
    /* Status overlay: top-left of the plot so it sits clear of the centre
       home dot and the aircraft rings.  Semi-opaque background so the text
       stays legible over a rendered tile.  Created after all data objects
       so it draws on top; hidden by default and populated by updateTiles. */
    _tile_lbl = sdr_label(_plot, sdr_font_mono(), COL_AMBER);
    ls_ui_style_overlay(_tile_lbl);
    lv_obj_align(_tile_lbl, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_obj_add_flag(_tile_lbl, LV_OBJ_FLAG_HIDDEN);

    _sel_lbl = mono(parent, COL_TEXT);
    lv_obj_set_width(_sel_lbl, lv_pct(100));
    lv_label_set_text(_sel_lbl, "no aircraft with position");

    lv_obj_t *row = ls_ui_controls(parent);
    ls_ui_button(row, "RANGE", LS_BTN_DEFAULT, rangeCb, this, &_range_lbl);
    ls_ui_button(row, "NEXT AC", LS_BTN_DEFAULT, selNextCb, this, nullptr);
    ls_ui_button(row, "SET HOME", LS_BTN_PRIMARY, centreCb, this, nullptr);
}

void AppMap::buildWptTab(lv_obj_t *parent)
{
    ls_ui_style_content(parent);

    _wpt_info = mono(parent, COL_LABEL);
    lv_obj_set_width(_wpt_info, lv_pct(100));
    lv_label_set_text(_wpt_info, "waypoints");

    _wpt_tbl = lv_table_create(parent);
    lv_obj_set_width(_wpt_tbl, lv_pct(100));
    lv_obj_set_flex_grow(_wpt_tbl, 1);
    lv_table_set_col_cnt(_wpt_tbl, 3);
    lv_table_set_row_cnt(_wpt_tbl, MAP_MAX_WPT + 1);
    lv_table_set_col_width(_wpt_tbl, 0, 130);
    lv_table_set_col_width(_wpt_tbl, 1, 150);
    lv_table_set_col_width(_wpt_tbl, 2, 150);
    lv_table_set_cell_value(_wpt_tbl, 0, 0, "NAME");
    lv_table_set_cell_value(_wpt_tbl, 0, 1, "BRG / DIST");
    lv_table_set_cell_value(_wpt_tbl, 0, 2, "POSITION");
    /**/
    /* Without this the table swallowed taps: _wsel only ever changed when
       MARK HERE wrote into a fresh slot, so after a reboot GOTO and DEL
       could target nothing but slot 0.  See map_wpt_select.c for why the
       classifier lives in its own file. */
    lv_obj_add_event_cb(_wpt_tbl, wptRowCb, LV_EVENT_VALUE_CHANGED, this);

    lv_obj_t *row = ls_ui_controls(parent);
    ls_ui_button(row, "MARK HERE", LS_BTN_PRIMARY, wptAddCb, this, nullptr);
    ls_ui_button(row, "GOTO", LS_BTN_PRIMARY, wptGotoCb, this, nullptr);
    ls_ui_button(row, "DEL", LS_BTN_DANGER, wptDelCb, this, nullptr);
}

bool AppMap::project(float lat, float lon, int *out_x, int *out_y) const
{
    if (!_home_set || _plot_w <= 0) return false;
    float dn = 0, de = 0;
    ll_delta_nm(_home_lat, _home_lon, lat, lon, &dn, &de);
    float rng = (float)RANGE_NM[_range_idx];
    float d   = nm_dist(dn, de);
    if (d > rng) return false;

    int cx = _plot_w / 2, cy = _plot_h / 2;
    int rad = (cx < cy ? cx : cy) - 8;
    *out_x = cx + (int)((de / rng) * rad);
    *out_y = cy - (int)((dn / rng) * rad);   /* north is up */
    return true;
}

void AppMap::updateMap(void)
{
    if (!_plot) return;

    _plot_w = lv_obj_get_width(_plot);
    _plot_h = lv_obj_get_height(_plot);
    int cx = _plot_w / 2, cy = _plot_h / 2;
    int rad = (cx < cy ? cx : cy) - 8;
    if (rad < 10) return;                    /* not laid out yet */

    for (int i = 0; i < 3; i++) {
        int rr = rad * (i + 1) / 3;
        lv_obj_set_size(_ring[i], rr * 2, rr * 2);
        lv_obj_set_pos(_ring[i], cx - rr, cy - rr);
    }

    if (_home_set) {
        lv_obj_clear_flag(_home_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(_home_dot, cx - 5, cy - 5);
    } else {
        lv_obj_add_flag(_home_dot, LV_OBJ_FLAG_HIDDEN);
    }

    char buf[96];
    int rng = RANGE_NM[_range_idx];
    if (_range_lbl) {
        snprintf(buf, sizeof(buf), "%d NM", rng);
        lv_label_set_text(_range_lbl, buf);
    }
    snprintf(buf, sizeof(buf), "%d NM", rng);
    ls_ui_readout_set(_screen_readout, buf);
    ls_ui_lamp_set(_screen_lamp, _home_set, LS_UI_COLOR_ACCENT);

    /**/
    updateTiles();

    /* Waypoints first, so aircraft draw over them. */
    int wshown = 0;
    for (int i = 0; i < MAP_MAX_WPT; i++) {
        int x, y;
        if (s_wpt[i].used && project(s_wpt[i].lat, s_wpt[i].lon, &x, &y)) {
            lv_obj_clear_flag(_wdot[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(_wdot[i], x - 3, y - 3);
            wshown++;
        } else {
            lv_obj_add_flag(_wdot[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    lakeshark_adsb_tel_t tel;
    memset(&tel, 0, sizeof(tel));
    lakeshark_adsb_telemetry(&tel);

    int shown = 0, with_pos = 0;
    for (int i = 0; i < LAKESHARK_ADSB_MAX; i++) {
        lakeshark_adsb_ac_t ac;
        bool have = (i < tel.n_aircraft) && lakeshark_adsb_aircraft_at(i, &ac);
        int x, y;
        if (have && ac.pos_valid) {
            with_pos++;
            if (project(ac.lat, ac.lon, &x, &y)) {
                lv_obj_clear_flag(_dot[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(_dot[i], x - 4, y - 4);
                lv_obj_set_style_bg_color(_dot[i],
                    (i == _sel) ? COL_AMBER : COL_GREEN, 0);
                shown++;
                continue;
            }
        }
        lv_obj_add_flag(_dot[i], LV_OBJ_FLAG_HIDDEN);
    }

    if (_hdr) {
        if (!_home_set) {
            snprintf(buf, sizeof(buf),
                     "LOCATION UNSET - SET HOME opens Settings. tracked %d",
                     tel.n_aircraft);
        } else {
            snprintf(buf, sizeof(buf),
                     "home %.4f,%.4f  rng %d NM  ac %d/%d shown %d  wpt %d",
                     (double)_home_lat, (double)_home_lon, rng,
                     with_pos, tel.n_aircraft, shown, wshown);
        }
        lv_label_set_text(_hdr, buf);
    }

    /* Selected aircraft, or the GOTO waypoint if one is set. */
    if (_sel_lbl) {
        lakeshark_adsb_ac_t ac;
        if (_goto >= 0 && s_wpt[_goto].used && _home_set) {
            float dn, de;
            ll_delta_nm(_home_lat, _home_lon, s_wpt[_goto].lat, s_wpt[_goto].lon,
                        &dn, &de);
            snprintf(buf, sizeof(buf), "GOTO %-12s  %03d deg  %.2f NM",
                     s_wpt[_goto].name, nm_bearing(dn, de),
                     (double)nm_dist(dn, de));
        } else if (_sel >= 0 && _sel < tel.n_aircraft &&
                   lakeshark_adsb_aircraft_at(_sel, &ac) && ac.pos_valid && _home_set) {
            float dn, de;
            ll_delta_nm(_home_lat, _home_lon, ac.lat, ac.lon, &dn, &de);
            snprintf(buf, sizeof(buf), "%-8s %5d ft  %03d deg  %.1f NM  hdg %03d",
                     ac.callsign[0] ? ac.callsign : "-------",
                     ac.altitude, nm_bearing(dn, de),
                     (double)nm_dist(dn, de), ac.heading);
        } else if (with_pos == 0) {
            snprintf(buf, sizeof(buf),
                     "No aircraft positions. Receive ADS-B first; MAP shows stored positions.");
        } else {
            snprintf(buf, sizeof(buf), "%d with position - NEXT AC to select",
                     with_pos);
        }
        lv_label_set_text(_sel_lbl, buf);
    }
}

/**/
/* Draw the tile grid so that HOME sits exactly at the centre of the plot. */

void AppMap::updateTiles(void)
{
    /* Pick the zoom whose scale best fits the selected range. A tile at zoom z
       spans 360/2^z degrees of longitude; empirically z13-14 suits 5-10 NM and
       z11-12 suits 25-50. Walk down from the finest pack we actually have. */
    int want = 14;
    int rng  = RANGE_NM[_range_idx];
    if      (rng <= 5)   want = 14;
    else if (rng <= 10)  want = 13;
    else if (rng <= 25)  want = 12;
    else if (rng <= 50)  want = 11;
    else                 want = 10;
    while (want > 6 && !map_tiles_have_zoom(want)) want--;
    _zoom = want;

    map_tile_probe_t probe;
    memset(&probe, 0, sizeof(probe));
    probe.renderer_ok = _tiles_ok;
    probe.home_set    = _home_set;
    probe.sd_mounted  = map_tiles_have_sd();
    probe.pack_present = map_tiles_have_pack();
    probe.zoom_present = map_tiles_have_zoom(_zoom);

    int drawn = 0;

    if (!_tiles_ok || !_home_set) {
        for (int i = 0; i < 9; i++)
            if (_tile_img[i]) lv_obj_add_flag(_tile_img[i], LV_OBJ_FLAG_HIDDEN);
    } else {
        double cx_t = map_lon2tilex(_home_lon, _zoom);
        double cy_t = map_lat2tiley(_home_lat, _zoom);
        int    tx0  = (int)cx_t, ty0 = (int)cy_t;
        /* Pixel offset of home inside its own tile. */
        int    ox   = (int)((cx_t - tx0) * MAP_TILE_PX);
        int    oy   = (int)((cy_t - ty0) * MAP_TILE_PX);

        int cx = _plot_w / 2, cy = _plot_h / 2;
        int n = 0;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++, n++) {
                lv_obj_t *im = _tile_img[n];
                if (!im) continue;
                int tx = tx0 + dx, ty = ty0 + dy;
                if (map_tiles_have(_zoom, tx, ty)) probe.files_present++;
                const uint8_t *rgb = map_tiles_get(_zoom, tx, ty);
                if (!rgb) { lv_obj_add_flag(im, LV_OBJ_FLAG_HIDDEN); continue; }
                _tile_dsc[n].data = rgb;
                lv_img_set_src(im, &_tile_dsc[n]);
                lv_obj_set_pos(im,
                               cx - ox + dx * MAP_TILE_PX,
                               cy - oy + dy * MAP_TILE_PX);
                lv_obj_clear_flag(im, LV_OBJ_FLAG_HIDDEN);
                drawn++;
            }
        }
    }
    probe.drawn = drawn;

    map_tile_state_t st = map_tile_classify(&probe);
    if (_tile_lbl) {
        if (st == MAP_TILE_STATE_OK) {
            lv_obj_add_flag(_tile_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            /* NO_HOME is already shouted by _hdr's "HOME UNSET" line, and
               doubling it in the overlay just adds noise.  Leave the label
               hidden and let the header carry the message. */
            if (st == MAP_TILE_STATE_NO_HOME) {
                lv_obj_add_flag(_tile_lbl, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_label_set_text(_tile_lbl, map_tile_state_text(st));
                lv_obj_clear_flag(_tile_lbl, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    _last_tile_state = (int)st;
}

void AppMap::updateWpt(void)
{
    if (!_wpt_tbl) return;
    char buf[64];
    int used = 0;
    for (int i = 0; i < MAP_MAX_WPT; i++) {
        int row = i + 1;
        if (!s_wpt[i].used) {
            lv_table_set_cell_value(_wpt_tbl, row, 0, (i == _wsel) ? ">  --" : "   --");
            lv_table_set_cell_value(_wpt_tbl, row, 1, "");
            lv_table_set_cell_value(_wpt_tbl, row, 2, "");
            continue;
        }
        used++;
        snprintf(buf, sizeof(buf), "%s%-12s",
                 (i == _wsel) ? "> " : "  ", s_wpt[i].name);
        lv_table_set_cell_value(_wpt_tbl, row, 0, buf);

        if (_home_set) {
            float dn, de;
            ll_delta_nm(_home_lat, _home_lon, s_wpt[i].lat, s_wpt[i].lon, &dn, &de);
            snprintf(buf, sizeof(buf), "%03d deg  %.2f NM",
                     nm_bearing(dn, de), (double)nm_dist(dn, de));
        } else {
            snprintf(buf, sizeof(buf), "home unset");
        }
        lv_table_set_cell_value(_wpt_tbl, row, 1, buf);

        snprintf(buf, sizeof(buf), "%.4f, %.4f",
                 (double)s_wpt[i].lat, (double)s_wpt[i].lon);
        lv_table_set_cell_value(_wpt_tbl, row, 2, buf);
    }
    if (_wpt_info) {
        snprintf(buf, sizeof(buf), "%d/%d stored%s", used, MAP_MAX_WPT,
                 _goto >= 0 ? "   GOTO active" : "");
        lv_label_set_text(_wpt_info, buf);
    }
}

void AppMap::timerCb(lv_timer_t *t)
{
    AppMap *self = static_cast<AppMap *>(t->user_data);
    if (!self) return;
    self->_home_set = settings_get_home(&self->_home_lat, &self->_home_lon);
    self->updateMap();
    self->updateWpt();
}

void AppMap::rangeCb(lv_event_t *e)
{
    AppMap *self = static_cast<AppMap *>(lv_event_get_user_data(e));
    if (!self) return;
    self->_range_idx = (self->_range_idx + 1) % RANGE_N;
    self->updateMap();
}

void AppMap::selNextCb(lv_event_t *e)
{
    AppMap *self = static_cast<AppMap *>(lv_event_get_user_data(e));
    if (!self) return;
    lakeshark_adsb_tel_t tel;
    memset(&tel, 0, sizeof(tel));
    lakeshark_adsb_telemetry(&tel);
    if (tel.n_aircraft <= 0) { self->_sel = -1; return; }
    /* Step to the next aircraft that actually has a position - selecting one
       without a fix would just blank the readout. */
    for (int step = 1; step <= tel.n_aircraft; step++) {
        int cand = (self->_sel + step) % tel.n_aircraft;
        lakeshark_adsb_ac_t ac;
        if (lakeshark_adsb_aircraft_at(cand, &ac) && ac.pos_valid) {
            self->_sel  = cand;
            self->_goto = -1;      /* aircraft selection cancels GOTO */
            self->updateMap();
            return;
        }
    }
}

/**/
/* Edit the user's reference explicitly; an aircraft is not their location. */
void AppMap::centreCb(lv_event_t *e)
{
    (void)e;
    LsShell::instance().launchByName("Settings");
}

void AppMap::wptAddCb(lv_event_t *e)
{
    AppMap *self = static_cast<AppMap *>(lv_event_get_user_data(e));
    if (!self || !self->_home_set) return;
    for (int i = 0; i < MAP_MAX_WPT; i++) {
        if (s_wpt[i].used) continue;
        s_wpt[i].used = 1;
        s_wpt[i].lat  = self->_home_lat;
        s_wpt[i].lon  = self->_home_lon;
        snprintf(s_wpt[i].name, MAP_WPT_NAME, "WPT%d", i + 1);
        self->_wsel = i;
        wpt_save();
        self->updateWpt();
        self->updateMap();
        return;
    }
}

void AppMap::wptDelCb(lv_event_t *e)
{
    AppMap *self = static_cast<AppMap *>(lv_event_get_user_data(e));
    if (!self) return;
    int i = self->_wsel;
    if (i < 0 || i >= MAP_MAX_WPT || !s_wpt[i].used) return;
    memset(&s_wpt[i], 0, sizeof(s_wpt[i]));
    if (self->_goto == i) self->_goto = -1;
    wpt_save();
    self->updateWpt();
    self->updateMap();
}

void AppMap::wptGotoCb(lv_event_t *e)
{
    AppMap *self = static_cast<AppMap *>(lv_event_get_user_data(e));
    if (!self) return;
    int i = self->_wsel;
    if (i < 0 || i >= MAP_MAX_WPT || !s_wpt[i].used) return;
    self->_goto = (self->_goto == i) ? -1 : i;
    self->_sel  = -1;
    self->updateWpt();
    self->updateMap();
}

/**/
/* LVGL fires VALUE_CHANGED on the table when the tap lands on a new cell.
   The classifier in map_wpt_select.c rejects the header row and any empty
   slot, so a tap on those leaves _wsel where it was; only a populated row
   promotes to the new selection.  Both updateWpt() and updateMap() run so
   the `>` prefix and any range/dot rendering reflect the change. */
void AppMap::wptRowCb(lv_event_t *e)
{
    AppMap *self = static_cast<AppMap *>(lv_event_get_user_data(e));
    if (!self || !self->_wpt_tbl) return;
    uint16_t row = LV_TABLE_CELL_NONE, col = LV_TABLE_CELL_NONE;
    lv_table_get_selected_cell(self->_wpt_tbl, &row, &col);
    uint8_t used[MAP_MAX_WPT];
    for (int i = 0; i < MAP_MAX_WPT; i++) used[i] = s_wpt[i].used;
    int slot = map_wpt_row_to_slot((int)row, used, MAP_MAX_WPT);
    if (slot < 0) return;
    self->_wsel = slot;
    self->updateWpt();
    self->updateMap();
}
