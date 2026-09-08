#include "sdr_ui.h"
#include "shell/ls_icons.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

LV_FONT_DECLARE(lv_font_lsmono_16);
LV_FONT_DECLARE(lv_font_lsmono_14);

/*  DejaVu Sans Mono, converted at the two sizes the readouts are laid out
    for. The readouts are columns of figures and they have to hold still as
    digits change, which needs a fixed pitch; LVGL's only built-in monospace
    is unscii, whose fixed 8x16 cell renders far too large for these panels.
    DejaVu is under the Bitstream Vera licence, so it can ship. */
/* LS-774 follow-up: generated mono fonts cover ASCII32..126 only. LVGL
 * keyboards use private-use backspace/enter/arrow/OK glyphs, so selecting
 * mono made those keys blank. Retain fixed-pitch text and supply the same-
 * size built-in symbol font for missing glyphs, including other app labels.
 * Copy the descriptor; generated const font data may live in read-only flash. */
const lv_font_t *sdr_font_mono(void)
{
    static const lv_font_t font=[] { auto f=lv_font_lsmono_16; f.fallback=&lv_font_montserrat_16; return f; }();
    return &font;
}
const lv_font_t *sdr_font_mono_sm(void)
{
    static const lv_font_t font=[] { auto f=lv_font_lsmono_14; f.fallback=&lv_font_montserrat_14; return f; }();
    return &font;
}
const lv_font_t *sdr_font_ui(void)      { return &lv_font_montserrat_14; }

/*LS-606*/
typedef struct {
    const char *name;
} sdr_pal_t;

static const sdr_pal_t THEMES[SDR_THEME_COUNT] = {
    { "RED" },
    { "WHITE" },
    { "BLUE" },
};

#define SDR_THEME_MAX_CB 8

static sdr_theme_t    s_theme = SDR_THEME_RED;
static lv_style_t     s_tab_sel;
static lv_style_t     s_press;
static lv_style_t     s_fill;
static bool           s_styles_ready;
static sdr_theme_cb_t s_cb[SDR_THEME_MAX_CB];
static void          *s_cb_ud[SDR_THEME_MAX_CB];

lv_color_t sdr_accent(void)     { return lv_color_hex(ls_ui_palette_theme_hex(s_theme, LS_UI_THEME_ACCENT)); }
lv_color_t sdr_accent_dim(void) { return lv_color_hex(ls_ui_palette_theme_hex(s_theme, LS_UI_THEME_DIM)); }
lv_color_t sdr_accent_bg(void)  { return lv_color_hex(ls_ui_palette_theme_hex(s_theme, LS_UI_THEME_BACKGROUND)); }

sdr_theme_t sdr_theme_get(void) { return s_theme; }

const char *sdr_theme_name(sdr_theme_t t)
{
    return (t >= 0 && t < SDR_THEME_COUNT) ? THEMES[t].name : "?";
}

/*LS-606*/
static void style_sync(void)
{
    lv_style_set_text_color(&s_tab_sel, sdr_accent());
    lv_style_set_bg_color(&s_tab_sel, sdr_accent_bg());
    lv_style_set_border_color(&s_tab_sel, sdr_accent());

    lv_style_set_bg_color(&s_press, sdr_accent_bg());
    lv_style_set_border_color(&s_press, sdr_accent());

    lv_style_set_bg_color(&s_fill, sdr_accent());
}

/*LS-606*/
void sdr_theme_init(void)
{
    if (s_styles_ready) return;
    s_styles_ready = true;

    lv_style_init(&s_tab_sel);
    lv_style_set_bg_opa(&s_tab_sel, LV_OPA_COVER);
    lv_style_set_border_width(&s_tab_sel, 2);
    lv_style_set_border_side(&s_tab_sel, LV_BORDER_SIDE_BOTTOM);

    lv_style_init(&s_press);
    lv_style_set_bg_opa(&s_press, LV_OPA_COVER);

    lv_style_init(&s_fill);
    lv_style_set_bg_opa(&s_fill, LV_OPA_COVER);

    style_sync();
}

/*LS-606*/
void sdr_theme_set(sdr_theme_t t)
{
    if (t < 0 || t >= SDR_THEME_COUNT || t == s_theme) return;
    s_theme = t;

    sdr_theme_init();
    style_sync();
    lv_obj_report_style_change(&s_tab_sel);
    lv_obj_report_style_change(&s_press);
    lv_obj_report_style_change(&s_fill);

    for (int i = 0; i < SDR_THEME_MAX_CB; i++)
        if (s_cb[i]) s_cb[i](s_cb_ud[i]);
}

/*LS-606*/
int sdr_theme_on_change(sdr_theme_cb_t cb, void *ud)
{
    if (!cb) return -1;
    for (int i = 0; i < SDR_THEME_MAX_CB; i++) {
        if (s_cb[i]) continue;
        s_cb[i] = cb;
        s_cb_ud[i] = ud;
        return i;
    }
    return -1;
}

/*LS-606*/
void sdr_theme_off_change(int id)
{
    if (id < 0 || id >= SDR_THEME_MAX_CB) return;
    s_cb[id] = nullptr;
    s_cb_ud[id] = nullptr;
}

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

        /*LS-606*/
        sdr_theme_init();
        lv_obj_add_style(btns, &s_tab_sel, LV_PART_ITEMS | LV_STATE_CHECKED);

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
    /*LS-606*/
    sdr_theme_init();
    lv_obj_add_style(tick, &s_fill, 0);
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
    lv_obj_set_style_border_color(b, SDR_BORDER, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    /*LS-606*/
    sdr_theme_init();
    lv_obj_add_style(b, &s_press, LV_STATE_PRESSED);
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
    lv_obj_set_width(ctl, lv_pct(65));
    lv_obj_set_style_bg_opa(ctl, LV_OPA_0, 0);
    lv_obj_set_style_border_width(ctl, 0, 0);
    lv_obj_set_style_pad_all(ctl, 0, 0);
    lv_obj_set_style_pad_column(ctl, 6, 0);
    lv_obj_set_flex_flow(ctl, LV_FLEX_FLOW_ROW_WRAP);
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
    lv_obj_set_style_border_color(t, SDR_RULE, 0);
    lv_obj_set_style_border_width(t, 1, 0);
    /*LS-606*/
    sdr_theme_init();
    lv_obj_add_style(t, &s_press, LV_STATE_PRESSED);
    lv_obj_set_style_radius(t, 2, 0);
    lv_obj_set_style_pad_all(t, 10, 0);
    lv_obj_set_style_pad_row(t, 4, 0);
    lv_obj_set_style_bg_color(t, sdr_accent(), LV_PART_INDICATOR);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *ic = lv_img_create(t);
    lv_img_set_src(ic, ls_icon_for(icon_key, 48));
    lv_obj_set_style_img_recolor(ic, sdr_accent(), 0);
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

/*LS-606*/
int sdr_bar_width(lv_obj_t *label, int reserved)
{
    if (!label) return 30;

    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    if (!font) font = sdr_font_mono();
    const lv_coord_t ls = lv_obj_get_style_text_letter_space(label, 0);

    lv_point_t sz;
    lv_txt_get_size(&sz, "||||||||||", font, ls, 0, LV_COORD_MAX, 0);
    int cw = sz.x / 10;
    if (cw < 4) cw = 9;

    int w = lv_obj_get_content_width(label);
    int n = (w > cw * 12) ? (w / cw) - reserved : 30;
    if (n < 8)  n = 8;
    if (n > 88) n = 88;
    return n;
}

/*LS-606*/
#define METER_TAG_MAX 6

typedef struct {
    char tag[METER_TAG_MAX];
    int  pct;
} sdr_meter_t;

static void meter_render(lv_obj_t *m)
{
    sdr_meter_t *s = (sdr_meter_t *)lv_obj_get_user_data(m);
    if (!s) return;

    const int tagn = (int)strlen(s->tag);
    char bar[96];
    sdr_ascii_bar(bar, sizeof(bar), s->pct, sdr_bar_width(m, tagn + 8));

    char line[128];
    snprintf(line, sizeof(line), "%s[%s]%4d%%", s->tag, bar, s->pct);
    sdr_text_if_changed(m, line);
}

static void meter_size_cb(lv_event_t *e) { meter_render(lv_event_get_target(e)); }
static void meter_free_cb(lv_event_t *e)
{
    free(lv_obj_get_user_data(lv_event_get_target(e)));
}

/*LS-606*/
lv_obj_t *sdr_meter(lv_obj_t *parent, const char *tag)
{
    sdr_meter_t *s = (sdr_meter_t *)calloc(1, sizeof(sdr_meter_t));
    if (!s) return nullptr;
    if (tag) { strncpy(s->tag, tag, METER_TAG_MAX - 1); s->tag[METER_TAG_MAX - 1] = 0; }

    lv_obj_t *m = lv_label_create(parent);
    lv_obj_set_width(m, lv_pct(100));
    lv_obj_set_style_text_font(m, sdr_font_mono(), 0);
    lv_obj_set_style_text_color(m, SDR_DIM, 0);
    lv_label_set_long_mode(m, LV_LABEL_LONG_CLIP);
    lv_obj_set_user_data(m, s);
    lv_obj_add_event_cb(m, meter_size_cb, LV_EVENT_SIZE_CHANGED, nullptr);
    lv_obj_add_event_cb(m, meter_free_cb, LV_EVENT_DELETE, nullptr);
    lv_label_set_text(m, "");
    return m;
}

/*LS-608*/
typedef struct {
    lv_obj_t *lbl;
    char      base[20];
    uint32_t  hold_ms;
    uint32_t  t0;
    int       shown;
    bool      fired;
} sdr_hold_t;

static void hold_reset(sdr_hold_t *h)
{
    h->t0 = 0;
    h->shown = -1;
    h->fired = false;
    sdr_text_if_changed(h->lbl, h->base);
    sdr_color_if_changed(h->lbl, SDR_TEXT);
}

static void hold_cb(lv_event_t *e)
{
    sdr_hold_t *h = (sdr_hold_t *)lv_event_get_user_data(e);
    lv_obj_t   *b = lv_event_get_target(e);
    if (!h) return;

    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        h->t0 = lv_tick_get();
        h->shown = -1;
        h->fired = false;
        break;

    case LV_EVENT_PRESSING: {
        if (!h->t0 || h->fired) break;
        const uint32_t el = lv_tick_elaps(h->t0);
        if (el >= h->hold_ms) {
            h->fired = true;
            sdr_text_if_changed(h->lbl, "DONE");
            sdr_color_if_changed(h->lbl, SDR_OK);
            lv_event_send(b, LV_EVENT_READY, NULL);
            break;
        }
        const int left = (int)((h->hold_ms - el + 999) / 1000);
        if (left != h->shown) {
            h->shown = left;
            char t[24];
            snprintf(t, sizeof(t), "HOLD %d", left);
            sdr_text_if_changed(h->lbl, t);
            sdr_color_if_changed(h->lbl, SDR_WARN);
        }
        break;
    }

    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        if (!h->fired) hold_reset(h);
        break;

    case LV_EVENT_DELETE:
        free(h);
        break;

    default: break;
    }
}

/*LS-608*/
lv_obj_t *sdr_hold_btn(lv_obj_t *parent, const char *txt, int hold_ms,
                       lv_event_cb_t cb, void *ud)
{
    sdr_hold_t *h = (sdr_hold_t *)calloc(1, sizeof(sdr_hold_t));
    if (!h) return nullptr;
    h->hold_ms = hold_ms > 0 ? (uint32_t)hold_ms : 2000;
    h->shown = -1;
    strncpy(h->base, txt ? txt : "HOLD", sizeof(h->base) - 1);

    lv_obj_t *b = sdr_btn(parent, h->base, nullptr, nullptr, &h->lbl);
    lv_obj_set_style_border_color(b, SDR_WARN, 0);

    lv_obj_add_event_cb(b, hold_cb, LV_EVENT_PRESSED, h);
    lv_obj_add_event_cb(b, hold_cb, LV_EVENT_PRESSING, h);
    lv_obj_add_event_cb(b, hold_cb, LV_EVENT_RELEASED, h);
    lv_obj_add_event_cb(b, hold_cb, LV_EVENT_PRESS_LOST, h);
    lv_obj_add_event_cb(b, hold_cb, LV_EVENT_DELETE, h);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_READY, ud);
    return b;
}

/*LS-606*/
void sdr_meter_set(lv_obj_t *meter, int pct, lv_color_t color)
{
    if (!meter) return;
    sdr_meter_t *s = (sdr_meter_t *)lv_obj_get_user_data(meter);
    if (!s) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    s->pct = pct;
    meter_render(meter);
    sdr_color_if_changed(meter, color);
}
