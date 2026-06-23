#include "AppSettings.hpp"
#include "shell/ls_shell.hpp"
#include "sdr_ui/sdr_ui.h"

#include <cstdio>

#include "bsp/esp-bsp.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "soc/lp_system_reg.h"
#include "soc/soc.h"

extern "C" {
#include "settings.h"
#include "app_registry.h"
#include "audio_out.h"
#include "display_ctl.h"
}

static void seg_brightness(void *ud, int v)
{
    (void)ud;
    display_ctl_set_user(v);
}

static void seg_volume(void *ud, int v)
{
    (void)ud;
    audio_volume_set(v);
}

static const char *reset_reason_str(void)
{
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "PANIC (code)";
        case ESP_RST_INT_WDT:   return "int watchdog";
        case ESP_RST_TASK_WDT:  return "task watchdog";
        case ESP_RST_WDT:       return "HW watchdog/hang";
        case ESP_RST_BROWNOUT:  return "BROWNOUT (power)";
        case ESP_RST_EXT:       return "external";
        case ESP_RST_DEEPSLEEP: return "deep-sleep";
        default:                return "unknown";
    }
}

LsSettings::LsSettings() : LsApp("Settings", "settings") {}

bool LsSettings::run(lv_obj_t *parent)
{
    sdr_style_screen(parent);
    lv_obj_set_style_pad_all(parent, 8, 0);
    lv_obj_set_style_pad_row(parent, 6, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);

    sdr_setrow_t r;

    sdr_section(parent, "DISPLAY");
    _bright_lbl = nullptr;
    sdr_seg_slider(parent, SDR_PAS_GOLD, 100, display_ctl_get_user(),
                   seg_brightness, this, &_bright_lbl);

    sdr_setting_row(parent, "AUTO-DIM", &r);
    _autodim_val = r.value;
    sdr_btn(r.controls, "TOGGLE", autodimCb, this, nullptr);

    sdr_setting_row(parent, "DIM AFTER", &r);
    _dimto_val = r.value;
    sdr_btn(r.controls, "CHANGE", dimToCb, this, nullptr);

    sdr_section(parent, "AUDIO");
    _vol_lbl = nullptr;
    sdr_seg_slider(parent, SDR_PAS_CYAN, 100, audio_volume_get(),
                   seg_volume, this, &_vol_lbl);
    sdr_setting_row(parent, "MUTE", &r);
    _mute_val = r.value;
    sdr_btn(r.controls, "TOGGLE", muteCb, this, nullptr);

    sdr_setting_row(parent, "BOOT SOUND", &r);
    _boot_val = r.value;
    sdr_btn(r.controls, "CHANGE", bootSndCb, this, nullptr);

    sdr_section(parent, "APPS");
    sdr_setting_row(parent, "FILE BROWSER", &r);
    lv_label_set_text(r.value, "");
    sdr_btn(r.controls, "OPEN", filesCb, this, nullptr);

    sdr_section(parent, "SYSTEM");
    sdr_setting_row(parent, "USB AUTO-REBOOT", &r);
    _usb_val = r.value;
    sdr_btn(r.controls, "TOGGLE", usbRebootCb, this, nullptr);

    sdr_setting_row(parent, "RESTART", &r);
    lv_label_set_text(r.value, "");
    sdr_btn(r.controls, "REBOOT", rebootCb, this, nullptr);

    sdr_setting_row(parent, "FLASH MODE", &r);
    lv_label_set_text(r.value, "download");
    sdr_btn(r.controls, "ENTER", dlModeCb, this, nullptr);

    sdr_section(parent, "ABOUT");
    sdr_setting_row(parent, "FREE RAM", &r);
    _heap_val = r.value;
    lv_label_set_text(_heap_val, "...");

    sdr_setrow_t rr;
    sdr_setting_row(parent, "LAST RESET", &rr);
    lv_label_set_text(rr.value, reset_reason_str());

    sdr_setrow_t vr;
    sdr_setting_row(parent, "FIRMWARE", &vr);
    lv_label_set_text(vr.value, "LakeShark");

    _timer = lv_timer_create(timerCb, 1000, this);
    timerCb(_timer);
    return true;
}

void LsSettings::timerCb(lv_timer_t *t)
{
    LsSettings *self = static_cast<LsSettings *>(t->user_data);
    if (self->_heap_val) {
        char b[64];
        snprintf(b, sizeof(b), "%u KB int / %u KB psram",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        lv_label_set_text(self->_heap_val, b);
    }
    if (self->_usb_val)
        lv_label_set_text(self->_usb_val, app_usb_autoreboot() ? "ON" : "OFF");
    if (self->_mute_val)
        lv_label_set_text(self->_mute_val, audio_is_muted() ? "MUTED" : "ON");
    if (self->_autodim_val)
        lv_label_set_text(self->_autodim_val, display_ctl_autodim_enabled() ? "ON" : "OFF");
    if (self->_dimto_val)
        lv_label_set_text_fmt(self->_dimto_val, "%ds", display_ctl_autodim_timeout());
    if (self->_bright_lbl)
        lv_label_set_text_fmt(self->_bright_lbl, "BRIGHTNESS  %d", display_ctl_get_user());
    if (self->_vol_lbl)
        lv_label_set_text_fmt(self->_vol_lbl, "VOLUME  %d", audio_volume_get());
    if (self->_boot_val) {
        static const char *bn[] = { "OFF", "BEEP", "VOICE" };
        int m = settings_get_boot_sound();
        lv_label_set_text(self->_boot_val, bn[(m >= 0 && m <= 2) ? m : 0]);
    }
}

void LsSettings::filesCb(lv_event_t *)
{
    LsShell::instance().launchByName("Files");
}

void LsSettings::muteCb(lv_event_t *) { audio_toggle_mute(); }

void LsSettings::autodimCb(lv_event_t *)
{
    display_ctl_set_autodim(!display_ctl_autodim_enabled());
}

void LsSettings::dimToCb(lv_event_t *)
{
    static const int steps[] = { 10, 15, 30, 60, 120 };
    const int n = (int)(sizeof(steps) / sizeof(steps[0]));
    int cur = display_ctl_autodim_timeout();
    int idx = 0;
    for (int i = 0; i < n; i++) if (steps[i] == cur) { idx = (i + 1) % n; break; }
    display_ctl_set_autodim_timeout(steps[idx]);
}

void LsSettings::bootSndCb(lv_event_t *)
{
    settings_set_boot_sound((settings_get_boot_sound() + 1) % 3);
}

void LsSettings::usbRebootCb(lv_event_t *)
{
    app_set_usb_autoreboot(!app_usb_autoreboot());
}

void LsSettings::rebootCb(lv_event_t *)
{
    esp_restart();
}

void LsSettings::dlModeCb(lv_event_t *)
{
    REG_SET_BIT(LP_SYSTEM_REG_SYS_CTRL_REG, LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

bool LsSettings::back(void) { return exitToLauncher(); }

bool LsSettings::close(void)
{
    if (_timer) { lv_timer_del(_timer); _timer = nullptr; }
    _heap_val = _bright_lbl = _vol_lbl = _usb_val = _mute_val = nullptr;
    _autodim_val = _dimto_val = nullptr;
    _boot_val = nullptr;
    return true;
}
