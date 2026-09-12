#pragma once

#include "lvgl.h"
#include "shell/ls_app.hpp"
#include "shell/ls_hub.h"
#include "home_widget_pref.h"

/**/
class AppHome : public LsApp {
public:
    AppHome();

    bool run(lv_obj_t *parent) override;
    bool close(void) override;
    bool pause(void) override;
    bool resume(void) override;
    bool passive(void) const override { return true; }
    void switchTab(int delta) override;

private:
    lv_obj_t *_screen_readout = nullptr;
    lv_obj_t *_screen_lamp = nullptr;
    void buildFace(lv_obj_t *parent);
    void buildTiles(lv_obj_t *parent);
    void buildInstrument(lv_obj_t *parent);

    static void hubCb(const ls_hub_state_t *s, uint32_t dirty, void *ud);
    void apply(const ls_hub_state_t *s, uint32_t dirty);

    static void tileCb(lv_event_t *e);
    static void faceCb(lv_event_t *e);
    static void widgetCb(lv_event_t *e);
    static void clockCb(lv_timer_t *timer);
    void refreshWidget(const ls_hub_state_t *s);
    home_widget_id_t _widget = HOME_WIDGET_RECEIVER;
    lv_obj_t *_widget_buttons[HOME_WIDGET_COUNT] = {};
    lv_obj_t *_save_status = nullptr;
    lv_timer_t *_clock_timer = nullptr;
    uint32_t _last_freq_hz = 0;
    char _last_receiver_app[8] = {};
    /**/
    static void themeCb(void *ud);

    lv_obj_t *_face   = nullptr;
    lv_obj_t *_mode   = nullptr;
    lv_obj_t *_freq   = nullptr;
    lv_obj_t *_detail = nullptr;

    bool _visible = true;
    int  _sub     = -1;
    /**/
    int  _theme_sub = -1;
};
