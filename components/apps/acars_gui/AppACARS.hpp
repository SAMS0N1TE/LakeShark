
#pragma once

#include "lvgl.h"
#include "shell/ls_app.hpp"
#include "shell/ls_text_entry.h"

class AppACARS : public LsApp {
public:
    AppACARS();
    ~AppACARS() override = default;

    bool run(lv_obj_t *parent) override;
    bool close(void) override;
    bool pause(void) override;
    bool resume(void) override;

private:
    lv_obj_t *_screen_readout = nullptr;
    lv_obj_t *_screen_lamp = nullptr;
    void        buildHeader(lv_obj_t *parent);
    void        buildTuning(lv_obj_t *parent);
    void        buildLog(lv_obj_t *parent);
    void        buildFooter(lv_obj_t *parent);

    void        refresh(void);
    void        updateTuningLabel(void);

    static void timerCb(lv_timer_t *t);
    static void injectCb(lv_event_t *e);
    static void clearCb(lv_event_t *e);
    static void chanCb(lv_event_t *e);
    static void freqCb(lv_event_t *e);
    static void gainCb(lv_event_t *e);
    static void agcCb(lv_event_t *e);
    static void entryDone(bool accepted, const char *text, void *user_data);
    void openEntry(bool gain);
    void closeEntry();
    ls_text_entry_t *_entry = nullptr;
    bool _entry_gain = false;
    lv_obj_t *_freq = nullptr;
    lv_obj_t *_gain = nullptr;
    lv_obj_t *_agc_btn = nullptr;
    lv_obj_t *_signal = nullptr;
    lv_obj_t *_entry_status = nullptr;

    lv_timer_t *_timer     = nullptr;

    lv_obj_t   *_hdr       = nullptr;
    lv_obj_t   *_tune_lbl  = nullptr;
    lv_obj_t   *_log_col   = nullptr;
    lv_obj_t   *_empty     = nullptr;

    uint32_t    _last_delivered = 0;
    uint32_t    _last_bad_crc   = 0;
    int         _last_head      = -1;
};
