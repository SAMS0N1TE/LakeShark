#pragma once

#include "lvgl.h"
#include "shell/ls_hub.h"

/*LS-603*/
class LsStatusBar {
public:
    lv_obj_t *build(lv_obj_t *parent, int w, lv_event_cb_t tap, void *ud);

private:
    static void hubCb(const ls_hub_state_t *s, uint32_t dirty, void *ud);
    void apply(const ls_hub_state_t *s, uint32_t dirty);

    lv_obj_t *_bar  = nullptr;
    lv_obj_t *_dot  = nullptr;
    lv_obj_t *_mode = nullptr;
    lv_obj_t *_bars = nullptr;
    lv_obj_t *_freq = nullptr;
    lv_obj_t *_usb  = nullptr;
    lv_obj_t *_sd   = nullptr;
    lv_obj_t *_bt   = nullptr;
    lv_obj_t *_vol  = nullptr;
    lv_obj_t *_bat  = nullptr;

    int _sub = -1;
};
