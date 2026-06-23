
#pragma once

#include "lvgl.h"
#include "shell/ls_app.hpp"
#include "sdr_ui/sdr_ui.h"

class AppP25 : public LsApp {
public:
    AppP25();
    ~AppP25();

    bool run(lv_obj_t *parent) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;
    bool pause(void) override;
    bool resume(void) override;
    void switchTab(int delta) override;

private:
    void buildDecodeTab(lv_obj_t *parent);
    void buildSignalTab(lv_obj_t *parent);
    void buildScanTab(lv_obj_t *parent);
    void buildSettingsTab(lv_obj_t *parent);
    void updateDecode(void);
    void updateSignal(void);
    void updateScan(void);
    void updateSettings(void);
    void rateSample(void);

    static void scanToggleCb(lv_event_t *e);
    static void scanSkipCb(lv_event_t *e);
    static void scanTableCb(lv_event_t *e);

    static void timerCb(lv_timer_t *t);
    static void freqDownCb(lv_event_t *e);
    static void freqUpCb(lv_event_t *e);
    static void freqEntryCb(lv_event_t *e);
    static void freqKbCb(lv_event_t *e);
    void openFreqEntry(void);
    void closeFreqEntry(void);
    static void modeCb(lv_event_t *e);
    static void resetCb(lv_event_t *e);
    static void gainCb(lv_event_t *e);
    static void agcCb(lv_event_t *e);
    static void beepCb(lv_event_t *e);

    static void freqM1Cb(lv_event_t *e);
    static void freqm25Cb(lv_event_t *e);
    static void freqp25Cb(lv_event_t *e);
    static void freqP1Cb(lv_event_t *e);
    static void gainStepCb(lv_event_t *e);
    static void agcCb2(lv_event_t *e);
    static void modeCycleCb(lv_event_t *e);
    static void polarityCb(lv_event_t *e);
    static void beepCb2(lv_event_t *e);
    static void favPrevCb(lv_event_t *e);
    static void favNextCb(lv_event_t *e);
    static void favSaveCb(lv_event_t *e);
    static void favTuneCb(lv_event_t *e);
    static void favClrCb(lv_event_t *e);
    static void presetLeftCb(lv_event_t *e);
    static void presetRightCb(lv_event_t *e);
    static void lpLeftCb(lv_event_t *e);
    static void lpRightCb(lv_event_t *e);
    static void shelfLeftCb(lv_event_t *e);
    static void shelfRightCb(lv_event_t *e);
    static void voiceTestCb(lv_event_t *e);
    static void rebootToggleCb(lv_event_t *e);
    static void volDownCb(lv_event_t *e);
    static void volUpCb(lv_event_t *e);
    static void volSliderCb(lv_event_t *e);
    static void muteCb(lv_event_t *e);

    lv_obj_t   *_tabview = nullptr;
    lv_timer_t *_timer   = nullptr;

    lv_obj_t *_d_led    = nullptr;
    lv_obj_t *_d_face_mode = nullptr;
    lv_obj_t *_d_rx     = nullptr;
    lv_obj_t *_d_freq   = nullptr;
    lv_obj_t *_d_smeter = nullptr;
    lv_obj_t *_d_smtxt  = nullptr;
    lv_obj_t *_d_bmeter = nullptr;
    lv_obj_t *_d_bmtxt  = nullptr;
    lv_obj_t *_d_decode = nullptr;
    lv_obj_t *_d_radio  = nullptr;
    lv_obj_t *_d_status = nullptr;
    lv_obj_t *_d_beepbtn_lbl = nullptr;
    lv_obj_t *_d_scan_btn_lbl = nullptr;

    sdr_seg_t *_d_gain_slider = nullptr;
    lv_obj_t  *_d_gain_lbl    = nullptr;
    sdr_seg_t *_d_vol_slider  = nullptr;
    lv_obj_t  *_d_vol_lbl     = nullptr;

    lv_obj_t *_freq_modal = nullptr;
    lv_obj_t *_freq_ta    = nullptr;

    lv_obj_t *_s_hdr   = nullptr;
    lv_obj_t *_s_iqbar = nullptr;
    lv_obj_t *_s_iqlbl = nullptr;
    lv_obj_t *_s_chart = nullptr;
    lv_chart_series_t *_s_voice = nullptr;
    lv_chart_series_t *_s_sync  = nullptr;
    lv_obj_t *_s_totals = nullptr;
    lv_obj_t *_s_err    = nullptr;

    static const int RATE_N = 60;
    int     _rate_voice[RATE_N] = {0};
    int     _rate_sync[RATE_N]  = {0};
    int     _rate_head = 0;
    int     _last_sync = 0;
    int     _last_voice = 0;
    int64_t _last_sample_us = 0;

    int       _fav_slot       = 0;
    lv_obj_t *_set_freq_val   = nullptr;
    lv_obj_t *_set_gain_val   = nullptr;
    lv_obj_t *_set_mode_val   = nullptr;
    lv_obj_t *_set_pol_val    = nullptr;
    lv_obj_t *_set_beep_val   = nullptr;
    lv_obj_t *_set_fav_val    = nullptr;
    lv_obj_t *_set_preset_val = nullptr;
    lv_obj_t *_set_lp_val     = nullptr;
    lv_obj_t *_set_shelf_val  = nullptr;
    lv_obj_t  *_set_vol_val    = nullptr;
    sdr_seg_t *_set_vol_slider = nullptr;
    lv_obj_t  *_set_gate_lbl   = nullptr;
    sdr_seg_t *_set_gate_slider = nullptr;
    lv_obj_t *_set_mute_val   = nullptr;
    lv_obj_t *_set_reboot_val = nullptr;

    lv_obj_t  *_scan_status   = nullptr;
    lv_obj_t  *_scan_btn_lbl  = nullptr;
    lv_obj_t  *_scan_table    = nullptr;
};
