#include "shell/ls_shell.hpp"
#include "shell/ls_app.hpp"
#include "shell/ls_hub.h"
#include "shell/ls_icons.h"

#include "sdr_ui/sdr_ui.h"
#include "esp_log.h"
#include "esp_attr.h"
/*LS-720*/
#include "esp_system.h"

#include <cstring>

static const char *TAG = "ls_shell";

#define RAIL_H   SDR_RAIL_H
#define RAIL_BG  SDR_CHASSIS

#define LAST_MAGIC 0x4C415354u
static RTC_NOINIT_ATTR uint32_t s_last_magic;
static RTC_NOINIT_ATTR char     s_last_app[24];

static void save_last_app(const char *name)
{
    if (!name) return;
    strncpy(s_last_app, name, sizeof(s_last_app) - 1);
    s_last_app[sizeof(s_last_app) - 1] = 0;
    s_last_magic = LAST_MAGIC;
}

static const char *load_last_app(void)
{
    return (s_last_magic == LAST_MAGIC && s_last_app[0]) ? s_last_app : nullptr;
}

/*LS-715*/
/* Resuming the last app turns any crash-on-open into a boot loop: the app
   faults, the board reboots, the shell opens the same app, it faults again,
   and there is no way back to HOME from the field. So arm a marker across the
   reboot before opening an app and clear it once the app has survived a few
   seconds. A marker still set at startup means that app did not survive being
   opened, and we refuse to open it again. */
#define ENTER_MAGIC 0x4C534E54u
static RTC_NOINIT_ATTR uint32_t s_enter_magic;
static RTC_NOINIT_ATTR char     s_enter_app[24];

static void enter_arm(const char *name)
{
    if (!name) return;
    strncpy(s_enter_app, name, sizeof(s_enter_app) - 1);
    s_enter_app[sizeof(s_enter_app) - 1] = 0;
    s_enter_magic = ENTER_MAGIC;
}

static void enter_disarm(void) { s_enter_magic = 0; s_enter_app[0] = 0; }

static const char *enter_pending(void)
{
    return (s_enter_magic == ENTER_MAGIC && s_enter_app[0]) ? s_enter_app : nullptr;
}

static void enter_survived_cb(lv_timer_t *) { enter_disarm(); }


bool LsApp::back(void)           { return exitToLauncher(); }
bool LsApp::exitToLauncher(void) { return true; }


LsShell &LsShell::instance(void)
{
    static LsShell s;
    return s;
}

static void style_container(lv_obj_t *c)
{
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_pad_row(c, 0, 0);
    lv_obj_set_style_pad_column(c, 0, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_set_style_bg_color(c, SDR_BG, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
}

void LsShell::begin(void)
{
    _root = lv_obj_create(nullptr);
    sdr_style_screen(_root);
    lv_obj_set_style_pad_all(_root, 0, 0);
    lv_obj_set_style_border_width(_root, 0, 0);
    lv_obj_clear_flag(_root, LV_OBJ_FLAG_SCROLLABLE);

    const int hor = lv_disp_get_hor_res(NULL);
    const int ver = lv_disp_get_ver_res(NULL);

    /*LS-606*/
    sdr_theme_init();
    sdr_theme_on_change(themeCb, this);

    /*LS-602*/
    ls_hub_start();

    /*LS-603*/
    _status.build(_root, hor, statusTapCb, this);

    /*LS-905*/
    _content = lv_obj_create(_root);
    lv_obj_set_pos(_content, 0, SDR_STATUS_H);
    lv_obj_set_size(_content, hor, ver - RAIL_H - SDR_STATUS_H);
    style_container(_content);

    _rail = lv_obj_create(_root);
    lv_obj_set_size(_rail, hor, RAIL_H);
    lv_obj_align(_rail, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(_rail, RAIL_BG, 0);
    lv_obj_set_style_bg_opa(_rail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(_rail, SDR_RULE, 0);
    lv_obj_set_style_border_width(_rail, 1, 0);
    lv_obj_set_style_border_side(_rail, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_radius(_rail, 0, 0);
    lv_obj_set_style_pad_hor(_rail, 4, 0);
    lv_obj_set_style_pad_ver(_rail, 0, 0);
    lv_obj_set_style_pad_column(_rail, 2, 0);
    lv_obj_set_flex_flow(_rail, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(_rail, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(_rail, LV_OBJ_FLAG_SCROLLABLE);

    /*LS-905*/
    lv_obj_add_event_cb(_root, gestureCb, LV_EVENT_GESTURE, this);

    lv_scr_load(_root);
    ESP_LOGI(TAG, "shell begin: %dx%d, status=%d rail=%d",
             hor, ver, SDR_STATUS_H, RAIL_H);
}

/*LS-606*/
void LsShell::themeCb(void *ud)
{
    LsShell *self = static_cast<LsShell *>(ud);
    if (self) self->updateRail();
}

/*LS-603*/
void LsShell::statusTapCb(lv_event_t *e)
{
    LsShell *self = static_cast<LsShell *>(lv_event_get_user_data(e));
    if (self) self->home();
}

/*LS-905*/
void LsShell::gestureCb(lv_event_t *e)
{
    LsShell *self = static_cast<LsShell *>(lv_event_get_user_data(e));
    if (!self || !self->_current) return;

    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;

    switch (lv_indev_get_gesture_dir(indev)) {
    case LV_DIR_LEFT:  self->_current->switchTab(+1); break;
    case LV_DIR_RIGHT: self->_current->switchTab(-1); break;
    default: break;
    }
}

void LsShell::registerApp(LsApp *app, bool show_in_rail)
{
    if (!app || _app_count >= MAX_APPS) return;
    app->init();
    _rail_hidden[_app_count] = !show_in_rail;
    _apps[_app_count++] = app;
}

/*LS-605*/
void LsShell::buildRail(void)
{
    for (int i = 0; i < _app_count; i++) {
        if (_rail_hidden[i]) { _rail_btn[i] = nullptr; continue; }
        lv_obj_t *b = lv_btn_create(_rail);
        lv_obj_set_height(b, RAIL_H - 1);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 0, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_border_side(b, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_set_style_pad_row(b, 3, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_user_data(b, _apps[i]);
        lv_obj_add_event_cb(b, railBtnCb, LV_EVENT_CLICKED, this);

        lv_obj_t *ic = lv_img_create(b);
        lv_img_set_src(ic, ls_icon_for(_apps[i]->icon(), 32));
        lv_obj_set_style_img_recolor_opa(ic, LV_OPA_COVER, 0);

        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, sdr_font_mono_sm(), 0);
        lv_obj_set_style_text_letter_space(l, 1, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_label_set_text(l, _apps[i]->name());
        _rail_btn[i] = b;
    }
    updateRail();
}

/*LS-605*/
void LsShell::updateRail(void)
{
    for (int i = 0; i < _app_count; i++) {
        lv_obj_t *b = _rail_btn[i];
        if (!b) continue;
        const bool active = (_apps[i] == _current);

        lv_obj_set_style_bg_color(b, active ? sdr_accent_bg() : RAIL_BG, 0);
        lv_obj_set_style_border_color(b, active ? sdr_accent() : RAIL_BG, 0);

        lv_obj_t *ic = lv_obj_get_child(b, 0);
        if (ic) lv_obj_set_style_img_recolor(ic, active ? sdr_accent() : SDR_DIM, 0);
        lv_obj_t *l = lv_obj_get_child(b, 1);
        if (l) lv_obj_set_style_text_color(l, active ? sdr_accent() : SDR_DIM, 0);
    }
}

void LsShell::start(const char *prefer)
{
    buildRail();

    const char *target  = prefer ? prefer : load_last_app();

    /*LS-715*/
    /*LS-720*/
    /* Only a FAULT reset means the app killed us. A flash, the reset button or
       a power-on also leave the marker armed if they land inside the 5 s
       window, and treating those as a crash sent every reflash to HOME and
       made the guard cry wolf. Brownout is excluded deliberately: a sagging
       battery is not the app's fault, and blaming it would hide the real
       cause behind a wrong diagnosis. */
    const esp_reset_reason_t rr = esp_reset_reason();
    const bool fault_reset = (rr == ESP_RST_PANIC)   || (rr == ESP_RST_INT_WDT) ||
                             (rr == ESP_RST_TASK_WDT) || (rr == ESP_RST_WDT);

    const char *pending = enter_pending();
    if (fault_reset && !prefer && pending && target && strcmp(pending, target) == 0) {
        ESP_LOGW(TAG, "%s did not survive being opened last boot (reset reason %d) - "
                      "starting at HOME instead", pending, (int)rr);
        target = nullptr;
    }
    enter_disarm();

    if (!(target && launchByName(target)) && _app_count > 0)
        launch(_apps[0]);
}

/*LS-600*/
int LsShell::indexOf(LsApp *app) const
{
    for (int i = 0; i < _app_count; i++) if (_apps[i] == app) return i;
    return -1;
}

/*LS-600*/
lv_obj_t *LsShell::containerFor(int idx)
{
    if (idx < 0 || idx >= _app_count) return nullptr;
    if (_app_cont[idx]) return _app_cont[idx];

    lv_obj_t *c = lv_obj_create(_content);
    lv_obj_set_pos(c, 0, 0);
    lv_obj_set_size(c, lv_pct(100), lv_pct(100));
    style_container(c);
    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    _app_cont[idx] = c;
    return c;
}

/*LS-600*/
void LsShell::launch(LsApp *app)
{
    if (!app || app == _current) return;

    const int idx = indexOf(app);
    if (idx < 0) return;

    if (_current) {
        const int prev = indexOf(_current);
        /*LS-604*/
        if (app->passive()) _current->background();
        else                _current->pause();
        if (prev >= 0 && _app_cont[prev])
            lv_obj_add_flag(_app_cont[prev], LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *cont = containerFor(idx);
    if (!cont) return;

    const bool first = !_app_built[idx];
    _current = app;

    lv_obj_clear_flag(cont, LV_OBJ_FLAG_HIDDEN);

    /*LS-715*/
    enter_arm(app->name());

    if (first) {
        ESP_LOGI(TAG, "build+launch: %s", app->name());
        _app_built[idx] = true;
        app->run(cont);
    } else {
        ESP_LOGI(TAG, "resume: %s", app->name());
        app->resume();
    }

    updateRail();
    save_last_app(app->name());

    /*LS-715*/
    if (lv_timer_t *g = lv_timer_create(enter_survived_cb, 5000, nullptr))
        lv_timer_set_repeat_count(g, 1);
}

/*LS-600*/
void LsShell::closeAll(void)
{
    for (int i = 0; i < _app_count; i++) {
        if (!_app_built[i]) continue;
        if (_apps[i]) _apps[i]->close();
        _app_built[i] = false;
        if (_app_cont[i]) { lv_obj_del(_app_cont[i]); _app_cont[i] = nullptr; }
    }
    _current = nullptr;
}

bool LsShell::launchByName(const char *name)
{
    if (!name || !*name) return false;
    for (int i = 0; i < _app_count; i++) {
        if (_apps[i] && _apps[i]->name() && strcmp(_apps[i]->name(), name) == 0) {
            launch(_apps[i]);
            return true;
        }
    }
    return false;
}

void LsShell::cycleNext(void) { cycleApp(+1); }

/*LS-604*/
void LsShell::cycleApp(int delta)
{
    if (_app_count <= 1) return;
    if (delta == 0) delta = 1;

    int cur = 0;
    for (int i = 0; i < _app_count; i++) if (_apps[i] == _current) { cur = i; break; }

    const int dir = delta > 0 ? 1 : -1;
    int n = cur;
    for (int step = 1; step <= _app_count; step++) {
        n += dir;
        if (n < 0) n = _app_count - 1;
        if (n >= _app_count) n = 0;
        if (!_rail_hidden[n]) { launch(_apps[n]); return; }
    }
}

/*LS-604*/
void LsShell::home(void) { launchByName("HOME"); }

void LsShell::goBack(void) { if (_current) _current->back(); }


void LsShell::railBtnCb(lv_event_t *e)
{
    LsShell *self = static_cast<LsShell *>(lv_event_get_user_data(e));
    lv_obj_t *b = lv_event_get_target(e);
    LsApp *app = static_cast<LsApp *>(lv_obj_get_user_data(b));
    if (self && app && app != self->_current) self->launch(app);
}

