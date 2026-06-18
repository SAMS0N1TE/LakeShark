#pragma once

#include "lvgl.h"
#include "shell/ls_app.hpp"
#include "mesh_gateway.h"

class MeshController : public LsApp {
public:
    MeshController();
    ~MeshController();

    bool run(lv_obj_t *parent) override;
    bool back(void) override;
    bool close(void) override;

    bool init(void) override;
    bool pause(void) override;
    bool resume(void) override;

private:
    void buildUi(lv_obj_t *parent);
    void buildRange(lv_obj_t *screen);
    void refresh(void);
    void refreshRange(void);
    void openRange(uint16_t addr);
    void closeRange(void);

    static void timerCb(lv_timer_t *t);
    static void onRescanBtn(lv_event_t *e);
    static void onResetBtn(lv_event_t *e);
    static void onResetMsgbox(lv_event_t *e);
    static void onTableClick(lv_event_t *e);
    static void onRangeBack(lv_event_t *e);

    lv_obj_t   *_screen;
    lv_obj_t   *_state_lbl;
    lv_obj_t   *_radio_lbl;
    lv_obj_t   *_stats_lbl;
    lv_obj_t   *_table;
    lv_timer_t *_timer;

    uint16_t    _row_addr[MESH_MAX_ROUTES];
    uint32_t    _row_count;

    lv_obj_t   *_range;
    uint16_t    _range_addr;
    lv_obj_t   *_r_title;
    lv_obj_t   *_r_rssi;
    lv_obj_t   *_r_snr;
    lv_obj_t   *_r_margin_bar;
    lv_obj_t   *_r_margin_lbl;
    lv_obj_t   *_chart;
    lv_chart_series_t *_series;
    lv_coord_t  _ychart[MESH_RSSI_HISTORY];
};
