
#pragma once

#include "lvgl.h"
#include "shell/ls_app.hpp"
#include "shell/ls_text_entry.h"
#include "sdr_ui/sdr_ui.h"
#include "ui/ls_spectrum_waterfall.h"
/**/
#include "scan_ui/scan_panel.hpp"
extern "C" {
#include "p25_tg_roster.h"
}

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
    /**/
    bool background(void) override;
    void switchTab(int delta) override;

private:
    void buildDecodeTab(lv_obj_t *parent);
    void buildInstrumentDecode(lv_obj_t *parent);
    void ensureTab(unsigned tab);
    lv_obj_t *_lazy_pages[7]={};
    unsigned _built_tabs=0;
    void buildSignalTab(lv_obj_t *parent);
    void buildHealthTab(lv_obj_t *parent);
    void buildScanTab(lv_obj_t *parent);
    void buildSettingsTab(lv_obj_t *parent);
    /**/
    void buildProgramTab(lv_obj_t *parent);
    /**/
    void buildTalkgroupsTab(lv_obj_t *parent);
    void updateDecode(void);
    void updateSignal(void);
    void updateHealth(void);
    void updateScan(void);
    void updateSettings(void);
    /**/
    void updateProgram(void);
    /**/
    void updateTalkgroups(void);
    void finishTalkgroupEdit(p25_tg_edit_result_t result, bool persistent,
                             const char *success);
    void rateSample(void);

    static void scanToggleCb(lv_event_t *e);
    static void scanSkipCb(lv_event_t *e);
    static void scanTableCb(lv_event_t *e);
    /**/
    static void holdCb(lv_event_t *e);
    static void lockCb(lv_event_t *e);
    /**/
    static void zonePrevCb(lv_event_t *e);
    static void zoneNextCb(lv_event_t *e);
    void        updateZone(void);
    /**/
    static void chAddCb(lv_event_t *e);
    static void chDelCb(lv_event_t *e);
    /**/
    static void chLockCb(lv_event_t *e);
    static void chNameCb(lv_event_t *e);
    static void nameEntryDone(bool accepted, const char *text, void *user_data);
    void        openNameEntry(void);
    void        closeNameEntry(void);
    void        updateChSel(void);

    /**/
    static void programReloadCb(lv_event_t *e);
    static void programPrevCb(lv_event_t *e);
    static void programNextCb(lv_event_t *e);
    /**/
    static void programSurveyCb(lv_event_t *e);
    static void programSurveyCancelCb(lv_event_t *e);
    /**/
    static void tgTableCb(lv_event_t *e);
    static void tgModeCb(lv_event_t *e);
    static void tgListCb(lv_event_t *e);
    static void tgHoldCb(lv_event_t *e);
    static void tgLockCb(lv_event_t *e);
    static void tgPriorityDownCb(lv_event_t *e);
    static void tgPriorityUpCb(lv_event_t *e);

    static void timerCb(lv_timer_t *t);
    /**/
    static void defaultsCb(lv_event_t *e);
    /**/
    static void scanFitCb(lv_event_t *e);
    static void scanFit(lv_obj_t *t);
    static void freqDownCb(lv_event_t *e);
    static void freqUpCb(lv_event_t *e);
    static void freqEntryCb(lv_event_t *e);
    static void freqEntryDone(bool accepted, const char *text, void *user_data);
    void openFreqEntry(void);
    void closeFreqEntry(void);
    static void modeCb(lv_event_t *e);
    static void resetCb(lv_event_t *e);
    static void gainCb(lv_event_t *e);
    static void agcCb(lv_event_t *e);
    static void beepCb(lv_event_t *e);
    static void spectrumTapCb(lv_event_t *e);
    static void spectrumGainDownCb(lv_event_t *e);
    static void spectrumGainUpCb(lv_event_t *e);

    static void freqM1Cb(lv_event_t *e);
    static void freqm25Cb(lv_event_t *e);
    static void freqp25Cb(lv_event_t *e);
    static void freqP1Cb(lv_event_t *e);
    static void autoFollowCb(lv_event_t *e);
    static void skipEncryptedCb(lv_event_t *e);
    static void skipDurationDownCb(lv_event_t *e);
    static void skipDurationUpCb(lv_event_t *e);
    static void cqpskTimingDownCb(lv_event_t *e);
    static void cqpskTimingUpCb(lv_event_t *e);
    static void cqpskCarrierDownCb(lv_event_t *e);
    static void cqpskCarrierUpCb(lv_event_t *e);
    static void cqpskDefaultsCb(lv_event_t *e);
    void setControlStatus(bool ok, const char *ok_text = "QUEUED");
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
    lv_obj_t *_d_identity = nullptr;
    lv_obj_t *_d_radio  = nullptr;
    lv_obj_t *_d_status = nullptr;
    lv_obj_t *_d_beepbtn_lbl = nullptr;
    lv_obj_t *_d_scan_btn_lbl = nullptr;
    /**/
    lv_obj_t *_d_hold_btn_lbl = nullptr;

    sdr_seg_t *_d_gain_slider = nullptr;
    lv_obj_t  *_d_gain_lbl    = nullptr;
    sdr_seg_t *_d_vol_slider  = nullptr;
    lv_obj_t  *_d_vol_lbl     = nullptr;

    ls_text_entry_t *_freq_entry = nullptr;

    lv_obj_t *_s_hdr   = nullptr;
    lv_obj_t *_s_rf    = nullptr;
    lv_obj_t *_s_peak  = nullptr;
    lv_obj_t *_s_iqbar = nullptr;
    lv_obj_t *_s_iqlbl = nullptr;
    lv_obj_t *_s_chart = nullptr;
    lv_chart_series_t *_s_voice = nullptr;
    lv_chart_series_t *_s_sync  = nullptr;
    lv_obj_t *_s_totals = nullptr;
    lv_obj_t *_s_call_details = nullptr;
    lv_obj_t *_s_err    = nullptr;
    lv_obj_t *_s_gui    = nullptr;
    ls_spectrum_waterfall_t _s_spectrum = {};
    uint32_t _s_spectrum_seq = 0;
    /* Last tuning the waterfall was drawn at. A retune makes the rows above
       the change mean a different frequency, which is the one case where
       history has to go. */
    uint32_t _s_spectrum_center_hz = 0;
    uint32_t _s_spectrum_span_hz = 0;

    static const int RATE_N = 60;
    int     _rate_voice[RATE_N] = {0};
    int     _rate_sync[RATE_N]  = {0};
    int     _rate_head = 0;
    int     _last_sync = 0;
    int     _last_voice = 0;
    int64_t _last_sample_us = 0;

    uint32_t _gui_last_us = 0;
    uint32_t _gui_max_us = 0;
    uint32_t _gui_over_budget = 0;
    int64_t  _gui_last_warn_us = 0;

    int       _fav_slot       = 0;
    lv_obj_t *_set_freq_val   = nullptr;
    lv_obj_t *_set_gain_val   = nullptr;
    lv_obj_t *_set_mode_val   = nullptr;
    lv_obj_t *_set_pol_val    = nullptr;
    lv_obj_t *_set_beep_val   = nullptr;
    lv_obj_t *_set_follow_val = nullptr;
    lv_obj_t *_set_skip_val   = nullptr;
    lv_obj_t *_set_skip_ms_val = nullptr;
    lv_obj_t *_set_control_status = nullptr;
    lv_obj_t *_set_cqpsk_timing_val = nullptr;
    lv_obj_t *_set_cqpsk_carrier_val = nullptr;
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

    lv_obj_t *_set_agc_btn    = nullptr;
    lv_obj_t *_set_pol_btn    = nullptr;
    lv_obj_t *_set_beep_btn   = nullptr;
    lv_obj_t *_set_follow_btn = nullptr;
    lv_obj_t *_set_skip_btn   = nullptr;
    lv_obj_t *_set_mute_btn   = nullptr;
    lv_obj_t *_set_reboot_btn = nullptr;

    /**/
    ScanPanel  _scan_panel;
    lv_obj_t  *_scan_table    = nullptr;
    uint32_t   _scan_render_sig = 0;
    bool       _scan_rendered = false;
    int        _scan_render_cur = -1;
    /**/
    lv_obj_t  *_reset_val     = nullptr;

    /**/
    lv_obj_t  *_zone_val      = nullptr;
    /**/
    lv_obj_t  *_pg_state      = nullptr;
    lv_obj_t  *_pg_system     = nullptr;
    lv_obj_t  *_pg_site       = nullptr;
    lv_obj_t  *_pg_roster     = nullptr;
    lv_obj_t  *_pg_source     = nullptr;
    lv_obj_t  *_pg_control    = nullptr;
    lv_obj_t  *_pg_list       = nullptr;
    lv_obj_t  *_pg_status     = nullptr;
    /**/
    lv_obj_t  *_pg_survey     = nullptr;

    /**/
    p25_tg_roster_t _tg_roster = {};
    lv_obj_t  *_tg_summary    = nullptr;
    lv_obj_t  *_tg_table      = nullptr;
    lv_obj_t  *_tg_observed   = nullptr;
    uint32_t   _tg_observed_second = UINT32_MAX;
    void updateObservedTalkgroups(void);
    lv_obj_t  *_tg_status     = nullptr;
    uint32_t   _tg_render_sig = 0;
    bool       _tg_rendered   = false;

    /**/
    int        _sel_idx       = -1;
    lv_obj_t  *_ch_val        = nullptr;
    ls_text_entry_t *_name_entry = nullptr;
};
