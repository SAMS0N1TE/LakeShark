
#pragma once

#include "lvgl.h"

#define SDR_BG        lv_color_hex(0x1A1A1A)
#define SDR_PANEL     lv_color_hex(0x2B2B2B)
#define SDR_PANEL_HI  lv_color_hex(0x363636)
#define SDR_SUNKEN    lv_color_hex(0x141414)
#define SDR_BORDER    lv_color_hex(0x484848)
#define SDR_BORDER_HI lv_color_hex(0x6E6E6E)
#define SDR_BEVEL_LO  lv_color_hex(0x0E0E0E)
#define SDR_LABEL     lv_color_hex(0x9A9A9A)
#define SDR_TEXT      lv_color_hex(0xD4D4D4)
#define SDR_BRIGHT    lv_color_hex(0xFFFFFF)
#define SDR_DIM       lv_color_hex(0x707070)
#define SDR_GREEN     lv_color_hex(0x68B25E)
#define SDR_AMBER     lv_color_hex(0xC9923C)
#define SDR_RED       lv_color_hex(0xC85A52)
#define SDR_CYAN      lv_color_hex(0xCFCAB6)
#define SDR_GOLD      lv_color_hex(0xC9A24A)
#define SDR_MAGENTA   lv_color_hex(0xB07AA0)
#define SDR_BTN       lv_color_hex(0x3A3A3A)
#define SDR_BTN_HI    lv_color_hex(0x1E1E1E)

#define SDR_PAS_BLUE   lv_color_hex(0x8CB6D9)
#define SDR_PAS_GREEN  lv_color_hex(0x8FC9AE)
#define SDR_PAS_GOLD   lv_color_hex(0xE0C27A)
#define SDR_PAS_ROSE   lv_color_hex(0xE39B96)
#define SDR_PAS_LAV    lv_color_hex(0xB3A7E0)
#define SDR_PAS_CYAN   lv_color_hex(0x9AD6D6)
#define SDR_PAS_AMBER  lv_color_hex(0xE6B873)
#define SDR_PAS_MINT   lv_color_hex(0x9FE0C4)
#define SDR_LCD_BG     lv_color_hex(0x070A09)
#define SDR_LCD_EDGE   lv_color_hex(0x101816)

#ifdef __cplusplus
extern "C" {
#endif

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
