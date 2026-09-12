#include "shell/ls_statusbar.hpp"
#include "sdr_ui/sdr_ui.h"

#include <cstdio>
#include <cstring>

#define BAR_SEGS 6

/**/
static lv_obj_t *glyph(lv_obj_t *parent, const char *sym)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, sdr_font_mono_sm(), 0);
    lv_obj_set_style_text_color(l, SDR_DIM, 0);
    lv_label_set_text(l, sym);
    return l;
}

/**/
lv_obj_t *LsStatusBar::build(lv_obj_t *parent, int w, lv_event_cb_t tap, void *ud)
{
    _bar = lv_obj_create(parent);
    lv_obj_set_size(_bar, w, SDR_STATUS_H);
    lv_obj_set_pos(_bar, 0, 0);
    lv_obj_set_style_bg_color(_bar, SDR_CHASSIS, 0);
    lv_obj_set_style_bg_opa(_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(_bar, SDR_RULE, 0);
    lv_obj_set_style_border_width(_bar, 1, 0);
    lv_obj_set_style_border_side(_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(_bar, 0, 0);
    lv_obj_set_style_pad_hor(_bar, 8, 0);
    lv_obj_set_style_pad_ver(_bar, 0, 0);
    lv_obj_set_style_pad_column(_bar, 7, 0);
    lv_obj_set_flex_flow(_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(_bar, LV_OBJ_FLAG_SCROLLABLE);

    _dot = lv_obj_create(_bar);
    lv_obj_set_size(_dot, 8, 8);
    lv_obj_set_style_bg_color(_dot, SDR_DIM, 0);
    lv_obj_set_style_bg_opa(_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_dot, 0, 0);
    lv_obj_set_style_radius(_dot, 0, 0);
    lv_obj_set_style_pad_all(_dot, 0, 0);
    lv_obj_clear_flag(_dot, LV_OBJ_FLAG_SCROLLABLE);

    if(LS_HAS_COMPACT_UI){
        _title=sdr_value(_bar,sdr_font_mono_sm(),SDR_TEXT);
        lv_obj_set_width(_title,0);lv_obj_set_flex_grow(_title,1);
        lv_label_set_long_mode(_title,LV_LABEL_LONG_DOT);
        lv_label_set_text(_title,"LAKESHARK");
    }
    _mode = sdr_value(_bar, sdr_font_mono_sm(), SDR_TEXT);
    lv_obj_set_style_text_letter_space(_mode, 1, 0);
    lv_label_set_text(_mode, "--");

    _bars = sdr_value(_bar, sdr_font_mono_sm(), SDR_DIM);
    lv_label_set_text(_bars, "------");

    _freq = sdr_value(_bar, sdr_font_mono(), SDR_BRIGHT);
    lv_obj_set_flex_grow(_freq, LS_HAS_COMPACT_UI?0:1);
    lv_obj_set_style_text_align(_freq, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(_freq, "---.----");

    if(LS_HAS_COMPACT_UI){lv_obj_add_flag(_mode,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(_bars,LV_OBJ_FLAG_HIDDEN);}

    _bat = sdr_value(_bar, sdr_font_mono_sm(), SDR_DIM);
    lv_label_set_text(_bat, "");
    lv_obj_add_flag(_bat, LV_OBJ_FLAG_HIDDEN);

    _usb = glyph(_bar, LV_SYMBOL_USB);
    _sd  = glyph(_bar, LV_SYMBOL_SD_CARD);
    _bt  = glyph(_bar, LV_SYMBOL_BLUETOOTH);
    _vol = glyph(_bar, LV_SYMBOL_VOLUME_MAX);

    if (tap) {
        lv_obj_add_flag(_bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(_bar, tap, LV_EVENT_CLICKED, ud);
    }

    _sub = ls_hub_subscribe(hubCb, this);
    /**/
    _theme_sub=sdr_theme_on_change(themeCb,this);
    return _bar;
}

void LsStatusBar::hubCb(const ls_hub_state_t *s, uint32_t dirty, void *ud)
{
    static_cast<LsStatusBar *>(ud)->apply(s, dirty);
}

/**/
void LsStatusBar::themeCb(void *ud)
{
    static_cast<LsStatusBar *>(ud)->apply(ls_hub_state(), LS_HUB_ALL);
}

/**/
void LsStatusBar::apply(const ls_hub_state_t *s, uint32_t dirty)
{
    /**/
    if (dirty & (LS_HUB_RADIO | LS_HUB_SIGNAL)) {
        lv_color_t dc = !s->rtl_ready ? SDR_ERR
                      : s->parked     ? SDR_WARN
                      : s->active     ? sdr_accent()
                                      : SDR_IDLE;
        if (lv_obj_get_style_bg_color(_dot, 0).full != dc.full)
            lv_obj_set_style_bg_color(_dot, dc, 0);

        sdr_text_if_changed(_mode, s->rtl_ready ? s->mode : "NO RTL");
        sdr_color_if_changed(_mode, s->rtl_ready ? SDR_TEXT : SDR_ERR);
    }

    if (dirty & LS_HUB_RADIO) {
        sdr_color_if_changed(_usb, s->rtl_ready ? SDR_TEXT : SDR_DIM);
        sdr_color_if_changed(_sd,  s->sd_present ? SDR_TEXT : SDR_DIM);
        sdr_color_if_changed(_bt,  s->c6_state == 1 ? SDR_TEXT : SDR_DIM);

        if (s->batt_pct >= 0) {
            char b[8];
            snprintf(b, sizeof(b), "%d%%", s->batt_pct);
            sdr_text_if_changed(_bat, b);
            sdr_color_if_changed(_bat, s->batt_pct < 15 ? SDR_ERR
                                     : s->batt_pct < 35 ? SDR_WARN : SDR_TEXT);
            lv_obj_clear_flag(_bat, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(_bat, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (dirty & LS_HUB_SIGNAL) {
        char bar[BAR_SEGS + 2];
        sdr_ascii_bar(bar, sizeof(bar), s->rtl_ready ? s->sig_pct : 0, BAR_SEGS);
        sdr_text_if_changed(_bars, bar);
        sdr_color_if_changed(_bars, !s->rtl_ready    ? SDR_OFF
                                  : s->sig_pct >= 95 ? SDR_ERR
                                  : s->active        ? sdr_accent()
                                  : s->sig_pct < 8   ? SDR_OFF : SDR_IDLE);
    }

    if (dirty & (LS_HUB_TUNE | LS_HUB_RADIO)) {
        char f[24];
        if (s->freq_hz)
            snprintf(f, sizeof(f), "%lu.%04lu",
                     (unsigned long)(s->freq_hz / 1000000UL),
                     (unsigned long)((s->freq_hz / 100UL) % 10000UL));
        else
            snprintf(f, sizeof(f), "---.----");
        sdr_text_if_changed(_freq, f);
        /**/
        sdr_color_if_changed(_freq, s->rtl_ready ? SDR_BRIGHT : SDR_OFF);
    }

    if (dirty & LS_HUB_AUDIO) {
        sdr_text_if_changed(_vol, s->muted ? LV_SYMBOL_MUTE
                                 : s->volume >= 50 ? LV_SYMBOL_VOLUME_MAX
                                                   : LV_SYMBOL_VOLUME_MID);
        sdr_color_if_changed(_vol, s->muted ? SDR_ERR : SDR_TEXT);
    }
}

void LsStatusBar::resize(int width,int x,int y)
{
    if(!_bar)return;
    lv_obj_set_width(_bar,width);lv_obj_set_pos(_bar,x,y);
    if(LS_HAS_COMPACT_UI && _freq){
        if(width<800)lv_obj_add_flag(_freq,LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(_freq,LV_OBJ_FLAG_HIDDEN);
    }
}
void LsStatusBar::setTitle(const char *app,const char *page)
{
    if(!_title)return;
    char text[96];snprintf(text,sizeof(text),"%s%s%s",app?app:"LAKESHARK",page && *page?" / ":"",page?page:"");
    sdr_text_if_changed(_title,text);
}

void LsStatusBar::detach()
{
    if(_sub>=0)ls_hub_unsubscribe(_sub);
    if(_theme_sub>=0)sdr_theme_off_change(_theme_sub);
    _sub=_theme_sub=-1;
}
