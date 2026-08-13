#include "sdr_ui.h"
#include "shell/ls_icons.h"

#include <cstdio>
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

/*LS-605*/
void sdr_style_tabview(lv_obj_t *tv)
{
    lv_obj_set_style_bg_color(tv, SDR_BG, 0);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, 0);

    lv_obj_t *btns = lv_tabview_get_tab_btns(tv);
    if (btns) {
        lv_obj_set_style_bg_color(btns, SDR_CHASSIS, 0);
        lv_obj_set_style_bg_opa(btns, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btns, SDR_RULE, 0);
        lv_obj_set_style_border_width(btns, 1, 0);
        lv_obj_set_style_border_side(btns, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_pad_all(btns, 0, 0);

        lv_obj_set_style_text_font(btns, sdr_font_mono_sm(), LV_PART_ITEMS);
        lv_obj_set_style_text_color(btns, SDR_DIM, LV_PART_ITEMS);
        lv_obj_set_style_text_letter_space(btns, 2, LV_PART_ITEMS);
        lv_obj_set_style_bg_opa(btns, LV_OPA_0, LV_PART_ITEMS);
        lv_obj_set_style_border_width(btns, 0, LV_PART_ITEMS);
        lv_obj_set_style_radius(btns, 0, LV_PART_ITEMS);

        lv_obj_set_style_text_color(btns, SDR_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(btns, SDR_ACCENT_BG, LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(btns, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_border_color(btns, SDR_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_border_width(btns, 2, LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_border_side(btns, LV_BORDER_SIDE_BOTTOM,
                                     LV_PART_ITEMS | LV_STATE_CHECKED);

        lv_obj_set_style_text_color(btns, SDR_TEXT, LV_PART_ITEMS | LV_STATE_PRESSED);
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

/*LS-605*/
lv_obj_t *sdr_panel(lv_obj_t *parent)
{

    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_width(p, lv_pct(100));
    lv_obj_set_height(p, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(p, SDR_SUNKEN, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(p, SDR_RULE, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_outline_width(p, 0, 0);
    lv_obj_set_style_radius(p, 2, 0);
    lv_obj_set_style_pad_all(p, 8, 0);
    lv_obj_set_style_pad_row(p, 4, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

/*LS-605*/
lv_obj_t *sdr_section(lv_obj_t *parent, const char *title)
{
    lv_obj_t *row = sdr_row(parent, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_top(row, 8, 0);
    lv_obj_set_style_pad_bottom(row, 3, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_style_border_color(row, SDR_RULE, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);

    lv_obj_t *tick = lv_obj_create(row);
    lv_obj_set_size(tick, 3, 14);
    lv_obj_set_style_bg_color(tick, SDR_ACCENT, 0);
    lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tick, 0, 0);
    lv_obj_set_style_radius(tick, 0, 0);
    lv_obj_set_style_pad_all(tick, 0, 0);
    lv_obj_clear_flag(tick, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = lv_label_create(row);
    lv_obj_set_flex_grow(l, 1);
    lv_obj_set_style_text_font(l, sdr_font_mono_sm(), 0);
    lv_obj_set_style_text_color(l, SDR_LABEL, 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_label_set_text(l, title);
    return row;
}

/*LS-605*/
lv_obj_t *sdr_btn(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, void *ud,
                  lv_obj_t **out_lbl)
{

    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_height(b, 50);
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    lv_obj_set_style_min_width(b, 56, 0);
    lv_obj_set_style_bg_color(b, SDR_BTN, 0);
    lv_obj_set_style_bg_color(b, SDR_ACCENT_BG, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, SDR_BORDER, 0);
    lv_obj_set_style_border_color(b, SDR_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 2, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 16, 0);
    lv_obj_set_style_pad_ver(b, 6, 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, sdr_font_mono_sm(), 0);
    lv_obj_set_style_text_color(l, SDR_TEXT, 0);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    if (out_lbl) *out_lbl = l;
    return b;
}

/*LS-605*/
void sdr_setting_row(lv_obj_t *parent, const char *name, sdr_setrow_t *out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, SDR_PANEL, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(row, SDR_RULE, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_radius(row, 2, 0);
    lv_obj_set_style_pad_hor(row, 10, 0);
    lv_obj_set_style_pad_ver(row, 6, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name_l = sdr_label(row, sdr_font_mono_sm(), SDR_LABEL);
    lv_obj_set_style_text_letter_space(name_l, 1, 0);
    lv_label_set_text(name_l, name);
    lv_obj_set_flex_grow(name_l, 1);

    lv_obj_t *val = sdr_label(row, sdr_font_mono(), SDR_BRIGHT);
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


/*LS-605*/
lv_obj_t *sdr_lcd_panel(lv_obj_t *parent, lv_color_t edge)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_width(p, lv_pct(100));
    lv_obj_set_height(p, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(p, SDR_LCD_BG, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(p, edge, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_outline_color(p, SDR_LCD_EDGE, 0);
    lv_obj_set_style_outline_width(p, 3, 0);
    lv_obj_set_style_outline_pad(p, 0, 0);
    lv_obj_set_style_radius(p, 3, 0);
    lv_obj_set_style_pad_all(p, 9, 0);
    lv_obj_set_style_pad_row(p, 3, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

/*LS-605*/
lv_obj_t *sdr_row(lv_obj_t *parent, lv_flex_align_t justify)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_width(r, lv_pct(100));
    lv_obj_set_height(r, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(r, LV_OPA_0, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_radius(r, 0, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_set_style_pad_column(r, 6, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, justify, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

/*LS-605*/
lv_obj_t *sdr_rule(lv_obj_t *parent)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_width(r, lv_pct(100));
    lv_obj_set_height(r, 1);
    lv_obj_set_style_bg_color(r, SDR_RULE, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_radius(r, 0, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

/*LS-605*/
lv_obj_t *sdr_micro(lv_obj_t *parent, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, sdr_font_mono_sm(), 0);
    lv_obj_set_style_text_color(l, SDR_LABEL, 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_label_set_text(l, txt ? txt : "");
    return l;
}

/*LS-605*/
lv_obj_t *sdr_value(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font ? font : sdr_font_mono(), 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_label_set_text(l, "");
    return l;
}

/*LS-605*/
lv_obj_t *sdr_chip(lv_obj_t *parent, const char *txt, lv_color_t color)
{
    lv_obj_t *c = lv_label_create(parent);
    lv_obj_set_style_text_font(c, sdr_font_mono_sm(), 0);
    lv_obj_set_style_text_color(c, color, 0);
    lv_obj_set_style_text_letter_space(c, 1, 0);
    lv_obj_set_style_bg_color(c, SDR_PANEL, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, SDR_RULE, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 2, 0);
    lv_obj_set_style_pad_hor(c, 5, 0);
    lv_obj_set_style_pad_ver(c, 2, 0);
    lv_label_set_long_mode(c, LV_LABEL_LONG_CLIP);
    lv_label_set_text(c, txt ? txt : "");
    return c;
}

/*LS-605*/
void sdr_chip_set(lv_obj_t *chip, const char *txt, lv_color_t color)
{
    if (!chip) return;
    sdr_text_if_changed(chip, txt);
    sdr_color_if_changed(chip, color);
}

/*LS-605*/
void sdr_chip_live(lv_obj_t *chip, bool live)
{
    if (!chip) return;
    lv_color_t edge = live ? lv_obj_get_style_text_color(chip, 0) : SDR_RULE;
    if (lv_obj_get_style_border_color(chip, 0).full == edge.full) return;
    lv_obj_set_style_border_color(chip, edge, 0);
    lv_obj_set_style_bg_color(chip, live ? SDR_PANEL_HI : SDR_PANEL, 0);
}

/*LS-605*/
static void tile_press_cb(lv_event_t *e)
{
    lv_obj_t *t = lv_event_get_target(e);
    lv_obj_t *ic = lv_obj_get_child(t, 0);
    if (!ic) return;
    bool down = (lv_event_get_code(e) == LV_EVENT_PRESSED);
    lv_obj_set_style_img_recolor(ic, down ? SDR_BRIGHT
                                          : lv_obj_get_style_bg_color(t, LV_PART_INDICATOR), 0);
}

/*LS-605*/
lv_obj_t *sdr_tile(lv_obj_t *parent, const char *icon_key, const char *title,
                   const char *sub, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *t = lv_obj_create(parent);
    lv_obj_set_height(t, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(t, SDR_PANEL, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(t, SDR_ACCENT_BG, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(t, SDR_RULE, 0);
    lv_obj_set_style_border_color(t, SDR_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_radius(t, 2, 0);
    lv_obj_set_style_pad_all(t, 10, 0);
    lv_obj_set_style_pad_row(t, 4, 0);
    lv_obj_set_style_bg_color(t, SDR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *ic = lv_img_create(t);
    lv_img_set_src(ic, ls_icon_for(icon_key, 48));
    lv_obj_set_style_img_recolor(ic, SDR_ACCENT, 0);
    lv_obj_set_style_img_recolor_opa(ic, LV_OPA_COVER, 0);

    lv_obj_t *l = lv_label_create(t);
    lv_obj_set_style_text_font(l, sdr_font_mono(), 0);
    lv_obj_set_style_text_color(l, SDR_BRIGHT, 0);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_label_set_text(l, title ? title : "");

    if (sub && *sub) {
        lv_obj_t *s = sdr_micro(t, sub);
        lv_obj_set_style_text_color(s, SDR_DIM, 0);
    }

    lv_obj_add_event_cb(t, tile_press_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(t, tile_press_cb, LV_EVENT_RELEASED, nullptr);
    if (cb) lv_obj_add_event_cb(t, cb, LV_EVENT_CLICKED, ud);
    return t;
}

/*LS-605*/
void sdr_tile_accent(lv_obj_t *tile, lv_color_t color)
{
    if (!tile) return;
    lv_obj_set_style_bg_color(tile, color, LV_PART_INDICATOR);
    lv_obj_t *ic = lv_obj_get_child(tile, 0);
    if (ic) lv_obj_set_style_img_recolor(ic, color, 0);
}

/*LS-605*/
void sdr_text_if_changed(lv_obj_t *label, const char *txt)
{
    if (!label || !txt) return;
    const char *cur = lv_label_get_text(label);
    if (cur && strcmp(cur, txt) == 0) return;
    lv_label_set_text(label, txt);
}

/*LS-605*/
void sdr_color_if_changed(lv_obj_t *obj, lv_color_t color)
{
    if (!obj) return;
    if (lv_obj_get_style_text_color(obj, 0).full == color.full) return;
    lv_obj_set_style_text_color(obj, color, 0);
}

/*LS-605*/
void sdr_ascii_bar(char *dst, int cap, int pct, int width)
{
    if (!dst || cap < 3) return;
    if (width > cap - 1) width = cap - 1;
    if (width < 1) width = 1;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    int fill = (pct * width + 50) / 100;
    int i = 0;
    for (; i < fill; i++) dst[i] = '#';
    for (; i < width; i++) dst[i] = '-';
    dst[i] = 0;
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
