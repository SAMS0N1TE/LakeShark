#include "sdr_ui.h"

#include <cstdlib>
#include <cstring>

LV_FONT_DECLARE(lv_font_consolas_16);
LV_FONT_DECLARE(lv_font_consolas_14);

const lv_font_t *sdr_font_mono(void)    { return &lv_font_consolas_16; }
const lv_font_t *sdr_font_mono_sm(void) { return &lv_font_consolas_14; }
const lv_font_t *sdr_font_ui(void)      { return &lv_font_montserrat_14; }

void sdr_style_screen(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, SDR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
}

void sdr_style_tabview(lv_obj_t *tv)
{
    lv_obj_set_style_bg_color(tv, SDR_BG, 0);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, 0);

    lv_obj_t *btns = lv_tabview_get_tab_btns(tv);
    if (btns) {
        lv_obj_set_style_bg_color(btns, SDR_BG, 0);
        lv_obj_set_style_bg_opa(btns, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btns, SDR_BORDER, 0);
        lv_obj_set_style_border_width(btns, 1, 0);
        lv_obj_set_style_border_side(btns, LV_BORDER_SIDE_BOTTOM, 0);

        lv_obj_set_style_text_font(btns, sdr_font_ui(), LV_PART_ITEMS);
        lv_obj_set_style_text_color(btns, SDR_LABEL, LV_PART_ITEMS);
        lv_obj_set_style_bg_opa(btns, LV_OPA_0, LV_PART_ITEMS);
        lv_obj_set_style_border_width(btns, 0, LV_PART_ITEMS);

        lv_obj_set_style_text_color(btns, SDR_BRIGHT, LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(btns, SDR_PANEL, LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(btns, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_border_color(btns, SDR_BORDER_HI, LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_border_width(btns, 2, LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_border_side(btns, LV_BORDER_SIDE_BOTTOM,
                                     LV_PART_ITEMS | LV_STATE_CHECKED);
    }
    lv_obj_t *content = lv_tabview_get_content(tv);
    if (content) {
        lv_obj_set_style_bg_color(content, SDR_BG, 0);
        lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    }
}

lv_obj_t *sdr_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

lv_obj_t *sdr_panel(lv_obj_t *parent)
{

    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_width(p, lv_pct(100));
    lv_obj_set_height(p, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(p, SDR_SUNKEN, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(p, SDR_BEVEL_LO, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_outline_color(p, SDR_BORDER, 0);
    lv_obj_set_style_outline_width(p, 1, 0);
    lv_obj_set_style_outline_pad(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_style_pad_all(p, 8, 0);
    lv_obj_set_style_pad_row(p, 4, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

lv_obj_t *sdr_section(lv_obj_t *parent, const char *title)
{

    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, SDR_BRIGHT, 0);
    lv_obj_set_style_border_color(l, SDR_BORDER, 0);
    lv_obj_set_style_border_width(l, 1, 0);
    lv_obj_set_style_border_side(l, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_bottom(l, 4, 0);
    lv_obj_set_style_pad_top(l, 4, 0);
    lv_label_set_text(l, title);
    return l;
}

lv_obj_t *sdr_btn(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, void *ud,
                  lv_obj_t **out_lbl)
{

    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_height(b, 50);
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    lv_obj_set_style_min_width(b, 56, 0);
    lv_obj_set_style_bg_color(b, SDR_BTN, 0);
    lv_obj_set_style_bg_color(b, SDR_BTN_HI, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, SDR_BORDER_HI, 0);
    lv_obj_set_style_border_color(b, SDR_BEVEL_LO, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 16, 0);
    lv_obj_set_style_pad_ver(b, 6, 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, sdr_font_ui(), 0);
    lv_obj_set_style_text_color(l, SDR_BRIGHT, 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    if (out_lbl) *out_lbl = l;
    return b;
}

void sdr_setting_row(lv_obj_t *parent, const char *name, sdr_setrow_t *out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, SDR_PANEL, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(row, SDR_BORDER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_hor(row, 10, 0);
    lv_obj_set_style_pad_ver(row, 6, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name_l = sdr_label(row, sdr_font_ui(), SDR_TEXT);
    lv_label_set_text(name_l, name);
    lv_obj_set_flex_grow(name_l, 1);

    lv_obj_t *val = sdr_label(row, sdr_font_mono(), SDR_CYAN);
    lv_label_set_text(val, "");
    lv_label_set_long_mode(val, LV_LABEL_LONG_CLIP);

    lv_obj_t *ctl = lv_obj_create(row);
    lv_obj_set_height(ctl, LV_SIZE_CONTENT);
    lv_obj_set_width(ctl, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(ctl, LV_OPA_0, 0);
    lv_obj_set_style_border_width(ctl, 0, 0);
    lv_obj_set_style_pad_all(ctl, 0, 0);
    lv_obj_set_style_pad_column(ctl, 6, 0);
    lv_obj_set_flex_flow(ctl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctl, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(ctl, LV_OBJ_FLAG_SCROLLABLE);

    if (out) {
        out->row = row;
        out->name = name_l;
        out->value = val;
        out->controls = ctl;
    }
}


lv_obj_t *sdr_lcd_panel(lv_obj_t *parent, lv_color_t edge)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_width(p, lv_pct(100));
    lv_obj_set_height(p, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(p, SDR_LCD_BG, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(p, edge, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_set_style_outline_color(p, SDR_LCD_EDGE, 0);
    lv_obj_set_style_outline_width(p, 2, 0);
    lv_obj_set_style_outline_pad(p, 1, 0);
    lv_obj_set_style_radius(p, 6, 0);
    lv_obj_set_style_pad_all(p, 7, 0);
    lv_obj_set_style_pad_row(p, 2, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}


#define SEG_MAX 92

struct sdr_seg {
    lv_obj_t   *bar;
    lv_obj_t   *vlabel;
    int         max;
    int         value;
    int         cw;
    lv_color_t  color;
    sdr_seg_cb_t cb;
    sdr_seg_cb_t rel_cb;
    void       *ud;
};

static void seg_render(sdr_seg_t *s)
{
    if (s->cw <= 0) {
        lv_point_t sz;
        lv_txt_get_size(&sz, "||||||||||", sdr_font_mono(), 0, 0, LV_COORD_MAX, 0);
        s->cw = sz.x / 10;
        if (s->cw < 4) s->cw = 9;
    }
    int w = lv_obj_get_content_width(s->bar);
    int segs = (w > s->cw * 8) ? (w / s->cw - 2) : 24;
    if (segs < 8) segs = 8;
    if (segs > SEG_MAX) segs = SEG_MAX;

    int fill = s->max > 0 ? (s->value * segs + s->max / 2) / s->max : 0;
    if (fill < 0) fill = 0;
    if (fill > segs) fill = segs;
    int knob = fill >= segs ? segs - 1 : fill;

    char buf[SEG_MAX + 4];
    int n = 0;
    buf[n++] = '[';
    for (int i = 0; i < segs; i++) {
        char c = (i < fill) ? '|' : '.';
        if (i == knob) c = '#';
        buf[n++] = c;
    }
    buf[n++] = ']';
    buf[n] = 0;
    lv_label_set_text(s->bar, buf);
    lv_obj_set_style_text_color(s->bar, s->color, 0);
}

static void seg_press_cb(lv_event_t *e)
{
    sdr_seg_t *s = (sdr_seg_t *)lv_event_get_user_data(e);
    lv_indev_t *iv = lv_indev_get_act();
    if (!s || !iv) return;
    lv_point_t pt;
    lv_indev_get_point(iv, &pt);
    lv_area_t a;
    lv_obj_get_coords(s->bar, &a);
    int w = a.x2 - a.x1;
    if (w <= 0) return;
    int rel = pt.x - a.x1;
    if (rel < 0) rel = 0;
    if (rel > w) rel = w;
    int v = (rel * s->max + w / 2) / w;
    if (v != s->value) {
        s->value = v;
        seg_render(s);
        if (s->cb) s->cb(s->ud, v);
    }
}

static void seg_release_cb(lv_event_t *e)
{
    sdr_seg_t *s = (sdr_seg_t *)lv_event_get_user_data(e);
    if (s && s->rel_cb) s->rel_cb(s->ud, s->value);
}

static void seg_size_cb(lv_event_t *e)
{
    sdr_seg_t *s = (sdr_seg_t *)lv_event_get_user_data(e);
    if (s) seg_render(s);
}

static void seg_free_cb(lv_event_t *e)
{
    sdr_seg_t *s = (sdr_seg_t *)lv_event_get_user_data(e);
    free(s);
}

sdr_seg_t *sdr_seg_slider(lv_obj_t *parent, lv_color_t color, int max, int value,
                          sdr_seg_cb_t cb, void *ud, lv_obj_t **out_label)
{
    sdr_seg_t *s = (sdr_seg_t *)malloc(sizeof(sdr_seg_t));
    if (!s) return nullptr;
    s->max = max > 0 ? max : 1;
    s->value = value;
    s->cw = 0;
    s->color = color;
    s->cb = cb;
    s->rel_cb = nullptr;
    s->ud = ud;

    lv_obj_t *p = sdr_panel(parent);
    lv_obj_set_style_pad_all(p, 5, 0);
    lv_obj_set_style_pad_row(p, 1, 0);

    lv_obj_t *l = sdr_label(p, sdr_font_mono(), SDR_TEXT);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_text(l, "");

    lv_obj_t *bar = lv_label_create(p);
    lv_obj_set_style_text_font(bar, sdr_font_mono(), 0);
    lv_obj_set_width(bar, lv_pct(100));
    lv_label_set_long_mode(bar, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_pad_ver(bar, 7, 0);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(bar, seg_press_cb, LV_EVENT_PRESSED, s);
    lv_obj_add_event_cb(bar, seg_press_cb, LV_EVENT_PRESSING, s);
    lv_obj_add_event_cb(bar, seg_release_cb, LV_EVENT_RELEASED, s);
    lv_obj_add_event_cb(bar, seg_size_cb, LV_EVENT_SIZE_CHANGED, s);
    lv_obj_add_event_cb(bar, seg_free_cb, LV_EVENT_DELETE, s);

    s->bar = bar;
    s->vlabel = l;
    seg_render(s);
    if (out_label) *out_label = l;
    return s;
}

void sdr_seg_set(sdr_seg_t *s, int value)
{
    if (!s) return;
    if (value == s->value) return;
    s->value = value;
    seg_render(s);
}

void sdr_seg_color(sdr_seg_t *s, lv_color_t color)
{
    if (!s) return;
    s->color = color;
    lv_obj_set_style_text_color(s->bar, color, 0);
}

void sdr_seg_on_release(sdr_seg_t *s, sdr_seg_cb_t cb)
{
    if (s) s->rel_cb = cb;
}

int sdr_seg_value(sdr_seg_t *s) { return s ? s->value : 0; }

int sdr_bar_width(lv_obj_t *label, int reserved)
{
    static int cw = 0;
    if (cw <= 0) {
        lv_point_t sz;
        lv_txt_get_size(&sz, "||||||||||", sdr_font_mono(), 0, 0, LV_COORD_MAX, 0);
        cw = sz.x / 10;
        if (cw < 4) cw = 9;
    }
    int w = label ? lv_obj_get_content_width(label) : 0;
    int n = (w > cw * 12) ? (w / cw) - reserved : 30;
    if (n < 8)  n = 8;
    if (n > 88) n = 88;
    return n;
}
