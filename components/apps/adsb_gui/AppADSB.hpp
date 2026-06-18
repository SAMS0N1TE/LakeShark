
#pragma once

#include "lvgl.h"
#include "shell/ls_app.hpp"
#include "sdr_ui/sdr_ui.h"

class AppADSB : public LsApp {
public:
    AppADSB();
    ~AppADSB();

    bool run(lv_obj_t *parent) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;
    bool pause(void) override;
    bool resume(void) override;
    void switchTab(int delta) override;

private:
    void buildListTab(lv_obj_t *parent);
    void buildTrackTab(lv_obj_t *parent);
    void buildDiagTab(lv_obj_t *parent);
    void buildRadarTab(lv_obj_t *parent);
    void buildSettingsTab(lv_obj_t *parent);

    void updateList(void);
    void updateTrack(void);
    void updateDiag(void);
    void updateRadar(void);
    void updateSettings(void);

    static void timerCb(lv_timer_t *t);
    static void tableClickCb(lv_event_t *e);
    static void testBtnCb(lv_event_t *e);
    static void prevBtnCb(lv_event_t *e);
    static void nextBtnCb(lv_event_t *e);
    static void gainBtnCb(lv_event_t *e);
    static void agcBtnCb(lv_event_t *e);

    static void setGainStepCb(lv_event_t *e);
    static void setAgcCb(lv_event_t *e);
    static void presetLeftCb(lv_event_t *e);
    static void presetRightCb(lv_event_t *e);
    static void lpLeftCb(lv_event_t *e);
    static void lpRightCb(lv_event_t *e);
    static void shelfLeftCb(lv_event_t *e);
    static void shelfRightCb(lv_event_t *e);
    static void voiceTestCb(lv_event_t *e);
    static void newCycleCb(lv_event_t *e);
    static void lostCycleCb(lv_event_t *e);
    static void posCycleCb(lv_event_t *e);
    static void volDownCb(lv_event_t *e);
    static void volUpCb(lv_event_t *e);
    static void volSliderCb(lv_event_t *e);
    static void muteCb(lv_event_t *e);
    static void cartoCb(lv_event_t *e);

    lv_obj_t  *_tabview   = nullptr;
    lv_timer_t *_timer    = nullptr;

    lv_obj_t *_list_hdr   = nullptr;
    lv_obj_t *_list_table = nullptr;
    lv_obj_t *_gain_lbl   = nullptr;

    uint32_t  _row_icao[20] = {0};

    lv_obj_t *_trk_hdr    = nullptr;
    lv_obj_t *_trk_pos    = nullptr;
    lv_obj_t *_trk_motion = nullptr;
    lv_obj_t *_trk_qual   = nullptr;
    lv_obj_t *_trk_types  = nullptr;
    lv_obj_t *_trk_chart  = nullptr;
    lv_chart_series_t *_trk_series = nullptr;

    lv_obj_t *_diag_hdr    = nullptr;
    lv_obj_t *_diag_inbar  = nullptr;
    lv_obj_t *_diag_inlbl  = nullptr;
    lv_obj_t *_diag_chart  = nullptr;
    lv_chart_series_t *_s_bursts = nullptr;
    lv_chart_series_t *_s_good   = nullptr;
    lv_chart_series_t *_s_mag    = nullptr;
    lv_obj_t *_diag_cum    = nullptr;
    lv_obj_t *_diag_usbbar = nullptr;
    lv_obj_t *_diag_usblbl = nullptr;

    lv_obj_t *_set_gain_val   = nullptr;
    lv_obj_t *_set_preset_val = nullptr;
    lv_obj_t *_set_lp_val     = nullptr;
    lv_obj_t *_set_shelf_val  = nullptr;
    lv_obj_t *_set_new_val    = nullptr;
    lv_obj_t *_set_lost_val   = nullptr;
    lv_obj_t *_set_pos_val    = nullptr;
    lv_obj_t  *_set_vol_val    = nullptr;
    sdr_seg_t *_set_vol_slider = nullptr;
    lv_obj_t *_set_mute_val   = nullptr;
    lv_obj_t *_set_carto_val  = nullptr;

    static const int RADAR_MAX = 16;
    lv_obj_t *_radar_scope = nullptr;
    lv_obj_t *_radar_hdr   = nullptr;
    lv_obj_t *_radar_dot[RADAR_MAX] = { nullptr };
    lv_obj_t *_radar_lbl[RADAR_MAX] = { nullptr };
};
