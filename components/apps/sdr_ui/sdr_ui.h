
#pragma once

#include "lvgl.h"
#include "ui/ls_ui_palette.h"

/* LS-460: compatibility names keep older screens source-compatible while all
 * shared primitives draw from the role palette. New app code uses LS_UI_*.
 * The hue aliases intentionally collapse onto meaning instead of preserving
 * the 33-colour vocabulary this kit replaces. */
#define SDR_ROLE_COLOR(role) lv_color_hex(ls_ui_palette_hex((role)))
#define SDR_BG        SDR_ROLE_COLOR(LS_UI_COLOR_BACKGROUND)
#define SDR_PANEL     SDR_ROLE_COLOR(LS_UI_COLOR_PANEL)
#define SDR_PANEL_HI  SDR_ROLE_COLOR(LS_UI_COLOR_PANEL)
#define SDR_SUNKEN    SDR_ROLE_COLOR(LS_UI_COLOR_BACKGROUND)
#define SDR_BORDER    SDR_ROLE_COLOR(LS_UI_COLOR_PANEL_BORDER)
#define SDR_BORDER_HI SDR_ROLE_COLOR(LS_UI_COLOR_PANEL_BORDER)
#define SDR_BEVEL_LO  SDR_ROLE_COLOR(LS_UI_COLOR_BACKGROUND)
#define SDR_LABEL     SDR_ROLE_COLOR(LS_UI_COLOR_DIM_TEXT)
#define SDR_TEXT      SDR_ROLE_COLOR(LS_UI_COLOR_TEXT)
#define SDR_BRIGHT    SDR_ROLE_COLOR(LS_UI_COLOR_TEXT)
#define SDR_DIM       SDR_ROLE_COLOR(LS_UI_COLOR_DIM_TEXT)
#define SDR_GREEN     SDR_ROLE_COLOR(LS_UI_COLOR_ACCENT)
#define SDR_AMBER     SDR_ROLE_COLOR(LS_UI_COLOR_WARN)
#define SDR_RED       SDR_ROLE_COLOR(LS_UI_COLOR_ALARM)
#define SDR_CYAN      SDR_ROLE_COLOR(LS_UI_COLOR_ACCENT)
#define SDR_GOLD      SDR_ROLE_COLOR(LS_UI_COLOR_WARN)
#define SDR_MAGENTA   SDR_ROLE_COLOR(LS_UI_COLOR_ACCENT)
#define SDR_BTN       SDR_ROLE_COLOR(LS_UI_COLOR_PANEL)
#define SDR_BTN_HI    SDR_ROLE_COLOR(LS_UI_COLOR_BACKGROUND)

#define SDR_PAS_BLUE  SDR_ROLE_COLOR(LS_UI_COLOR_ACCENT)
#define SDR_PAS_GREEN SDR_ROLE_COLOR(LS_UI_COLOR_ACCENT)
#define SDR_PAS_GOLD  SDR_ROLE_COLOR(LS_UI_COLOR_WARN)
#define SDR_PAS_ROSE  SDR_ROLE_COLOR(LS_UI_COLOR_ALARM)
#define SDR_PAS_LAV   SDR_ROLE_COLOR(LS_UI_COLOR_ACCENT)
#define SDR_PAS_CYAN  SDR_ROLE_COLOR(LS_UI_COLOR_ACCENT)
#define SDR_PAS_AMBER SDR_ROLE_COLOR(LS_UI_COLOR_WARN)
#define SDR_PAS_MINT  SDR_ROLE_COLOR(LS_UI_COLOR_ACCENT)
#define SDR_LCD_BG    SDR_ROLE_COLOR(LS_UI_COLOR_BACKGROUND)
#define SDR_LCD_EDGE  SDR_ROLE_COLOR(LS_UI_COLOR_PANEL_BORDER)

#define SDR_CHASSIS   SDR_ROLE_COLOR(LS_UI_COLOR_BACKGROUND)
#define SDR_RULE      SDR_ROLE_COLOR(LS_UI_COLOR_PANEL_BORDER)
#define SDR_OK         SDR_PAS_GREEN
#define SDR_WARN       SDR_PAS_AMBER
#define SDR_ERR        SDR_PAS_ROSE

/*LS-606*/
#define SDR_OFF        SDR_ROLE_COLOR(LS_UI_COLOR_DIM_TEXT)
#define SDR_IDLE       SDR_ROLE_COLOR(LS_UI_COLOR_DIM_TEXT)

#define SDR_STATUS_H   30
#define SDR_RAIL_H     76

#ifdef __cplusplus
extern "C" {
#endif

/*LS-606*/
typedef enum {
    SDR_THEME_RED = 0,
    SDR_THEME_WHITE,
    SDR_THEME_BLUE,
    SDR_THEME_COUNT
} sdr_theme_t;

/*LS-606*/
void        sdr_theme_init(void);
void        sdr_theme_set(sdr_theme_t t);
sdr_theme_t sdr_theme_get(void);
const char *sdr_theme_name(sdr_theme_t t);

/*LS-606*/
lv_color_t sdr_accent(void);
lv_color_t sdr_accent_dim(void);
lv_color_t sdr_accent_bg(void);

/*LS-606*/
typedef void (*sdr_theme_cb_t)(void *ud);
int  sdr_theme_on_change(sdr_theme_cb_t cb, void *ud);
void sdr_theme_off_change(int id);

const lv_font_t *sdr_font_mono(void);
const lv_font_t *sdr_font_mono_sm(void);
const lv_font_t *sdr_font_ui(void);

void sdr_style_screen(lv_obj_t *scr);

void sdr_style_tabview(lv_obj_t *tv);

lv_obj_t *sdr_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color);

lv_obj_t *sdr_panel(lv_obj_t *parent);

lv_obj_t *sdr_section(lv_obj_t *parent, const char *title);

lv_obj_t *sdr_btn(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, void *ud,
                  lv_obj_t **out_lbl);

typedef struct {
    lv_obj_t *row;
    lv_obj_t *name;
    lv_obj_t *value;
    lv_obj_t *controls;
} sdr_setrow_t;

void sdr_setting_row(lv_obj_t *parent, const char *name, sdr_setrow_t *out);

lv_obj_t *sdr_lcd_panel(lv_obj_t *parent, lv_color_t edge);

/*LS-605*/
lv_obj_t *sdr_row(lv_obj_t *parent, lv_flex_align_t justify);

/*LS-605*/
lv_obj_t *sdr_rule(lv_obj_t *parent);

/*LS-605*/
lv_obj_t *sdr_micro(lv_obj_t *parent, const char *txt);

/*LS-605*/
lv_obj_t *sdr_value(lv_obj_t *parent, const lv_font_t *font, lv_color_t color);

/*LS-605*/
lv_obj_t *sdr_tile(lv_obj_t *parent, const char *icon_key, const char *title,
                   const char *sub, lv_event_cb_t cb, void *ud);
void      sdr_tile_accent(lv_obj_t *tile, lv_color_t color);

/*LS-605*/
void sdr_text_if_changed(lv_obj_t *label, const char *txt);
void sdr_color_if_changed(lv_obj_t *obj, lv_color_t color);

/*LS-605*/
void sdr_ascii_bar(char *dst, int cap, int pct, int width);

/*LS-606*/
lv_obj_t *sdr_meter(lv_obj_t *parent, const char *tag);
void      sdr_meter_set(lv_obj_t *meter, int pct, lv_color_t color);

/*LS-608*/
lv_obj_t *sdr_hold_btn(lv_obj_t *parent, const char *txt, int hold_ms,
                       lv_event_cb_t cb, void *ud);

typedef struct sdr_seg sdr_seg_t;
typedef void (*sdr_seg_cb_t)(void *ud, int value);

sdr_seg_t *sdr_seg_slider(lv_obj_t *parent, lv_color_t color, int max, int value,
                          sdr_seg_cb_t cb, void *ud, lv_obj_t **out_label);
void sdr_seg_set(sdr_seg_t *s, int value);
void sdr_seg_color(sdr_seg_t *s, lv_color_t color);
void sdr_seg_on_release(sdr_seg_t *s, sdr_seg_cb_t cb);
int  sdr_seg_value(sdr_seg_t *s);

/* Segment count that fills `label`'s width minus `reserved` chars (tag/percent),
 * for full-width ASCII text meters drawn into a mono label. */
int  sdr_bar_width(lv_obj_t *label, int reserved);

#ifdef __cplusplus
}
#endif
