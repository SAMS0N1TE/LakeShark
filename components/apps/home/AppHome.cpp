#include "AppHome.hpp"
#include "shell/ls_shell.hpp"
#include "sdr_ui/sdr_ui.h"
#include "ui/ls_ui.h"
#include "home/home_widget_view.h"
#include "ui/ls_instrument.h"
#include "ls_board.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include "lakeshark_backend.h"
#include "settings.h"
#include "ls_time.h"
}

/**/
static const struct { const char *app; const char *icon; const char *title;
                      const char *sub; ls_ui_color_role_t accent; } TILES[] = {
    { "P25",      "p25",      "P25",   "DIGITAL VOICE", LS_UI_COLOR_ID_RED },
    { "FM",       "fm",       "FM",    "ANALOG / PAGE", LS_UI_COLOR_ID_TEAL },
    { "ADS-B",    "adsb",     "ADS-B", "AIR TRAFFIC",   LS_UI_COLOR_ID_BLUE },
    /* REC was reachable from the rail but had no tile here, so HOME
       did not show the one app that writes to the SD card. The "rec" icon
       already existed in the icon table. */
    { "REC",      "rec",      "REC",   "CAPTURE / SCOUT", LS_UI_COLOR_ID_ORANGE },
    { "ACARS",    "acars",    "ACARS", "AIRCRAFT TEXT", LS_UI_COLOR_ID_VIOLET },
    { "Files",    "files",    "FILES", "SD BROWSER",    LS_UI_COLOR_ID_STEEL },
    /**/
    { "MUSIC",    "files",    "MUSIC", "PLAYER",        LS_UI_COLOR_ID_ROSE },
    { "MAP",      "map",      "MAP",   "NAV / ADS-B",   LS_UI_COLOR_ID_GREEN },
    { "Settings", "settings", "CONFIG","DEVICE",        LS_UI_COLOR_DIM_TEXT },
};

AppHome::AppHome() : LsApp("HOME", "home") {}

/**/
void AppHome::tileCb(lv_event_t *e)
{
    const char *app = static_cast<const char *>(lv_event_get_user_data(e));
    if (app) LsShell::instance().launchByName(app);
}

/**/
void AppHome::faceCb(lv_event_t *e)
{
    auto *self = static_cast<AppHome *>(lv_event_get_user_data(e));
    if (!self || self->_widget != HOME_WIDGET_RECEIVER) return;
    if (self->_last_receiver_app[0])
        LsShell::instance().launchByName(self->_last_receiver_app);
}

bool AppHome::run(lv_obj_t *parent)
{
    /* pause hides the hub subscription; reconstruction must restore
     * visibility even though the lightweight app descriptor is reused. */
    _visible = true;
    _widget = (home_widget_id_t)settings_get_home_widget();
#if LS_HAS_COMPACT_UI
    buildInstrument(parent);
#else
    ls_ui_screen_t screen;
    ls_ui_screen_create(parent, nullptr, false, LS_UI_COLOR_ACCENT, &screen);
    _screen_readout = screen.readout;
    _screen_lamp = screen.lamp;
    ls_ui_screen_set_readout(&screen, "OVERVIEW");
    parent = screen.content;
    /* measured square-panel overflow was 5 px. Recover 6 px of
     * vertical padding while preserving every font and control dimension. */
    lv_obj_set_style_pad_ver(parent,5,0);

    buildFace(parent);
    buildTiles(parent);
#endif

    /* HOME is passive. Entering it must not choose a backend, unpark
      a receiver, or retune merely to make the face non-empty. */

    /**/
    lv_obj_update_layout(parent);

    _sub = ls_hub_subscribe(hubCb, this);
    /**/
    _theme_sub = sdr_theme_on_change(themeCb, this);
    apply(ls_hub_state(), LS_HUB_ALL);
    _clock_timer = lv_timer_create(clockCb, 1000, this);
    return true;
}

#if LS_HAS_COMPACT_UI
void AppHome::buildInstrument(lv_obj_t *parent)
{
    auto *v=ls_instrument_create(parent,"LAKESHARK","01 / FIELD DESK",192,52,52);
    if(!v)return;
    _mode=ls_instrument_text(v->left,"LAST RECEIVER");
    lv_obj_set_pos(_mode,0,8);lv_obj_set_size(_mode,lv_pct(100),32);
    _face=ls_instrument_button(v->left,"---.----",faceCb,this,&_freq);
    lv_obj_set_pos(_face,0,44);lv_obj_set_size(_face,lv_pct(100),64);
    lv_obj_set_style_text_font(_freq,&lv_font_montserrat_32,0);
    _detail=ls_instrument_text(v->left,"Select a receiver from the directory.");
    lv_obj_set_pos(_detail,0,124);lv_obj_set_size(_detail,lv_pct(100),56);
    lv_label_set_long_mode(_detail,LV_LABEL_LONG_WRAP);
    _save_status=ls_instrument_text(v->left,"");
    lv_obj_set_pos(_save_status,0,184);lv_obj_set_size(_save_status,lv_pct(100),32);
    lv_obj_add_flag(_save_status,LV_OBJ_FLAG_HIDDEN);
    static const char *choices[]={"RECEIVER","SYSTEM","CLOCK"};
    for(int i=0;i<HOME_WIDGET_COUNT;i++)
        _widget_buttons[i]=ls_instrument_button(v->footer,choices[i],widgetCb,this);
    ls_instrument_list(v->footer,3,52);
    for(unsigned i=0;i<sizeof(TILES)/sizeof(TILES[0]);i++) {
        char text[64];snprintf(text,sizeof(text),"%02u / %-6s  %s",i+1,TILES[i].title,TILES[i].sub);
        ls_instrument_button(v->right,text,tileCb,(void *)TILES[i].app);
    }
    lv_obj_add_event_cb(v->right,[](lv_event_t *e){
        auto *p=lv_event_get_target(e);
        int columns=lv_obj_get_width(lv_obj_get_parent(p))>=800?2:1;
        int rows=(9+columns-1)/columns;
        int row=lv_obj_get_content_height(p)/rows;
        int width=lv_obj_get_content_width(p)/columns;
        for(unsigned i=0;i<lv_obj_get_child_cnt(p);i++){
            auto *b=lv_obj_get_child(p,i);
            lv_obj_set_pos(b,(i%columns)*width,(i/columns)*row);
            lv_obj_set_size(b,width-(columns>1?8:0),row-6);
        }
    },LV_EVENT_SIZE_CHANGED,nullptr);
    lv_event_send(v->right,LV_EVENT_SIZE_CHANGED,nullptr);
}
#endif

/**/
void AppHome::themeCb(void *ud)
{
    AppHome *self = static_cast<AppHome *>(ud);
    ls_ui_frame_color(self->_face, sdr_accent_dim());
    self->apply(ls_hub_state(), LS_HUB_ALL);
}

/**/
void AppHome::buildFace(lv_obj_t *parent)
{
    /* Stable IDs and a single picker table are the extension point for future
     * implemented widgets. No unavailable transport is advertised here. */
    static const char *choices[HOME_WIDGET_COUNT] = { "RECEIVER", "SYSTEM", "CLOCK" };
    auto *picker = ls_ui_button_group(parent);
    for (int i = 0; i < HOME_WIDGET_COUNT; ++i) {
        _widget_buttons[i] = ls_ui_group_button(picker, choices[i],
            i == _widget ? LS_BTN_TOGGLE_ON : LS_BTN_TOGGLE_OFF,
            widgetCb, this, nullptr);
    }
    _face = sdr_lcd_panel(parent, sdr_accent_dim());
    lv_obj_add_flag(_face, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_face, faceCb, LV_EVENT_CLICKED, this);

    _mode = sdr_value(_face, &lv_font_montserrat_20, SDR_IDLE);
    lv_obj_set_width(_mode, lv_pct(100));
    lv_label_set_long_mode(_mode, LV_LABEL_LONG_WRAP);
    lv_label_set_text(_mode, "--");

    _freq = sdr_value(_face, &lv_font_montserrat_32, SDR_TEXT);
    lv_obj_set_width(_freq, lv_pct(100));
    lv_obj_set_style_text_align(_freq, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(_freq, LV_LABEL_LONG_WRAP);
    lv_label_set_text(_freq, "---.----");

    /* Prose, not a column of figures, so it does not need the fixed-pitch
       face - and asking for one costs height and width it cannot spare: the
       mono face is 8 px per character and on the 720x720 panel this line
       wrapped and pushed the picker into a scrollbar. */
    _detail = sdr_value(_face, sdr_font_ui(), SDR_IDLE);
    lv_obj_set_width(_detail, lv_pct(100));
    lv_obj_set_style_text_align(_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(_detail, "STANDBY");

    lv_label_set_long_mode(_detail, LV_LABEL_LONG_WRAP);
    _save_status = sdr_value(_face, sdr_font_mono_sm(), SDR_WARN);
    lv_obj_set_width(_save_status, lv_pct(100));
    lv_label_set_long_mode(_save_status, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(_save_status, LV_OBJ_FLAG_HIDDEN);
}

void AppHome::widgetCb(lv_event_t *e)
{
    auto *self = static_cast<AppHome *>(lv_event_get_user_data(e));
    for (int i = 0; i < HOME_WIDGET_COUNT; ++i) {
        if (lv_event_get_target(e) != self->_widget_buttons[i]) continue;
        if (!settings_set_home_widget(i)) {
            lv_label_set_text(self->_save_status, "Selection not saved - try again");
            lv_obj_clear_flag(self->_save_status, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        self->_widget = (home_widget_id_t)i;
        lv_obj_add_flag(self->_save_status, LV_OBJ_FLAG_HIDDEN);
        self->refreshWidget(ls_hub_state());
        return;
    }
}

void AppHome::clockCb(lv_timer_t *timer)
{
    auto *self = static_cast<AppHome *>(timer->user_data);
    if (self->_visible && self->_widget == HOME_WIDGET_CLOCK)
        self->refreshWidget(ls_hub_state());
}

void AppHome::refreshWidget(const ls_hub_state_t *s)
{
    if (!_face) return;
    if (s && s->freq_hz && s->target_app[0]) {
        _last_freq_hz = s->freq_hz;
        snprintf(_last_receiver_app, sizeof(_last_receiver_app), "%s", s->target_app);
    }
    ls_hub_state_t receiver = {};
    receiver.freq_hz = _last_freq_hz;
    snprintf(receiver.target_app, sizeof(receiver.target_app), "%s", _last_receiver_app);
    home_widget_view_t view;
    home_widget_present(_widget, _widget == HOME_WIDGET_RECEIVER ? &receiver : s,
        ls_time_is_synced(), time(nullptr), &view);
    sdr_text_if_changed(_mode, view.title);
    sdr_text_if_changed(_freq, view.value);
    sdr_text_if_changed(_detail, view.detail);
#if LS_HAS_COMPACT_UI
    sdr_color_if_changed(_mode,ls_instrument_accent());
    sdr_color_if_changed(_freq,ls_instrument_ink());
    for(int i=0;i<HOME_WIDGET_COUNT;i++)ls_instrument_select(_widget_buttons[i],i==_widget);
#else
    sdr_color_if_changed(_mode, sdr_accent());
    sdr_color_if_changed(_freq, SDR_TEXT);
    for (int i = 0; i < HOME_WIDGET_COUNT; ++i)
        ls_ui_button_set_role(_widget_buttons[i], i == _widget
            ? LS_BTN_TOGGLE_ON : LS_BTN_TOGGLE_OFF);
#endif
    sdr_color_if_changed(_detail, SDR_LABEL);
    if (_widget == HOME_WIDGET_RECEIVER && _last_receiver_app[0])
        lv_obj_add_flag(_face, LV_OBJ_FLAG_CLICKABLE);
    else lv_obj_clear_flag(_face, LV_OBJ_FLAG_CLICKABLE);
}

/**/
void AppHome::buildTiles(lv_obj_t *parent)
{
    sdr_section(parent, "LAUNCH");

    lv_obj_t *grid = ls_ui_controls(parent);
    lv_obj_update_layout(parent);
    int columns = lv_obj_get_content_width(parent) / 128;
    if (columns < 2) columns = 2;
    if (columns > 4) columns = 4;

    const int n = (int)(sizeof(TILES) / sizeof(TILES[0]));
    for (int i = 0; i < n; i++) {
        lv_obj_t *t = sdr_tile(grid, TILES[i].icon, TILES[i].title, TILES[i].sub,
                               tileCb, (void *)TILES[i].app);
        lv_obj_set_width(t, lv_pct(100 / columns - 1));
        for (uint32_t child = 0; child < lv_obj_get_child_cnt(t); ++child) {
            auto *item = lv_obj_get_child(t, child);
            if (!lv_obj_check_type(item, &lv_label_class)) continue;
            lv_obj_set_width(item, lv_pct(100));
            lv_label_set_long_mode(item, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(item, LV_TEXT_ALIGN_CENTER, 0);
        }
        sdr_tile_accent(t, ls_ui_color(TILES[i].accent));
    }
}

void AppHome::hubCb(const ls_hub_state_t *s, uint32_t dirty, void *ud)
{
    static_cast<AppHome *>(ud)->apply(s, dirty);
}

/**/
void AppHome::apply(const ls_hub_state_t *s, uint32_t dirty)
{
    if (!_visible || !s) return;
    refreshWidget(s);
}

/**/
bool AppHome::pause(void)
{
    _visible = false;
    if (_clock_timer) lv_timer_pause(_clock_timer);
    return true;
}

/**/
bool AppHome::resume(void)
{
    _visible = true;
    if (_clock_timer) lv_timer_resume(_clock_timer);
    apply(ls_hub_state(), LS_HUB_ALL);
    return true;
}

bool AppHome::close(void)
{
    _visible = false;
    if (_clock_timer) { lv_timer_del(_clock_timer); _clock_timer = nullptr; }
    if (_sub >= 0) { ls_hub_unsubscribe(_sub); _sub = -1; }
    /**/
    if (_theme_sub >= 0) { sdr_theme_off_change(_theme_sub); _theme_sub = -1; }
    _face = _mode = _freq = _detail = nullptr;
    _save_status = nullptr;
    for (auto &button : _widget_buttons) button = nullptr;
    return true;
}

/**/
void AppHome::switchTab(int delta) { LsShell::instance().cycleApp(delta); }
