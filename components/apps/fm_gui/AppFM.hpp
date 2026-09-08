
#pragma once

#include "lvgl.h"
#include "shell/ls_app.hpp"
#include "shell/ls_text_entry.h"
#include "sdr_ui/sdr_ui.h"
#include "ui/ls_spectrum_waterfall.h"
/*LS-746*/
#include "scan_ui/scan_panel.hpp"

class AppFM : public LsApp {
public:
    AppFM();
    ~AppFM();

    bool run(lv_obj_t *parent) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;
    bool pause(void) override;
    bool resume(void) override;
    /*LS-604*/
    bool background(void) override;
    void switchTab(int delta) override;

private:
    lv_obj_t *_screen_readout = nullptr;
    lv_obj_t *_screen_lamp = nullptr;
    /*LS-608*/
    lv_obj_t *_reset_val = nullptr;

    void buildVfoTab(lv_obj_t *parent);
    /*LS-746*/
    void buildScanCtlTab(lv_obj_t *parent);
    ScanPanel _scan_panel;

    /*LS-703  Shared with REC and P25; this app still owns the sweep producer. */
    ls_spectrum_waterfall_t _s_spectrum = {};
    uint32_t    _wf_sweep  = 0;
    /* -1 so the first tick only records the mode rather than acting on it. */
    int         _last_mode = -1;
    void buildPageTab(lv_obj_t *parent);
    void buildScanTab(lv_obj_t *parent);
    void buildConfigTab(lv_obj_t *parent);
    void updateVfo(void);
    void updatePages(void);
    void updateScan(void);
    void updateConfig(void);

    static void timerCb(lv_timer_t *t);
    /*LS-608*/
    static void resetCb(lv_event_t *e);

    static void modeCb(lv_event_t *e);
    /*LS-731*/
    static void scanToggleCb(lv_event_t *e);
    static void scanSkipCb(lv_event_t *e);
    /*LS-736*/
    static void autoSqCb(lv_event_t *e);
    static void stepDownCb(lv_event_t *e);
    static void stepUpCb(lv_event_t *e);
    static void stepCycleCb(lv_event_t *e);
    static void tuneDeltaCb(lv_event_t *e);
    static void gainCb(lv_event_t *e);
    static void gainDownCb(lv_event_t *e);
    static void gainUpCb(lv_event_t *e);
    static void gainSliderCb(lv_event_t *e);
    static void sqSliderCb(lv_event_t *e);
    static void agcCb(lv_event_t *e);
    static void sqDownCb(lv_event_t *e);
    static void sqUpCb(lv_event_t *e);
    static void baudCb(lv_event_t *e);
    static void scanRestartCb(lv_event_t *e);
    static void tunePeakCb(lv_event_t *e);
    static void bandCb(lv_event_t *e);
    static void volSliderCb(lv_event_t *e);
    static void muteCb(lv_event_t *e);

    void openFreqEntry(void);
    void closeFreqEntry(void);
    static void freqEntryCb(lv_event_t *e);
    static void freqEntryDone(bool accepted, const char *text, void *user_data);

    lv_timer_t *_timer   = nullptr;
    lv_obj_t   *_tabview = nullptr;

    lv_obj_t *_v_mode  = nullptr;
    lv_obj_t *_v_rx    = nullptr;
    lv_obj_t *_v_lamp  = nullptr;
    lv_obj_t *_v_freq  = nullptr;
    lv_obj_t *_v_status = nullptr;
    lv_obj_t *_v_smeter = nullptr;
    lv_obj_t *_v_smtxt = nullptr;
    lv_obj_t *_v_act   = nullptr;
    lv_obj_t *_v_acttxt = nullptr;
    lv_obj_t *_v_diag  = nullptr;
    lv_obj_t *_v_dn_lbl = nullptr;
    lv_obj_t *_v_up_lbl = nullptr;
    lv_obj_t *_v_step_lbl = nullptr;
    /*LS-731*/
    lv_obj_t *_v_scan_lbl   = nullptr;
    lv_obj_t *_v_scan_state = nullptr;

    sdr_seg_t *_v_gain_slider = nullptr;
    lv_obj_t  *_v_gain_lbl = nullptr;
    sdr_seg_t *_v_sq_slider = nullptr;
    lv_obj_t  *_v_sq_lbl = nullptr;
    sdr_seg_t *_v_vol_slider = nullptr;
    lv_obj_t  *_v_vol_lbl = nullptr;

    lv_obj_t *_p_lamp  = nullptr;
    lv_obj_t *_p_strap = nullptr;
    lv_obj_t *_p_counts = nullptr;
    lv_obj_t *_p_log   = nullptr;
    lv_obj_t *_p_baud  = nullptr;

    lv_obj_t *_s_info  = nullptr;
    lv_obj_t *_s_peak  = nullptr;
    lv_obj_t *_c_freq = nullptr;
    lv_obj_t *_c_gain = nullptr;
    lv_obj_t *_c_sql  = nullptr;
    lv_obj_t *_c_baud = nullptr;
    lv_obj_t *_c_band = nullptr;
    sdr_seg_t *_c_gain_slider = nullptr;
    lv_obj_t  *_c_vol_lbl = nullptr;
    sdr_seg_t *_c_vol_slider = nullptr;
    lv_obj_t *_c_mute = nullptr;
    lv_obj_t *_c_diag = nullptr;

    ls_text_entry_t *_freq_entry = nullptr;

    int _step_idx = 2;
};
