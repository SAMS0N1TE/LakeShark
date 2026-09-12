#include "shell/ls_shell.hpp"
#include "ls_board.h"
#include <cstdint>
#include "shell/ls_app.hpp"
#include "shell/ls_hub.h"
#include "shell/ls_icons.h"
#include "shell/ls_input.h"
#include "shell/ls_tx_confirmation.h"

#include "sdr_ui/sdr_ui.h"
/**/
#include "ui/ls_shade.h"
#include "esp_log.h"
#include "esp_attr.h"
/**/
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "ui/ls_ui.h"
#include <cstdio>

extern "C" {
/**/
#include "shell/ls_shell_nav.h"
/**/
#include "ls_safe_mode.h"
#include "app_registry.h"
#include "settings.h"
}

#include <cstring>

static const char *TAG = "ls_shell";

#define RAIL_H   SDR_RAIL_H
#define RAIL_BG  SDR_CHASSIS

#define LAST_MAGIC UINT32_C(1279349588)
static RTC_NOINIT_ATTR uint32_t s_last_magic;
static RTC_NOINIT_ATTR char     s_last_app[24];

static void save_last_app(const char *name)
{
    if (!name) return;
    strncpy(s_last_app, name, sizeof(s_last_app) - 1);
    s_last_app[sizeof(s_last_app) - 1] = 0;
    s_last_magic = LAST_MAGIC;
    /* Copy it into the safe-mode record too. That record is
       magic/version/CRC-validated, so the safe screen can name the app the
       board was on without trusting a bare RTC string it did not write. */
    ls_safe_app(name);
}

static const char *load_last_app(void)
{
    return (s_last_magic == LAST_MAGIC && s_last_app[0]) ? s_last_app : nullptr;
}

/**/
/* Resuming the last app turns any crash-on-open into a boot loop: the app
   faults, the board reboots, the shell opens the same app, it faults again,
   and there is no way back to HOME from the field. So arm a marker across the
   reboot before opening an app and clear it once the app has survived a few
   seconds. A marker still set at startup means that app did not survive being
   opened, and we refuse to open it again. */
#define ENTER_MAGIC UINT32_C(1280527956)
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

static void enter_survived_cb(lv_timer_t *timer)
{
    enter_disarm();
    lv_timer_pause(timer);
}

bool LsApp::back(void)           { return exitToLauncher(); }

/**/
/* Was a no-op that just returned true.  Every app whose back() delegated here
   (P25, FM, ADS-B, Map, Media, REC, Settings, Files at the roots) therefore
   never left the app.  Route through the shared nav gateway which the shell
   installs in begin() and the bench drives with counting hooks. */
bool LsApp::exitToLauncher(void)
{
    LsShell &s = LsShell::instance();
    ls_shell_nav_exit_to_launcher(s.current() == this, name());
    return true;
}

LsShell &LsShell::instance(void)
{
    static LsShell s;
    return s;
}

/**/
/* Trampoline the C-language nav gateway back into the C++ shell.  Kept as a
   file-scope function so begin() can hand its address to ls_shell_nav_configure
   without a std::function or capturing lambda. */
static void ls_shell_go_home_hook(void *)
{
    LsShell::instance().home();
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
    ls_transition_init(&_transition);
    ls_input_init();
    ls_input_set_back_handler([](){LsShell::instance().goBack();});

    _root = lv_obj_create(nullptr);
    sdr_style_screen(_root);
    lv_obj_set_style_pad_all(_root, 0, 0);
    lv_obj_set_style_border_width(_root, 0, 0);
    lv_obj_clear_flag(_root, LV_OBJ_FLAG_SCROLLABLE);

    const int hor = lv_disp_get_hor_res(NULL);
    const int ver = lv_disp_get_ver_res(NULL);

    _surface=lv_obj_create(_root);
    style_container(_surface);
    lv_obj_set_pos(_surface,0,0);
    lv_obj_set_size(_surface,hor,ver);

    /**/
    sdr_theme_init();
    sdr_theme_on_change(themeCb, this);

    /**/
    ls_hub_start();

    /**/
    const ls_shell_nav_hooks_t nav_hooks = { ls_shell_go_home_hook, nullptr, nullptr };
    ls_shell_nav_configure(&nav_hooks);

    /**/
    /* No tap-to-home. */

    _status.build(_surface, hor, [](lv_event_t *e){
        (void)e;
        if (!ls_shade_is_open()) ls_shade_open();
    }, this);

    /**/
    _content = lv_obj_create(_surface);
    lv_obj_set_pos(_content, 0, SDR_STATUS_H);
    lv_obj_set_size(_content, hor, ver - RAIL_H - SDR_STATUS_H);
    style_container(_content);
    /* Preallocate the tiny transition surface while startup has headroom.
     * The status label borrows its fixed buffer instead of reallocating each
     * animation frame during the low-memory unload window. */
    _loading = ls_ui_panel(_content, nullptr);
    lv_obj_set_size(_loading, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_align(_loading, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    _loading_text = sdr_label(_loading, sdr_font_mono_sm(), LS_UI_TEXT);
    lv_obj_set_width(_loading_text, lv_pct(100));
    lv_obj_set_style_text_align(_loading_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(_loading, LV_OBJ_FLAG_HIDDEN);
    _transition_timer = lv_timer_create(transitionCb, 50, this);

    _rail = lv_obj_create(_surface);
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
    if (LS_HAS_COMPACT_UI) {
        lv_obj_add_event_cb(_rail,[](lv_event_t *e){
            static_cast<LsShell *>(lv_event_get_user_data(e))->showNavigation(false);
        },LV_EVENT_CANCEL,this);
        lv_obj_clear_flag(_rail,LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(_rail,[](lv_event_t *e){
            auto *input=lv_indev_get_act();
            if(input && (lv_indev_get_gesture_dir(input)==LV_DIR_BOTTOM || lv_indev_get_gesture_dir(input)==LV_DIR_RIGHT))
                static_cast<LsShell *>(lv_event_get_user_data(e))->showNavigation(false);
        },LV_EVENT_GESTURE,this);
        _nav_auto=settings_get_nav_autohide();
        _nav_visible=!_nav_auto;
        lv_obj_set_flex_align(_rail,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_START);
        lv_obj_add_flag(_rail, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(_rail, LV_DIR_HOR);
        lv_obj_set_scrollbar_mode(_rail, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(_rail, LV_OBJ_FLAG_SCROLL_CHAIN);
    } else lv_obj_clear_flag(_rail, LV_OBJ_FLAG_SCROLLABLE);

    if(LS_HAS_COMPACT_UI){
        _nav_handle=lv_btn_create(_surface);
        lv_obj_set_style_pad_all(_nav_handle,0,0);
        lv_obj_set_style_radius(_nav_handle,0,0);
        lv_obj_set_style_bg_color(_nav_handle,SDR_BG,0);
        lv_obj_set_style_shadow_width(_nav_handle,0,0);
        lv_obj_set_style_border_width(_nav_handle,1,0);
        lv_obj_set_style_border_color(_nav_handle,SDR_RULE,0);
        lv_obj_clear_flag(_nav_handle,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_flag(_nav_handle,LV_OBJ_FLAG_PRESS_LOCK);
        auto *label=lv_label_create(_nav_handle);
        lv_obj_set_style_text_font(label,sdr_font_mono_sm(),0);
        lv_obj_set_style_text_color(label,SDR_TEXT,0);
        lv_label_set_text(label,"^  APPS");lv_obj_center(label);
        lv_obj_add_event_cb(_nav_handle,[](lv_event_t *e){
            auto *self=static_cast<LsShell *>(lv_event_get_user_data(e));
            auto *input=lv_indev_get_act();lv_point_t point={};
            if(input)lv_indev_get_point(input,&point);
            if(lv_event_get_code(e)==LV_EVENT_PRESSED){self->_nav_press_y=point.y;self->_nav_press_x=point.x;}
            else if(lv_event_get_code(e)==LV_EVENT_RELEASED){
                bool wide=lv_disp_get_hor_res(nullptr)>lv_disp_get_ver_res(nullptr);
                int delta=wide?point.x-self->_nav_press_x:point.y-self->_nav_press_y;
                self->showNavigation(delta < -20?true:delta > 20?false:!self->_nav_visible);
            }
        },LV_EVENT_ALL,this);
    }
    /**/
    lv_obj_add_event_cb(_root, gestureCb, LV_EVENT_GESTURE, this);
    /**/
    lv_obj_add_event_cb(_root, pressedCb, LV_EVENT_PRESSED, this);

    /* Shade is a child of _root so it inherits the screen's coord
       space and z-order under the same input group. Built once and reused;
       open/close is a hidden-flag flip. */
    ls_shade_build(_surface);

    lv_scr_load(_root);
    ls_input_set_screen(_root);
    ESP_LOGI(TAG, "shell begin: %dx%d, status=%d rail=%d",
             hor, ver, SDR_STATUS_H, RAIL_H);
}

void LsShell::setSafeInsets(int horizontal,int vertical)
{
    _safe_x=horizontal>0?horizontal:0;
    _safe_y=vertical>0?vertical:0;
    ls_ui_set_safe_insets(_safe_x,_safe_y);
    ls_shade_set_safe_insets(_safe_x,_safe_y);
    resize();
}
void LsShell::setRoundedViewport(int radius,int edge)
{
    _round_radius=radius>0?radius:0;
    _edge=edge>0?edge:0;
    resize();
}
void LsShell::resize(void)
{
    if (!_root) return;
    int hor=lv_disp_get_hor_res(nullptr), ver=lv_disp_get_ver_res(nullptr);
    if(LS_HAS_COMPACT_UI){_safe_y=hor>ver?12:24;_safe_x=hor>ver?64:32;}
    hor-=2*_edge; ver-=2*_edge;
    lv_obj_set_pos(_surface,_edge,_edge);
    lv_obj_set_size(_surface,hor,ver);
    lv_obj_set_style_radius(_surface,_round_radius,0);
    /* The child bounds already clear the curved corners. Masking the entire
     * subtree makes every changing label pay for a screen-sized clip. */
    lv_obj_set_style_clip_corner(_surface,false,0);
    lv_obj_set_style_border_width(_surface,_round_radius>0?1:0,0);
    lv_obj_set_style_border_color(_surface,SDR_RULE,0);
    lv_obj_update_layout(_surface);
    hor=lv_obj_get_content_width(_surface);
    ver=lv_obj_get_content_height(_surface);
    ls_ui_set_safe_insets(_safe_x+_edge,_safe_y+_edge);
    /* Two different insets, and conflating them cost 128 px of width. */

    const bool side=LS_HAS_COMPACT_UI && hor>ver;
    const int corner_x=LS_HAS_COMPACT_UI?(side?40:28):_safe_x;
    const int body_x=LS_HAS_COMPACT_UI?12:_safe_x;
    /* The body's lower edge is the one corner it can reach.  Landscape holds
       it further off the glass so the 12 px sides stay clear of the arc. */
    const int bottom_gap=(LS_HAS_COMPACT_UI&&side)?28:_safe_y;
    _status.resize(hor-2*corner_x,corner_x,_safe_y);
    lv_obj_set_pos(_content,body_x,_safe_y+SDR_STATUS_H);
    int chrome_x=corner_x;
    /* Portrait's rail is RAIL_H tall and sits _safe_y off the bottom,
       so reserving the 24 px handle let it cover the last RAIL_H of content
       and the action row disappeared under the app bar. Reserve what the rail
       actually occupies. It is still a constant, so hiding the rail does not
       resize the page. Landscape keeps 24: there the rail is a side overlay
       and only the handle sits in the content's lane. */
    int reserved=!LS_HAS_COMPACT_UI?RAIL_H:(side?24:RAIL_H);
    lv_obj_set_size(_content,hor-2*body_x-(side?reserved:0),
                    ver-_safe_y-bottom_gap-SDR_STATUS_H-(side?0:reserved));
    if(side){
        lv_obj_set_flex_flow(_rail,LV_FLEX_FLOW_COLUMN);
        lv_obj_set_pos(_rail,hor-124,82);
        lv_obj_set_size(_rail,116,ver-134);
    }else{
        lv_obj_set_flex_flow(_rail,LV_FLEX_FLOW_ROW);
        lv_obj_set_size(_rail,hor-2*chrome_x,RAIL_H);
        lv_obj_align(_rail,LV_ALIGN_BOTTOM_MID,0,-_safe_y);
    }
    for(int i=0;i<_app_count;i++)if(_rail_btn[i]){
        lv_obj_set_flex_grow(_rail_btn[i],0);
        lv_obj_set_width(_rail_btn[i],side?108:112);
        lv_obj_set_height(_rail_btn[i],side?44:RAIL_H-1);
    }
    lv_obj_set_scroll_dir(_rail,side?LV_DIR_VER:LV_DIR_HOR);
    if(_nav_down_button){lv_obj_set_width(_nav_down_button,side?108:112);lv_obj_set_height(_nav_down_button,side?44:RAIL_H-1);}
    if(LS_HAS_COMPACT_UI && _nav_handle){
        if(side){
            lv_obj_set_pos(_nav_handle,hor-32,82);
            lv_obj_set_size(_nav_handle,24,ver-134);
        }else{
            lv_obj_set_size(_nav_handle,hor-2*chrome_x,24);
            lv_obj_align(_nav_handle,LV_ALIGN_BOTTOM_MID,0,-_safe_y);
        }
        lv_label_set_text(lv_obj_get_child(_nav_handle,0),side?"<":"^  APPS");
        if(_nav_visible){lv_obj_clear_flag(_rail,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(_nav_handle,LV_OBJ_FLAG_HIDDEN);}
        else {lv_obj_add_flag(_rail,LV_OBJ_FLAG_HIDDEN);lv_obj_clear_flag(_nav_handle,LV_OBJ_FLAG_HIDDEN);}
        lv_obj_move_foreground(_rail);lv_obj_move_foreground(_nav_handle);
    }
    ls_shade_resize();
    lv_obj_update_layout(_root);
}

/**/
void LsShell::themeCb(void *ud)
{
    LsShell *self = static_cast<LsShell *>(ud);
    if (self) self->updateRail();
}

/**/
/* Vertical gestures drive the pull-down shade. LEFT/RIGHT forward to
   the current app's tab-switch on the larger panels. Compact touch controls
   keep horizontal drags for sliders and use explicit tab buttons instead.
   LV_DIR_BOTTOM is a downward swipe,
   LV_DIR_TOP an upward one - LVGL 8 names the direction the finger travels
   in, not the edge it points at. */
/* Where the finger went down, so a downward swipe can tell "pull the
   shade" from "scroll this panel". Without it the shade opened on any downward
   swipe anywhere, which ate the scroll gesture on every scrollable tab - on
   REC the controls below the fold could not be reached at all. */
lv_coord_t LsShell::_press_y = 0;

void LsShell::pressedCb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    _press_y = p.y;
}

void LsShell::gestureCb(lv_event_t *e)
{
    LsShell *self = static_cast<LsShell *>(lv_event_get_user_data(e));
    if (!self || self->transitioning()) return;

    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;

    const lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    switch (dir) {
    case LV_DIR_BOTTOM:
        if(LS_HAS_COMPACT_UI && self->_nav_visible &&
           _press_y>=lv_disp_get_ver_res(nullptr)-self->_safe_y-32-RAIL_H){
            self->showNavigation(false);break;
        }
        /* Only from the top edge, the way a phone does it. A swipe that
           starts in the body belongs to whatever is under it. The band is the
           status bar plus a little, so it stays reachable with a thumb. */
        if (_press_y > self->_edge + self->_safe_y + SDR_STATUS_H * 2) break;
        if (!ls_shade_is_open()) ls_shade_open();
        break;
    case LV_DIR_TOP:
        if(LS_HAS_COMPACT_UI && _press_y>=lv_disp_get_ver_res(nullptr)-self->_safe_y-32)self->showNavigation(true);
        else if (ls_shade_is_open()) ls_shade_close();
        break;
    case LV_DIR_LEFT:
#if !LS_HAS_COMPACT_UI
        if (self->_current) self->_current->switchTab(+1);
#endif
        break;
    case LV_DIR_RIGHT:
#if !LS_HAS_COMPACT_UI
        if (self->_current) self->_current->switchTab(-1);
#endif
        break;
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

/**/
void LsShell::buildRail(void)
{
    if(LS_HAS_COMPACT_UI){
        _nav_down_button=ls_ui_button(_rail,"DOWN",LS_BTN_DEFAULT,[](lv_event_t *e){
            auto *self=static_cast<LsShell *>(lv_event_get_user_data(e));self->showNavigation(false);
        },this,nullptr);
        lv_obj_set_style_min_width(_nav_down_button,112,0);lv_obj_set_width(_nav_down_button,112);lv_obj_set_height(_nav_down_button,RAIL_H-1);
    }
    for (int i = 0; i < _app_count; i++) {
        if (_rail_hidden[i]) { _rail_btn[i] = nullptr; continue; }
        lv_obj_t *b = lv_btn_create(_rail);
        sdr_button_guard_swipe(b);
        lv_obj_set_height(b, RAIL_H - 1);
        lv_obj_set_flex_grow(b, LS_HAS_COMPACT_UI ? 0 : 1);
        if (LS_HAS_COMPACT_UI) lv_obj_set_width(b, 112);
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

        if(!LS_HAS_COMPACT_UI){
            lv_obj_t *ic = lv_img_create(b);
            lv_img_set_src(ic, ls_icon_for(_apps[i]->icon(), 32));
            lv_obj_set_style_img_recolor_opa(ic, LV_OPA_COVER, 0);
        }

        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, sdr_font_mono_sm(), 0);
        lv_obj_set_style_text_letter_space(l, 1, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_label_set_text(l, _apps[i]->name());
        _rail_btn[i] = b;
    }
    updateRail();
    resize();
}

/**/
void LsShell::updateRail(void)
{
    for (int i = 0; i < _app_count; i++) {
        lv_obj_t *b = _rail_btn[i];
        if (!b) continue;
        const bool active = (_apps[i] == _current);

        lv_obj_set_style_bg_color(b, active ? sdr_accent_bg() : RAIL_BG, 0);
        lv_obj_set_style_border_color(b, active ? sdr_accent() : RAIL_BG, 0);

        if(!LS_HAS_COMPACT_UI){
            lv_obj_t *ic = lv_obj_get_child(b, 0);
            if(ic)lv_obj_set_style_img_recolor(ic, active?sdr_accent():SDR_DIM,0);
        }
        lv_obj_t *l = lv_obj_get_child(b, LS_HAS_COMPACT_UI?0:1);
        if (l) lv_obj_set_style_text_color(l, active ? sdr_accent() : SDR_DIM, 0);
    }
}

void LsShell::start(const char *prefer)
{
    buildRail();

    const char *target  = prefer ? prefer : load_last_app();

    /**/
    /**/
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

/**/
int LsShell::indexOf(LsApp *app) const
{
    for (int i = 0; i < _app_count; i++) if (_apps[i] == app) return i;
    return -1;
}

/**/
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

/**/
void LsShell::launch(LsApp *app)
{
    if (!app || !_transition_timer) return;
    const int idx = indexOf(app);
    if (idx < 0) return;
    ls_transition_request(&_transition, idx);
}

/**/
void LsShell::closeAll(void)
{
    ls_transition_request(&_transition, -1);
}

static void transition_memory(const char *stage)
{
    ESP_LOGI(TAG, "transition %s: internal=%u/%u DMA=%u/%u PSRAM=%u", stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

uint32_t LsShell::transitionStop(void *ctx)
{
    LsShell *self = static_cast<LsShell *>(ctx);
    ls_tx_confirmation_dismiss();
    self->_transition_error = nullptr;
    if (self->_entry_timer) {
        lv_timer_del(self->_entry_timer);
        self->_entry_timer = nullptr;
    }
    /* Set the registry fence before pause(), so its legacy park calls cannot
     * use the synchronous fallback or race a new backend start. */
    uint32_t token = app_ui_park_request();
    if (self->_current && !self->_current->pause()) return 0;
    ls_input_focus_modal(self->_loading);
    transition_memory("before unload");
    return token;
}

int LsShell::transitionStopped(void *ctx, uint32_t token)
{
    LsShell *self = static_cast<LsShell *>(ctx);
    const int radio = app_ui_park_status(token);
    /* parking the receiver does not retire Settings' Wi-Fi worker.
     * Loading P25 during an outstanding scan would overlap its internal stack. */
    return ls_transition_dependencies_stopped(radio,
        !self->_current || self->_current->stopped());
}

bool LsShell::transitionUnload(void *ctx, int)
{
    LsShell *self = static_cast<LsShell *>(ctx);
    for (int i = 0; i < self->_app_count; ++i) {
        if (!self->_app_built[i]) continue;
        if (!self->_apps[i]->close()) return false;
        self->_app_built[i] = false;
        if (self->_app_cont[i]) {
            lv_obj_del(self->_app_cont[i]);
            self->_app_cont[i] = nullptr;
        }
    }
    self->_current = nullptr;
    ls_input_refresh_focus();
    transition_memory("after unload");
    return true;
}

bool LsShell::transitionBuild(void *ctx, int idx)
{
    LsShell *self = static_cast<LsShell *>(ctx);
    if (idx < 0) return true;
    const size_t available = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    if (!ls_transition_memory_admit(available, largest, dma_largest)) {
        self->_transition_error = "Low memory. Tap an app to retry.";
        ESP_LOGE(TAG, "load refused: 8bit=%u/%u DMA largest=%u (minimal floor)",
                 (unsigned)available, (unsigned)largest, (unsigned)dma_largest);
        return false;
    }
    lv_obj_t *cont = self->containerFor(idx);
    if (!cont) return false;
    LsApp *app = self->_apps[idx];
    self->_current = app;
    self->_transition.current = idx;
    self->_app_built[idx] = true;
    enter_arm(app->name());
    app_ui_park_release();
    ls_ui_clear_page_title();
    if (!app->run(cont)) {
        (void)app_ui_park_request();
        (void)app->pause();
        ESP_LOGE(TAG, "construction failed: %s", app->name());
        return false;
    }
    transition_memory("after construction");
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_HIDDEN);
    self->updateRail();
    ls_input_focus_screen();
    save_last_app(app->name());
    /* Keep one owned timer: an old app's timer must not disarm the next
     * app's crash-on-entry marker after rapid navigation. */
    self->_entry_timer = lv_timer_create(enter_survived_cb, 5000, nullptr);
    if (self->_entry_timer) lv_timer_set_repeat_count(self->_entry_timer, -1);
    return true;
}

void LsShell::showTransition(void)
{
    if (!_loading) return;
    if (_transition.phase == LS_TRANS_IDLE) {
        lv_obj_add_flag(_loading, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const char *target = _transition.target >= 0 ? _apps[_transition.target]->name() : "HOME";
    const char *stage = _transition.phase == LS_TRANS_FAILED
        ? (_transition_error ? _transition_error : "Could not stop or load. Tap an app to retry.")
        : _transition.phase == LS_TRANS_BUILD ? "Loading" : "Stopping and unloading";
    char caption[sizeof(_loading_caption)];
    snprintf(caption, sizeof(caption), "%s\n%s", stage, target);
    if (!strcmp(caption,_loading_caption) &&
        !lv_obj_has_flag(_loading,LV_OBJ_FLAG_HIDDEN)) return;
    memcpy(_loading_caption,caption,strlen(caption)+1);
    lv_label_set_text_static(_loading_text, _loading_caption);
    lv_obj_clear_flag(_loading, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_loading);
}

void LsShell::transitionCb(lv_timer_t *timer)
{
    LsShell *self = static_cast<LsShell *>(timer->user_data);
    if(LS_HAS_COMPACT_UI){
        self->_status.setTitle(self->_current?self->_current->name():"LAKESHARK",ls_ui_page_title());
        if(self->_nav_auto && self->_nav_visible && lv_disp_get_inactive_time(nullptr)>5000)self->showNavigation(false);
    }
    if (self->_transition.phase == LS_TRANS_IDLE) return;
    const ls_transition_hooks_t hooks = {transitionStop, transitionStopped,
                                         transitionUnload, transitionBuild, self};
    const auto before=self->_transition.phase;
    self->showTransition();
    ls_transition_tick(&self->_transition, &hooks, lv_tick_get(), 10000);
    if (before!=self->_transition.phase)
        ESP_LOGI(TAG,"transition phase %d -> %d target=%d token=%lu radio=%d",
            (int)before,(int)self->_transition.phase,self->_transition.target,
            (unsigned long)self->_transition.token,app_ui_park_status(self->_transition.token));
    self->showTransition();
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

/**/
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

/**/
void LsShell::home(void) { launchByName("HOME"); }

/**/
/* Old form was `if (_current) _current->back();` which threw away the return
   value.  That silently dropped the "app did not handle it, take me home"
   signal, so back on any app whose back() returned false stranded the user.
   The gateway routes an unhandled back to HOME.

   The transmit confirmation is dismissed first, and unconditionally: a
   pending authorisation must not outlive the screen that asked for it, and
   that has to hold whether or not the app handles the back itself. */
void LsShell::goBack(void)
{
    if(LS_HAS_COMPACT_UI && _nav_visible){showNavigation(false);return;}
    ls_tx_confirmation_dismiss();
    if (transitioning()) { home(); return; }
    if (!_current) return;
    const bool handled = _current->back();
    ls_shell_nav_dispatch_back(handled);
}

void LsShell::railBtnCb(lv_event_t *e)
{
    LsShell *self = static_cast<LsShell *>(lv_event_get_user_data(e));
    lv_obj_t *b = lv_event_get_target(e);
    LsApp *app = static_cast<LsApp *>(lv_obj_get_user_data(b));
    if (self && app) {self->launch(app);if(self->_nav_auto)self->showNavigation(false);}
}

void LsShell::showNavigation(bool visible)
{
    if(!LS_HAS_COMPACT_UI || !_rail)return;
    _nav_visible=visible;
    lv_disp_trig_activity(nullptr);
    resize();
    if(visible)ls_input_focus_modal(_rail);else ls_input_focus_screen();
}
void LsShell::setNavigationAutoHide(bool enabled)
{
    _nav_auto=enabled;settings_set_nav_autohide(enabled);
    if(!enabled)showNavigation(true);
}
