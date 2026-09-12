
#pragma once

#include "lvgl.h"
#include "shell/ls_app.hpp"
#include "shell/ls_text_entry.h"
#include "sdr_ui/sdr_ui.h"
#include "ui/ls_spectrum_waterfall.h"

/**/
class AppREC : public LsApp {
public:
    AppREC();
    ~AppREC();

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
    lv_obj_t *_screen_readout = nullptr;
    lv_obj_t *_screen_lamp = nullptr;
    void buildRecordTab(lv_obj_t *parent);
    /**/
    void buildScoutTab(lv_obj_t *parent);
    void updateScout(void);
    /**/
    void applyScoutSplit(void);
    void scoutRefreshRecBtn(void);
    void scoutLoadPrefs(void);
    void scoutSavePrefs(void);
    static void scoutViewChanged(ls_spectrum_waterfall_t *view,
                                 void *user_data);
    void buildConfigTab(lv_obj_t *parent);
    void buildFilesTab(lv_obj_t *parent);

    void updateRecord(void);
    void updateConfig(void);
    void refreshFiles(void);

    static void timerCb(lv_timer_t *t);

    static void armCb(lv_event_t *e);
    static void stopCb(lv_event_t *e);
    static void saveCb(lv_event_t *e);
    /**/
    static void toScoutCb(lv_event_t *e);

    static void freqDownCb(lv_event_t *e);
    static void freqUpCb(lv_event_t *e);
    static void freqCoarseDownCb(lv_event_t *e);
    static void freqCoarseUpCb(lv_event_t *e);
    static void presetCb(lv_event_t *e);

    /**/
    static void freqEntryCb(lv_event_t *e);
    static void freqEntryDone(bool accepted, const char *text, void *user_data);
    void openFreqEntry(void);
    void closeFreqEntry(void);

    static void gainDownCb(lv_event_t *e);
    static void gainUpCb(lv_event_t *e);
    static void threshDownCb(lv_event_t *e);
    static void threshUpCb(lv_event_t *e);
    static void threshAutoCb(lv_event_t *e);
    static void gapDownCb(lv_event_t *e);
    static void gapUpCb(lv_event_t *e);
    static void bwDownCb(lv_event_t *e);
    static void bwUpCb(lv_event_t *e);
    /**/
    static void bwAutoCb(lv_event_t *e);
    static void cfgResetCb(lv_event_t *e);
    static void minPulseDownCb(lv_event_t *e);
    static void minPulseUpCb(lv_event_t *e);
    static void maxSpanDownCb(lv_event_t *e);
    static void maxSpanUpCb(lv_event_t *e);
    static void minEdgesDownCb(lv_event_t *e);
    static void minEdgesUpCb(lv_event_t *e);

    /**/
    static void scoutTunePeakCb(lv_event_t *e);
    /**/
    static void scoutSpanCb(lv_event_t *e);
    static void scoutZoomOutCb(lv_event_t *e);
    static void scoutZoomInCb(lv_event_t *e);
    /**/
    static void scoutGainDownCb(lv_event_t *e);
    static void scoutGainUpCb(lv_event_t *e);
    static void scoutArmCb(lv_event_t *e);

    static void filesRefreshCb(lv_event_t *e);
    static void filesDeleteCb(lv_event_t *e);
    static void filesRowCb(lv_event_t *e);

    lv_obj_t   *_tabview = nullptr;
    lv_timer_t *_timer   = nullptr;

    /* RECORD tab, LCD-face layout in the same visual language as
       AppP25's DECODE face: phase strap + big freq + status subline +
       signal meter + grouped stats. */
    lv_obj_t *_rec_face      = nullptr;
    lv_obj_t *_rec_phase_lbl = nullptr;
    lv_obj_t *_rec_lamp      = nullptr;
    lv_obj_t *_rec_rx        = nullptr;
    lv_obj_t *_rec_freq      = nullptr;
    lv_obj_t *_rec_sub       = nullptr;
    lv_obj_t *_rec_magbar    = nullptr;
    lv_obj_t *_rec_stats     = nullptr;
    lv_obj_t *_rec_result    = nullptr;
    lv_obj_t *_rec_file      = nullptr;

    lv_obj_t *_cfg_freq    = nullptr;
    lv_obj_t *_cfg_preset  = nullptr;
    lv_obj_t *_cfg_gain    = nullptr;
    lv_obj_t *_cfg_thresh  = nullptr;
    lv_obj_t *_cfg_gap     = nullptr;
    lv_obj_t *_cfg_bw      = nullptr;
    lv_obj_t *_cfg_minpul  = nullptr;
    lv_obj_t *_cfg_maxspan = nullptr;
    lv_obj_t *_cfg_minedg  = nullptr;

    /**/
    ls_text_entry_t *_freq_entry = nullptr;

    /* SCOUT tab.  See buildScoutTab for the layout and why. */
    lv_obj_t *_scout_hdr       = nullptr;
    lv_obj_t *_scout_freq      = nullptr;
    lv_obj_t *_scout_sub       = nullptr;
    lv_obj_t *_scout_peak_lbl  = nullptr;
    /* Chrome that hides in fullscreen so the waterfall gets the
       whole panel; grouped in one container to make show/hide one call. */
    lv_obj_t *_scout_chrome    = nullptr;
    lv_obj_t *_scout_area      = nullptr;
    ls_spectrum_waterfall_t _scout_spectrum = {};
    lv_obj_t *_scout_span_lbl  = nullptr;
    lv_obj_t *_scout_stats     = nullptr;
    lv_obj_t *_scout_rec_lbl   = nullptr;
    lv_obj_t *_scout_row1      = nullptr;
    lv_obj_t *_scout_row2      = nullptr;
    uint32_t  _scout_last_sweep = 0;
    /* Which SPAN entry is showing.  Zoom is a display crop of the native
       ~200 kHz window, not a hardware sweep - the tuner does not move
       when this changes. */
    int       _scout_zoom = 0;
    /* SCOUT visible-buffer sizes.  Both derive from the panel at
       run() - never a literal.  A 240 hardcoded here shipped as a
       waterfall filling half the LCD-4.3, and disagreed with the comment that had assumed 460. The shared widget reports the PSRAM
       footprint. */
    int       _wf_w             = 0;
    int       _scout_bins       = 0;
    int       _scout_area_cap_h = 0;

    int       _scout_split_pct  = 50;
    int       _scout_contrast_pct = 100;
    /* Fullscreen waterfall: chrome hides; responsive controls remain. */
    bool      _scout_full       = false;
    /* Rolling frame-rate window: last N sweep timestamps sampled in the
       LVGL tick, so the number the panel prints is the rate the user
       actually sees. */
    int64_t   _scout_last_us = 0;
    float     _scout_fps     = 0.0f;

    lv_obj_t *_files_table = nullptr;
    lv_obj_t *_files_note  = nullptr;

    static const int FILES_MAX = 24;
    char _file_name[FILES_MAX][40] = {{0}};
    int  _file_count = 0;
    int  _file_sel   = -1;

    int _preset = -1;
};
