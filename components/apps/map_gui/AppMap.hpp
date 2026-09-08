#pragma once

/*LS-737*/
/* Map / navigator. Phase 1+2: ADS-B plotted in real range and bearing around a
   stored home position, plus waypoints. Deliberately NOT included yet: offline
   tiles (needs a tile format decision and is its own project) and any claim to
   know where you are beyond what you typed in. */

#include "lvgl.h"
#include "shell/ls_app.hpp"

#define MAP_MAX_WPT 16
#define MAP_WPT_NAME 13

typedef struct {
    char  name[MAP_WPT_NAME];
    float lat;
    float lon;
    uint8_t used;
} map_wpt_t;

class AppMap : public LsApp {
public:
    AppMap();
    ~AppMap();

    bool run(lv_obj_t *parent) override;
    bool back(void) override;
    bool close(void) override;
    bool pause(void) override;
    bool resume(void) override;
    bool background(void) override;

private:
    lv_obj_t *_screen_readout = nullptr;
    lv_obj_t *_screen_lamp = nullptr;
    void buildMapTab(lv_obj_t *parent);
    void buildWptTab(lv_obj_t *parent);
    void updateMap(void);
    void updateWpt(void);

    static void timerCb(lv_timer_t *t);
    static void rangeCb(lv_event_t *e);
    static void selNextCb(lv_event_t *e);
    static void centreCb(lv_event_t *e);
    static void wptAddCb(lv_event_t *e);
    static void wptDelCb(lv_event_t *e);
    static void wptGotoCb(lv_event_t *e);
    static void wptRowCb(lv_event_t *e);

    /* Screen position for a lat/lon, relative to home. Returns false when it
       falls outside the current range ring. */
    bool project(float lat, float lon, int *out_x, int *out_y) const;

    lv_timer_t *_timer   = nullptr;
    lv_obj_t   *_tabview = nullptr;

    /*LS-740*/
    lv_obj_t     *_tile_img[9] = {nullptr};
    lv_img_dsc_t  _tile_dsc[9] = {};
    int           _zoom = 13;
    bool          _tiles_ok = false;
    void          updateTiles(void);

    /*LS-764*/
    /* Overlay label parented to _plot, positioned in the top-left so it stays
       clear of the centre home dot and the aircraft rings.  Hidden when tiles
       are drawing normally; shown with a short reason otherwise so a blank
       backdrop can no longer be mistaken for a renderer crash. */
    lv_obj_t     *_tile_lbl = nullptr;
    int           _last_tile_state = -1;

    lv_obj_t *_plot     = nullptr;
    lv_obj_t *_ring[3]  = {nullptr, nullptr, nullptr};
    lv_obj_t *_home_dot = nullptr;
    lv_obj_t *_dot[16]  = {nullptr};
    lv_obj_t *_wdot[MAP_MAX_WPT] = {nullptr};
    lv_obj_t *_hdr      = nullptr;
    lv_obj_t *_sel_lbl  = nullptr;
    lv_obj_t *_range_lbl = nullptr;

    lv_obj_t *_wpt_tbl  = nullptr;
    lv_obj_t *_wpt_info = nullptr;

    int   _plot_w = 0, _plot_h = 0;
    int   _range_idx = 3;      /* index into RANGE_NM */
    int   _sel = -1;           /* selected aircraft index */
    int   _wsel = 0;           /* selected waypoint slot */
    int   _goto = -1;          /* waypoint being navigated to, -1 none */
    float _home_lat = 0, _home_lon = 0;
    bool  _home_set = false;
};
