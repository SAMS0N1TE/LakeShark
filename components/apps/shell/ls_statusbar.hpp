#pragma once

#include "lvgl.h"
#include "shell/ls_hub.h"

/**/
class LsStatusBar {
public:
    void detach();
    lv_obj_t *build(lv_obj_t *parent, int w, lv_event_cb_t tap, void *ud);
    void resize(int width,int x=0,int y=0);
    void setTitle(const char *app,const char *page);

private:
    static void hubCb(const ls_hub_state_t *s, uint32_t dirty, void *ud);
    /**/
    static void themeCb(void *ud);
    void apply(const ls_hub_state_t *s, uint32_t dirty);

    lv_obj_t *_bar  = nullptr;
    lv_obj_t *_dot  = nullptr;
    lv_obj_t *_mode = nullptr;
    lv_obj_t *_title = nullptr;
    lv_obj_t *_bars = nullptr;
    lv_obj_t *_freq = nullptr;
    lv_obj_t *_usb  = nullptr;
    lv_obj_t *_sd   = nullptr;
    lv_obj_t *_bt   = nullptr;
    lv_obj_t *_vol  = nullptr;
    lv_obj_t *_bat  = nullptr;

    int _sub = -1, _theme_sub = -1;
};
