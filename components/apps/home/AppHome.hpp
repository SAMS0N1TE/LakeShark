#pragma once

#include "lvgl.h"
#include "shell/ls_app.hpp"
#include "shell/ls_hub.h"

/*LS-604*/
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
    void buildFace(lv_obj_t *parent);
    void buildTiles(lv_obj_t *parent);
    void buildSystem(lv_obj_t *parent);

    static void hubCb(const ls_hub_state_t *s, uint32_t dirty, void *ud);
    void apply(const ls_hub_state_t *s, uint32_t dirty);

    static void tileCb(lv_event_t *e);
    static void faceCb(lv_event_t *e);

    lv_obj_t *_face   = nullptr;
    lv_obj_t *_mode   = nullptr;
    lv_obj_t *_state  = nullptr;
    lv_obj_t *_freq   = nullptr;
    lv_obj_t *_detail = nullptr;
    lv_obj_t *_meter  = nullptr;

    lv_obj_t *_sys_rtl = nullptr;
    lv_obj_t *_sys_sd  = nullptr;
    lv_obj_t *_sys_c6  = nullptr;
    lv_obj_t *_sys_iq  = nullptr;
    lv_obj_t *_ticker  = nullptr;

    bool _visible = true;
    int  _sub     = -1;
};
