#include "AppSettings.hpp"
#include "shell/ls_shell.hpp"
#include "sdr_ui/sdr_ui.h"
#include "settings/settings_action.h"
#include "ui/ls_ui.h"

#include <cstdio>
#include <cstring>

#include "bsp/esp-bsp.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "soc/lp_system_reg.h"
#include "soc/soc.h"

extern "C" {
#include "settings.h"
#include "location_pref.h"
#include "app_registry.h"
#include "link_ctl.h"  /**/
#include "audio_out.h"
#include "display_ctl.h"
/**/
#include "ls_version.h"
}

static const uint32_t SETTINGS_SAFETY_HOLD_MS = 3000U;

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
    /* Settings was the one screen left building directly on the app
     * parent.  That bypassed the composed header, scrollable framed content,
     * shared value rows and role-based controls. */
    ls_ui_screen_t screen;
    ls_ui_screen_create(parent, "SETTINGS", false, LS_UI_COLOR_DIM_TEXT, &screen);
    ls_ui_screen_set_readout(&screen, "DEVICE");
    ls_ui_screen_set_lamp(&screen, false, LS_UI_COLOR_DIM_TEXT);

    buildLocation(screen.content);
    buildWifi(screen.content);

    ls_ui_value_t r;
    lv_obj_t *display = ls_ui_panel(screen.content, "DISPLAY");
    _bright_lbl = nullptr;
    sdr_seg_slider(display, LS_UI_ACCENT, 100, display_ctl_get_user(),
                   brightnessCb, this, &_bright_lbl);

    if(LS_HAS_COMPACT_UI){
        ls_ui_value(display,"NAV AUTO-HIDE",&r);
        lv_label_set_text(r.value,LsShell::instance().navigationAutoHide()?"ON":"OFF");
        ls_ui_button(r.controls,"TOGGLE",LS_BTN_DEFAULT,[](lv_event_t *e){
            auto &shell=LsShell::instance();shell.setNavigationAutoHide(!shell.navigationAutoHide());
            lv_label_set_text(static_cast<lv_obj_t *>(lv_event_get_user_data(e)),shell.navigationAutoHide()?"ON":"OFF");
        },r.value,nullptr);
    }

    /**/
    ls_ui_value(display, "ACCENT", &r);
    _theme_val = r.value;
    _theme_btn = ls_ui_button(r.controls, "CHANGE", LS_BTN_PRIMARY,
                              themeCb, this, nullptr);

    ls_ui_value(display, "AUTO-DIM", &r);
    _autodim_val = r.value;
    _autodim_btn = ls_ui_button(r.controls, "TOGGLE", LS_BTN_DEFAULT,
                                autodimCb, this, nullptr);

    ls_ui_value(display, "DIM AFTER", &r);
    _dimto_val = r.value;
    _dimto_btn = ls_ui_button(r.controls, "CHANGE", LS_BTN_PRIMARY,
                              dimToCb, this, nullptr);

    lv_obj_t *audio = ls_ui_panel(screen.content, "AUDIO");
    _vol_lbl = nullptr;
    sdr_seg_slider(audio, LS_UI_ACCENT, 100, audio_volume_get(),
                   volumeCb, this, &_vol_lbl);
    ls_ui_value(audio, "MUTE", &r);
    _mute_val = r.value;
    _mute_btn = ls_ui_button(r.controls, "TOGGLE", LS_BTN_DEFAULT,
                             muteCb, this, nullptr);

    ls_ui_value(audio, "BOOT SOUND", &r);
    _boot_val = r.value;
    _boot_btn = ls_ui_button(r.controls, "CHANGE", LS_BTN_DEFAULT,
                             bootSndCb, this, nullptr);

    lv_obj_t *apps = ls_ui_panel(screen.content, "APPS");
    ls_ui_value(apps, "FILE BROWSER", &r);
    lv_label_set_text(r.value, "");
    ls_ui_button(r.controls, "OPEN", LS_BTN_PRIMARY, filesCb, this, nullptr);

    lv_obj_t *system = ls_ui_panel(screen.content, "SYSTEM");
    ls_ui_value(system, "USB AUTO-REBOOT", &r);
    _usb_val = r.value;
    _usb_btn = ls_ui_button(r.controls, "TOGGLE", LS_BTN_DEFAULT,
                            usbRebootCb, this, nullptr);

    lv_obj_t *destructive_hint = ls_ui_hold_hint(system, SETTINGS_SAFETY_HOLD_MS);

    ls_ui_value(system, "RESTART", &r);
    lv_label_set_text(r.value, "");
    ls_ui_hold_button_hinted(r.controls, "HOLD 3", SETTINGS_SAFETY_HOLD_MS,
                             LS_BTN_DANGER, rebootCb, this,
                             destructive_hint, "RESTART");

    ls_ui_value(system, "FLASH MODE", &r);
    lv_label_set_text(r.value, "download");
    ls_ui_hold_button_hinted(r.controls, "HOLD 3", SETTINGS_SAFETY_HOLD_MS,
                             LS_BTN_DANGER, dlModeCb, this,
                             destructive_hint, "FLASH MODE");

    lv_obj_t *about = ls_ui_panel(screen.content, "ABOUT");
    ls_ui_value(about, "FREE RAM", &r);
    _heap_val = r.value;
    lv_label_set_text(_heap_val, "...");

    ls_ui_value_t rr;
    ls_ui_value(about, "LAST RESET", &rr);
    lv_label_set_text(rr.value, reset_reason_str());

    /**/
    /* Which build is on this device.  Reads the same string the `version`
       console command prints, so a screenshot of this row is enough to
       identify a firmware.  The label wraps because the git-describe
       revision plus the [DIRTY] marker is wider than the value column. */
    char vbuf[LS_VERSION_LINE_MAX];
    ls_version_line(vbuf, sizeof(vbuf));
    ls_ui_value_t vr;
    ls_ui_value(about, "FIRMWARE", &vr);
    lv_label_set_long_mode(vr.value, LV_LABEL_LONG_WRAP);
    lv_label_set_text(vr.value, vbuf);

    _timer = lv_timer_create(timerCb, 1000, this);
    timerCb(_timer);
    return true;
}

void LsSettings::timerCb(lv_timer_t *t)
{
    LsSettings *self = static_cast<LsSettings *>(t->user_data);
    if (!self) return;
    self->refreshExternal();
    self->refreshValues();
}

void LsSettings::refreshExternal(void)
{
    refreshWifi();
    if (_heap_val) {
        char b[64];
        snprintf(b, sizeof(b), "%u KB int / %u KB psram",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        lv_label_set_text(_heap_val, b);
    }
}

static void apply_feedback(ls_settings_action_t action, int value,
                           lv_obj_t *label, lv_obj_t *button)
{
    ls_settings_feedback_t feedback;
    ls_settings_feedback(action, value, &feedback);
    if (label && feedback.text) lv_label_set_text(label, feedback.text);
    ls_ui_button_set_role(button, feedback.role);
}

void LsSettings::refreshValues(void)
{
    apply_feedback(LS_SETTINGS_ACTION_USB_AUTOREBOOT, app_usb_autoreboot(),
                   _usb_val, _usb_btn);
    apply_feedback(LS_SETTINGS_ACTION_MUTE, audio_is_muted(),
                   _mute_val, _mute_btn);
    apply_feedback(LS_SETTINGS_ACTION_AUTODIM, display_ctl_autodim_enabled(),
                   _autodim_val, _autodim_btn);
    if (_dimto_val)
        lv_label_set_text_fmt(_dimto_val, "%ds", display_ctl_autodim_timeout());
    ls_ui_button_set_role(_dimto_btn, LS_BTN_PRIMARY);
    if (_bright_lbl)
        lv_label_set_text_fmt(_bright_lbl, "BRIGHTNESS  %d", display_ctl_get_user());
    if (_vol_lbl)
        lv_label_set_text_fmt(_vol_lbl, "VOLUME  %d", audio_volume_get());
    /**/
    if (_theme_val) {
        lv_label_set_text(_theme_val, sdr_theme_name(sdr_theme_get()));
        lv_obj_set_style_text_color(_theme_val, sdr_accent(), 0);
    }
    ls_ui_button_set_role(_theme_btn, LS_BTN_PRIMARY);
    apply_feedback(LS_SETTINGS_ACTION_BOOT_SOUND, settings_get_boot_sound(),
                   _boot_val, _boot_btn);
}

void LsSettings::filesCb(lv_event_t *)
{
    LsShell::instance().launchByName("Files");
}

void LsSettings::brightnessCb(void *user_data, int value)
{
    LsSettings *self = static_cast<LsSettings *>(user_data);
    display_ctl_set_user(value);
    if (self) self->refreshValues();
}

void LsSettings::volumeCb(void *user_data, int value)
{
    LsSettings *self = static_cast<LsSettings *>(user_data);
    audio_volume_set(value);
    if (self) self->refreshValues();
}

void LsSettings::muteCb(lv_event_t *event)
{
    LsSettings *self = static_cast<LsSettings *>(lv_event_get_user_data(event));
    audio_toggle_mute();
    if (self) self->refreshValues();
}

void LsSettings::autodimCb(lv_event_t *event)
{
    LsSettings *self = static_cast<LsSettings *>(lv_event_get_user_data(event));
    display_ctl_set_autodim(ls_settings_action_next(LS_SETTINGS_ACTION_AUTODIM,
                                                    display_ctl_autodim_enabled(), 0));
    if (self) self->refreshValues();
}

void LsSettings::dimToCb(lv_event_t *event)
{
    LsSettings *self = static_cast<LsSettings *>(lv_event_get_user_data(event));
    display_ctl_set_autodim_timeout(ls_settings_action_next(
        LS_SETTINGS_ACTION_DIM_TIMEOUT, display_ctl_autodim_timeout(), 0));
    if (self) self->refreshValues();
}

void LsSettings::bootSndCb(lv_event_t *event)
{
    LsSettings *self = static_cast<LsSettings *>(lv_event_get_user_data(event));
    settings_set_boot_sound(ls_settings_action_next(LS_SETTINGS_ACTION_BOOT_SOUND,
                                                     settings_get_boot_sound(), 0));
    if (self) self->refreshValues();
}

/**/
void LsSettings::themeCb(lv_event_t *event)
{
    LsSettings *self = static_cast<LsSettings *>(lv_event_get_user_data(event));
    sdr_theme_t next = (sdr_theme_t)ls_settings_action_next(
        LS_SETTINGS_ACTION_THEME, sdr_theme_get(), SDR_THEME_COUNT);
    sdr_theme_set(next);
    settings_set_theme((int)next);
    if (self) self->refreshValues();
}

void LsSettings::usbRebootCb(lv_event_t *event)
{
    LsSettings *self = static_cast<LsSettings *>(lv_event_get_user_data(event));
    app_set_usb_autoreboot(ls_settings_action_next(LS_SETTINGS_ACTION_USB_AUTOREBOOT,
                                                   app_usb_autoreboot(), 0));
    if (self) self->refreshValues();
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

bool LsSettings::back(void) { closeLocationEntry(); return exitToLauncher(); }

/**/
bool LsSettings::pause(void)
{
    closeLocationEntry();
    closeWifiEntry();
    settings_wifi_active(false);
    if (_timer) lv_timer_pause(_timer);
    return true;
}

/**/
bool LsSettings::resume(void)
{
    settings_wifi_active(true);
    if (_timer) { lv_timer_resume(_timer); timerCb(_timer); }
    return true;
}

bool LsSettings::close(void)
{
    closeLocationEntry();
    _location_lat = _location_lon = _location_note = nullptr;
    for (auto &button : _location_buttons) button = nullptr;
    closeWifiEntry();
    settings_wifi_active(false);
    volatile char *secret = _wifi_password;
    for (size_t i = 0; i < sizeof(_wifi_password); ++i) secret[i] = 0;
    _wifi_ssid[0] = 0;
    _wifi_note[0] = 0;
    _wifi_status = _wifi_result = _wifi_ssid_label = _wifi_password_label = nullptr;
    _wifi_networks = _wifi_ssid_button = _wifi_password_button = nullptr;
    for (auto &button : _wifi_buttons) button = nullptr;
    if (_timer) { lv_timer_del(_timer); _timer = nullptr; }
    _heap_val = _bright_lbl = _vol_lbl = _usb_val = _mute_val = nullptr;
    /**/
    _theme_val = nullptr;
    _autodim_val = _dimto_val = nullptr;
    _boot_val = nullptr;
    _usb_btn = _mute_btn = _autodim_btn = nullptr;
    _dimto_btn = _boot_btn = _theme_btn = nullptr;
    return true;
}

void LsSettings::buildLocation(lv_obj_t *parent)
{
    float lat = 0, lon = 0;
    bool known = settings_get_home(&lat, &lon);
    _location_values[0] = lat; _location_values[1] = lon;
    _location_known[0] = _location_known[1] = known;
    auto *panel = ls_ui_panel(parent, "LOCATION (MANUAL)");
    ls_ui_value_t row;
    ls_ui_value(panel, "LATITUDE", &row); _location_lat = row.value;
    _location_buttons[0] = ls_ui_button(row.controls, "EDIT", LS_BTN_DEFAULT, locationCb, this, nullptr);
    ls_ui_value(panel, "LONGITUDE", &row); _location_lon = row.value;
    _location_buttons[1] = ls_ui_button(row.controls, "EDIT", LS_BTN_DEFAULT, locationCb, this, nullptr);
    ls_ui_value(panel, "REFERENCE", &row);
    _location_buttons[2] = ls_ui_button(row.controls, "SAVE", LS_BTN_PRIMARY, locationCb, this, nullptr);
    _location_buttons[3] = ls_ui_button(row.controls, "UNSET", LS_BTN_DEFAULT, locationCb, this, nullptr);
    /**/
    _location_note = ls_ui_note(panel, known ? "Stored manual reference" : "Unset: enter both coordinates");
    refreshLocation();
}

void LsSettings::refreshLocation()
{
    lv_obj_t *labels[2] = {_location_lat, _location_lon};
    for (int i = 0; i < 2; ++i) {
        char text[24];
        if (_location_known[i]) snprintf(text, sizeof(text), "%.6f", _location_values[i]);
        else snprintf(text, sizeof(text), "Unset");
        lv_label_set_text(labels[i], text);
    }
}

void LsSettings::locationCb(lv_event_t *e)
{
    auto *self = static_cast<LsSettings *>(lv_event_get_user_data(e));
    if (self->_location_entry || self->_wifi_entry) return;
    auto *target = lv_event_get_target(e);
    if (target == self->_location_buttons[2] || target == self->_location_buttons[3]) {
        bool unset = target == self->_location_buttons[3];
        if (!unset && (!self->_location_known[0] || !self->_location_known[1])) {
            lv_label_set_text(self->_location_note, "Enter both coordinates first");
            return;
        }
        bool ok = unset ? settings_clear_home() : settings_set_home(
            (float)self->_location_values[0], (float)self->_location_values[1]);
        if (ok && unset) self->_location_known[0] = self->_location_known[1] = false;
        lv_label_set_text(self->_location_note, ok ? "Accepted; save queued" : "Not accepted: settings unavailable or queue full");
        self->refreshLocation();
        return;
    }
    self->_location_axis = target == self->_location_buttons[1] ? 1 : 0;
    char value[24] = {};
    if (self->_location_known[self->_location_axis])
        snprintf(value, sizeof(value), "%.6f", self->_location_values[self->_location_axis]);
    ls_text_entry_config_t config = {};
    config.title = self->_location_axis ? "LONGITUDE (-180 TO 180)" : "LATITUDE (-90 TO 90)";
    config.text = value; config.accepted_chars = "0123456789.-+";
    config.max_length = 16; config.width = lv_pct(90); config.mode = LS_TEXT_ENTRY_NUMBER;
    self->_location_entry = ls_text_entry_open(&config, locationDone, self);
    if (!self->_location_entry) lv_label_set_text(self->_location_note, "Entry unavailable");
}

void LsSettings::locationDone(bool accepted, const char *text, void *user_data)
{
    auto *self = static_cast<LsSettings *>(user_data);
    self->_location_entry = nullptr;
    if (!accepted) return;
    double value;
    if (!location_parse(text, self->_location_axis != 0, &value)) {
        lv_label_set_text(self->_location_note, "Invalid coordinate or outside range");
        return;
    }
    self->_location_values[self->_location_axis] = value;
    self->_location_known[self->_location_axis] = true;
    self->refreshLocation();
    lv_label_set_text(self->_location_note, "Draft: SAVE applies both coordinates");
}

void LsSettings::closeLocationEntry()
{
    if (!_location_entry) return;
    auto *entry = _location_entry; _location_entry = nullptr;
    ls_text_entry_close(entry);
}

void LsSettings::buildWifi(lv_obj_t *parent)
{
    lv_obj_t *panel = ls_ui_panel(parent, "WI-FI");
    /**/
    _wifi_status = ls_ui_note(panel, "Reading station status...");
    lv_label_set_long_mode(_wifi_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_wifi_status, lv_pct(100));
    /**/
    _wifi_result = ls_ui_note(panel, "");
    lv_label_set_long_mode(_wifi_result, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_wifi_result, lv_pct(100));
    _wifi_networks = lv_dropdown_create(panel);
    lv_obj_set_width(_wifi_networks, lv_pct(100));
    ls_ui_style_content(_wifi_networks);
    lv_dropdown_set_options(_wifi_networks, "Scan to find networks");
    lv_obj_add_state(_wifi_networks, LV_STATE_DISABLED);
    lv_obj_add_event_cb(_wifi_networks, wifiSelectCb, LV_EVENT_VALUE_CHANGED, this);
    ls_ui_value_t row;
    ls_ui_value(panel, "NETWORK", &row);
    _wifi_ssid_label = row.value;
    _wifi_ssid_button = ls_ui_button(row.controls, "EDIT", LS_BTN_DEFAULT, wifiEntryCb, this, nullptr);
    ls_ui_value(panel, "PASSWORD", &row);
    _wifi_password_label = row.value;
    _wifi_password_button = ls_ui_button(row.controls, "EDIT", LS_BTN_DEFAULT, wifiEntryCb, this, nullptr);
    lv_obj_t *controls = ls_ui_controls(panel);
    const char *captions[] = { "SCAN", "CONNECT", "DISCONNECT", "SAVED", "FORGET" };
    for (int i = 0; i < 5; ++i)
        _wifi_buttons[i] = ls_ui_button(controls, captions[i], i == 1 ? LS_BTN_PRIMARY : LS_BTN_DEFAULT,
                                        wifiCb, this, nullptr);
    _wifi_scan_revision = 0;
    _wifi_available = settings_wifi_start();
    refreshWifi();
}

void LsSettings::refreshWifi()
{
    if (!_wifi_status) return;
    _wifi_available = settings_wifi_start();
    SettingsWifiSnapshot snapshot;
    settings_wifi_snapshot(&snapshot);
    lv_label_set_text(_wifi_status, _wifi_available ?
        (snapshot.status[0] ? snapshot.status : "Reading station status...") : "Wi-Fi worker unavailable: low memory");
    lv_label_set_text(_wifi_result, _wifi_note[0] ? _wifi_note : snapshot.result);
    lv_label_set_text(_wifi_ssid_label, _wifi_ssid[0] ? _wifi_ssid : "Select or edit");
    lv_label_set_text(_wifi_password_label, _wifi_password[0] ? "********" : "None (open network)");
    for (auto button : _wifi_buttons) {
        if (snapshot.busy || !_wifi_available) lv_obj_add_state(button, LV_STATE_DISABLED);
        else lv_obj_clear_state(button, LV_STATE_DISABLED);
    }
    if (!_wifi_ssid[0]) lv_obj_add_state(_wifi_buttons[1], LV_STATE_DISABLED);
    if (_wifi_scan_revision != snapshot.scan_revision) {
        _wifi_scan_revision = snapshot.scan_revision;
        _wifi_network_count = snapshot.count;
        memcpy(_wifi_scan, snapshot.networks, sizeof(_wifi_scan));
        lv_dropdown_clear_options(_wifi_networks);
        lv_dropdown_add_option(_wifi_networks, "Select network...", LV_DROPDOWN_POS_LAST);
        for (int i = 0; i < _wifi_network_count; ++i) {
            char text[64];
            snprintf(text, sizeof(text), "%s  %d dBm%s", _wifi_scan[i].ssid,
                     _wifi_scan[i].rssi, _wifi_scan[i].secure ? " *" : "");
            lv_dropdown_add_option(_wifi_networks, text, LV_DROPDOWN_POS_LAST);
        }
    }
    if (snapshot.busy || !_wifi_available || !_wifi_network_count)
        lv_obj_add_state(_wifi_networks, LV_STATE_DISABLED);
    else lv_obj_clear_state(_wifi_networks, LV_STATE_DISABLED);
}

void LsSettings::wifiSelectCb(lv_event_t *e)
{
    auto *self = static_cast<LsSettings *>(lv_event_get_user_data(e));
    int index = (int)lv_dropdown_get_selected(self->_wifi_networks) - 1;
    if (index < 0 || index >= self->_wifi_network_count) return;
    snprintf(self->_wifi_ssid, sizeof(self->_wifi_ssid), "%s", self->_wifi_scan[index].ssid);
    self->_wifi_note[0] = 0;
    memset(self->_wifi_password, 0, sizeof(self->_wifi_password));
    self->refreshWifi();
}

void LsSettings::wifiCb(lv_event_t *e)
{
    auto *self = static_cast<LsSettings *>(lv_event_get_user_data(e));
    const SettingsWifiCommand commands[] = { WIFI_SCAN, WIFI_JOIN, WIFI_LEAVE, WIFI_REJOIN, WIFI_FORGET };
    for (int i = 0; i < 5; ++i) {
        if (lv_event_get_target(e) != self->_wifi_buttons[i]) continue;
        self->_wifi_note[0] = 0;
        if (!settings_wifi_request(commands[i], self->_wifi_ssid, self->_wifi_password)) {
            snprintf(self->_wifi_note, sizeof(self->_wifi_note), "Use a network name and 8-63 byte password (or blank)");
            self->refreshWifi();
            return;
        }
        if (i == 1 || i == 4) memset(self->_wifi_password, 0, sizeof(self->_wifi_password));
        self->refreshWifi();
        break;
    }
}

void LsSettings::wifiEntryCb(lv_event_t *e)
{
    auto *self = static_cast<LsSettings *>(lv_event_get_user_data(e));
    if (self->_wifi_entry) return;
    self->_wifi_edit_password = lv_event_get_target(e) == self->_wifi_password_button;
    ls_text_entry_config_t config = {};
    config.title = self->_wifi_edit_password ? "WI-FI PASSWORD" : "WI-FI NETWORK";
    config.text = self->_wifi_edit_password ? self->_wifi_password : self->_wifi_ssid;
    config.placeholder = self->_wifi_edit_password ? "Blank for open network" : "Network name (SSID)";
    config.max_length = self->_wifi_edit_password ? 63 : 32;
    config.width = lv_pct(90);
    config.mode = LS_TEXT_ENTRY_TEXT;
    config.password = self->_wifi_edit_password;
    self->_wifi_entry = ls_text_entry_open(&config, wifiEntryDone, self);
}

void LsSettings::wifiEntryDone(bool accepted, const char *text, void *user_data)
{
    auto *self = static_cast<LsSettings *>(user_data);
    self->_wifi_entry = nullptr;
    if (accepted && text) {
        self->_wifi_note[0] = 0;
        if (strlen(text) > (self->_wifi_edit_password ? 63U : 32U)) {
            snprintf(self->_wifi_note, sizeof(self->_wifi_note), "Too long: network max 32 bytes; password max 63 bytes");
            self->refreshWifi();
            return;
        }
        if (self->_wifi_edit_password)
            snprintf(self->_wifi_password, sizeof(self->_wifi_password), "%s", text);
        else {
            snprintf(self->_wifi_ssid, sizeof(self->_wifi_ssid), "%s", text);
            memset(self->_wifi_password, 0, sizeof(self->_wifi_password));
        }
    }
    self->refreshWifi();
}

void LsSettings::closeWifiEntry()
{
    if (_wifi_entry) {
        auto *entry = _wifi_entry;
        _wifi_entry = nullptr;
        ls_text_entry_close(entry);
    }
}
