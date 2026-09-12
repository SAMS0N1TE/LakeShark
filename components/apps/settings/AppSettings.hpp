#pragma once

#include "lvgl.h"
#include "shell/ls_app.hpp"
#include "sdr_ui/sdr_ui.h"
#include "shell/ls_text_entry.h"
#include "settings_wifi.h"

class LsSettings : public LsApp {
public:
    LsSettings();

    bool run(lv_obj_t *parent) override;
    bool back(void) override;
    bool close(void) override;
    /**/
    bool pause(void) override;
    bool stopped(void) const override { return settings_wifi_stopped(); }
    /**/
    bool resume(void) override;
    /**/
    bool passive(void) const override { return true; }

private:
    void buildLocation(lv_obj_t *parent);
    void refreshLocation();
    void closeLocationEntry();
    static void locationCb(lv_event_t *e);
    static void locationDone(bool accepted, const char *text, void *user_data);
    lv_obj_t *_location_lat = nullptr, *_location_lon = nullptr, *_location_note = nullptr;
    lv_obj_t *_location_buttons[4] = {};
    ls_text_entry_t *_location_entry = nullptr;
    double _location_values[2] = {};
    bool _location_known[2] = {};
    int _location_axis = 0;
    void buildWifi(lv_obj_t *parent);
    void refreshWifi();
    void closeWifiEntry();
    static void wifiCb(lv_event_t *e);
    static void wifiSelectCb(lv_event_t *e);
    static void wifiEntryCb(lv_event_t *e);
    static void wifiEntryDone(bool accepted, const char *text, void *user_data);
    lv_obj_t *_wifi_status = nullptr, *_wifi_result = nullptr;
    lv_obj_t *_wifi_ssid_label = nullptr, *_wifi_password_label = nullptr;
    lv_obj_t *_wifi_networks = nullptr;
    lv_obj_t *_wifi_buttons[5] = {};
    lv_obj_t *_wifi_ssid_button = nullptr, *_wifi_password_button = nullptr;
    ls_text_entry_t *_wifi_entry = nullptr;
    bool _wifi_edit_password = false, _wifi_available = false;
    unsigned _wifi_scan_revision = 0;
    int _wifi_network_count = 0;
    ls_wifi_scan_ap_t _wifi_scan[16] = {};
    char _wifi_ssid[33] = {}, _wifi_password[64] = {};
    char _wifi_note[96] = {};
    static void timerCb(lv_timer_t *t);
    static void brightnessCb(void *user_data, int value);
    static void volumeCb(void *user_data, int value);
    static void filesCb(lv_event_t *e);
    static void rebootCb(lv_event_t *e);
    static void dlModeCb(lv_event_t *e);
    static void usbRebootCb(lv_event_t *e);
    static void muteCb(lv_event_t *e);
    static void autodimCb(lv_event_t *e);
    static void dimToCb(lv_event_t *e);
    static void bootSndCb(lv_event_t *e);
    /**/
    static void themeCb(lv_event_t *e);

    void refreshValues(void);
    void refreshExternal(void);

    lv_timer_t *_timer       = nullptr;
    lv_obj_t   *_heap_val    = nullptr;
    lv_obj_t   *_bright_lbl  = nullptr;
    lv_obj_t   *_vol_lbl     = nullptr;
    lv_obj_t   *_usb_val     = nullptr;
    lv_obj_t   *_mute_val    = nullptr;
    lv_obj_t   *_autodim_val = nullptr;
    lv_obj_t   *_dimto_val   = nullptr;
    lv_obj_t   *_boot_val    = nullptr;
    /**/
    lv_obj_t   *_theme_val   = nullptr;
    lv_obj_t   *_usb_btn     = nullptr;
    lv_obj_t   *_mute_btn    = nullptr;
    lv_obj_t   *_autodim_btn = nullptr;
    lv_obj_t   *_dimto_btn   = nullptr;
    lv_obj_t   *_boot_btn    = nullptr;
    lv_obj_t   *_theme_btn   = nullptr;
};
