
#pragma once

#include "lvgl.h"
#include "shell/ls_app.hpp"
#include "sdr_ui/sdr_ui.h"

/*LS-020*/
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
    void switchTab(int delta) override;

private:
    void buildRecordTab(lv_obj_t *parent);
    void buildConfigTab(lv_obj_t *parent);
    void buildFilesTab(lv_obj_t *parent);

    void updateRecord(void);
    void updateConfig(void);
    void refreshFiles(void);

    static void timerCb(lv_timer_t *t);

    static void armCb(lv_event_t *e);
    static void stopCb(lv_event_t *e);
    static void saveCb(lv_event_t *e);

    static void freqDownCb(lv_event_t *e);
    static void freqUpCb(lv_event_t *e);
    static void freqCoarseDownCb(lv_event_t *e);
    static void freqCoarseUpCb(lv_event_t *e);
    static void presetCb(lv_event_t *e);

    static void gainDownCb(lv_event_t *e);
    static void gainUpCb(lv_event_t *e);
    static void threshDownCb(lv_event_t *e);
    static void threshUpCb(lv_event_t *e);
    static void threshAutoCb(lv_event_t *e);
    static void gapDownCb(lv_event_t *e);
    static void gapUpCb(lv_event_t *e);
    static void bwDownCb(lv_event_t *e);
    static void bwUpCb(lv_event_t *e);
    static void minPulseDownCb(lv_event_t *e);
    static void minPulseUpCb(lv_event_t *e);
    static void maxSpanDownCb(lv_event_t *e);
    static void maxSpanUpCb(lv_event_t *e);
    static void minEdgesDownCb(lv_event_t *e);
    static void minEdgesUpCb(lv_event_t *e);

    static void filesRefreshCb(lv_event_t *e);
    static void filesDeleteCb(lv_event_t *e);
    static void filesRowCb(lv_event_t *e);

    lv_obj_t   *_tabview = nullptr;
    lv_timer_t *_timer   = nullptr;

    lv_obj_t *_rec_hdr     = nullptr;
    lv_obj_t *_rec_magbar  = nullptr;
    lv_obj_t *_rec_maglbl  = nullptr;
    lv_obj_t *_rec_stats   = nullptr;
    lv_obj_t *_rec_result  = nullptr;
    lv_obj_t *_rec_file    = nullptr;

    lv_obj_t *_cfg_freq    = nullptr;
    lv_obj_t *_cfg_preset  = nullptr;
    lv_obj_t *_cfg_gain    = nullptr;
    lv_obj_t *_cfg_thresh  = nullptr;
    lv_obj_t *_cfg_gap     = nullptr;
    lv_obj_t *_cfg_bw      = nullptr;
    lv_obj_t *_cfg_minpul  = nullptr;
    lv_obj_t *_cfg_maxspan = nullptr;
    lv_obj_t *_cfg_minedg  = nullptr;

    lv_obj_t *_files_table = nullptr;
    lv_obj_t *_files_note  = nullptr;

    static const int FILES_MAX = 24;
    char _file_name[FILES_MAX][40] = {{0}};
    int  _file_count = 0;
    int  _file_sel   = -1;

    int _preset = -1;
};
