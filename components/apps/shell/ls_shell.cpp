#include "shell/ls_shell.hpp"
#include "shell/ls_app.hpp"

#include "sdr_ui/sdr_ui.h"
#include "esp_log.h"
#include "esp_attr.h"

#include <cstring>

static const char *TAG = "ls_shell";

#define RAIL_H   76
#define ARROW_W  50
#define RAIL_BG  lv_color_hex(0x0C0E0D)

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


bool LsApp::back(void)           { return exitToLauncher(); }
bool LsApp::exitToLauncher(void) { return true; }


LsShell &LsShell::instance(void)
{
    static LsShell s;
    return s;
}

static void reset_content(lv_obj_t *c)
{
    lv_obj_clean(c);
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

    _content = lv_obj_create(_root);
    lv_obj_set_pos(_content, ARROW_W, 0);
    lv_obj_set_size(_content, hor - 2 * ARROW_W, ver - RAIL_H);
    reset_content(_content);

    _rail = lv_obj_create(_root);
    lv_obj_set_size(_rail, hor, RAIL_H);
    lv_obj_align(_rail, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(_rail, RAIL_BG, 0);
    lv_obj_set_style_bg_opa(_rail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(_rail, SDR_GOLD, 0);
    lv_obj_set_style_border_width(_rail, 1, 0);
    lv_obj_set_style_border_side(_rail, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_radius(_rail, 0, 0);
    lv_obj_set_style_pad_hor(_rail, 8, 0);
    lv_obj_set_style_pad_ver(_rail, 8, 0);
    lv_obj_set_style_pad_column(_rail, 6, 0);
    lv_obj_set_flex_flow(_rail, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(_rail, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(_rail, LV_OBJ_FLAG_SCROLLABLE);

    buildTabArrows();

    lv_scr_load(_root);
    ESP_LOGI(TAG, "shell begin: %dx%d, rail=%d", hor, ver, RAIL_H);
}

void LsShell::buildTabArrows(void)
{
    const int ver = lv_disp_get_ver_res(NULL);
    struct { lv_obj_t **slot; const char *txt; lv_align_t al; lv_event_cb_t cb; } a[] = {
        { &_tab_prev, "<", LV_ALIGN_TOP_LEFT,  tabPrevCb },
        { &_tab_next, ">", LV_ALIGN_TOP_RIGHT, tabNextCb },
    };
    for (auto &e : a) {
        lv_obj_t *b = lv_btn_create(_root);
        lv_obj_set_size(b, ARROW_W, ver - RAIL_H);
        lv_obj_align(b, e.al, 0, 0);
        lv_obj_set_style_bg_color(b, RAIL_BG, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, SDR_PANEL_HI, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(b, SDR_GOLD, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_side(b, e.al == LV_ALIGN_TOP_LEFT ? LV_BORDER_SIDE_RIGHT
                                                                  : LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_radius(b, 0, 0);
        lv_obj_add_event_cb(b, e.cb, LV_EVENT_CLICKED, this);
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(l, SDR_PAS_GOLD, 0);
        lv_label_set_text(l, e.txt);
        lv_obj_center(l);
        *e.slot = b;
    }
}

void LsShell::registerApp(LsApp *app)
{
    if (!app || _app_count >= MAX_APPS) return;
    app->init();
    _apps[_app_count++] = app;
}

void LsShell::buildRail(void)
{
    for (int i = 0; i < _app_count; i++) {
        lv_obj_t *b = lv_btn_create(_rail);
        lv_obj_set_height(b, RAIL_H - 18);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 3, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_pad_all(b, 2, 0);
        lv_obj_set_user_data(b, _apps[i]);
        lv_obj_add_event_cb(b, railBtnCb, LV_EVENT_CLICKED, this);

        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, sdr_font_mono_sm(), 0);
        lv_label_set_text(l, _apps[i]->name());
        lv_obj_center(l);
        _rail_btn[i] = b;
    }
    updateRail();
}

void LsShell::updateRail(void)
{
    for (int i = 0; i < _app_count; i++) {
        lv_obj_t *b = _rail_btn[i];
        if (!b) continue;
        bool active = (_apps[i] == _current);
        lv_obj_set_style_bg_color(b, active ? SDR_PAS_GOLD : lv_color_hex(0x1A1C1B), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(b, active ? SDR_BRIGHT : SDR_BORDER, 0);
        lv_obj_t *l = lv_obj_get_child(b, 0);
        if (l) lv_obj_set_style_text_color(l, active ? lv_color_hex(0x141008) : SDR_DIM, 0);
    }
}

void LsShell::start(const char *prefer)
{
    buildRail();

    const char *target = prefer ? prefer : load_last_app();

    if (!(target && launchByName(target)) && _app_count > 0)
        launch(_apps[0]);
}

void LsShell::launch(LsApp *app)
{
    if (!app) return;
    if (_current && _current != app) _current->close();
    _current = app;
    reset_content(_content);
    ESP_LOGI(TAG, "launch: %s", app->name());
    app->run(_content);
    updateRail();
    save_last_app(app->name());
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

void LsShell::cycleNext(void)
{
    if (_app_count <= 1) return;
    int cur = 0;
    for (int i = 0; i < _app_count; i++) if (_apps[i] == _current) { cur = i; break; }
    launch(_apps[(cur + 1) % _app_count]);
}

void LsShell::home(void)   { }

void LsShell::goBack(void) { if (_current) _current->back(); }


void LsShell::railBtnCb(lv_event_t *e)
{
    LsShell *self = static_cast<LsShell *>(lv_event_get_user_data(e));
    lv_obj_t *b = lv_event_get_target(e);
    LsApp *app = static_cast<LsApp *>(lv_obj_get_user_data(b));
    if (self && app && app != self->_current) self->launch(app);
}

void LsShell::tabPrevCb(lv_event_t *e)
{
    LsShell *self = static_cast<LsShell *>(lv_event_get_user_data(e));
    if (self && self->_current) self->_current->switchTab(-1);
}

void LsShell::tabNextCb(lv_event_t *e)
{
    LsShell *self = static_cast<LsShell *>(lv_event_get_user_data(e));
    if (self && self->_current) self->_current->switchTab(+1);
}
