#pragma once

#include "lvgl.h"
#include "shell/ls_app.hpp"
#include "sdr_ui/sdr_ui.h"

class LsSettings : public LsApp {
public:
    LsSettings();

    bool run(lv_obj_t *parent) override;
    bool back(void) override;
    bool close(void) override;

private:
    static void timerCb(lv_timer_t *t);
    static void filesCb(lv_event_t *e);
    static void rebootCb(lv_event_t *e);
    static void dlModeCb(lv_event_t *e);
    static void usbRebootCb(lv_event_t *e);
    static void muteCb(lv_event_t *e);
    static void autodimCb(lv_event_t *e);
    static void dimToCb(lv_event_t *e);
    static void bootSndCb(lv_event_t *e);

    lv_timer_t *_timer       = nullptr;
    lv_obj_t   *_heap_val    = nullptr;
    lv_obj_t   *_bright_lbl  = nullptr;
    lv_obj_t   *_vol_lbl     = nullptr;
    lv_obj_t   *_usb_val     = nullptr;
    lv_obj_t   *_mute_val    = nullptr;
    lv_obj_t   *_autodim_val = nullptr;
    lv_obj_t   *_dimto_val   = nullptr;
    lv_obj_t   *_boot_val    = nullptr;
};
