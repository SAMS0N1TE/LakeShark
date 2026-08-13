
#pragma once

#include "lvgl.h"

/*LS-605*/
#define SDR_BG        lv_color_hex(0x0A0C0D)
#define SDR_PANEL     lv_color_hex(0x171A1C)
#define SDR_PANEL_HI  lv_color_hex(0x21262A)
#define SDR_SUNKEN    lv_color_hex(0x0D1012)
#define SDR_BORDER    lv_color_hex(0x2C3237)
#define SDR_BORDER_HI lv_color_hex(0x4A5259)
#define SDR_BEVEL_LO  lv_color_hex(0x050708)
#define SDR_LABEL     lv_color_hex(0x79838B)
#define SDR_TEXT      lv_color_hex(0xC8D0D6)
#define SDR_BRIGHT    lv_color_hex(0xF4F7F9)
#define SDR_DIM       lv_color_hex(0x515A61)
#define SDR_GREEN     lv_color_hex(0x5FCB86)
#define SDR_AMBER     lv_color_hex(0xE3A83F)
#define SDR_RED       lv_color_hex(0xE45B50)
#define SDR_CYAN      lv_color_hex(0x84D2CE)
#define SDR_GOLD      lv_color_hex(0xF2A233)
#define SDR_MAGENTA   lv_color_hex(0xB782B0)
#define SDR_BTN       lv_color_hex(0x1D2226)
#define SDR_BTN_HI    lv_color_hex(0x0F1315)

#define SDR_PAS_BLUE   lv_color_hex(0x86B8E2)
#define SDR_PAS_GREEN  lv_color_hex(0x83CFAB)
#define SDR_PAS_GOLD   lv_color_hex(0xEFC272)
#define SDR_PAS_ROSE   lv_color_hex(0xE9968F)
#define SDR_PAS_LAV    lv_color_hex(0xB0A4E4)
#define SDR_PAS_CYAN   lv_color_hex(0x92DADA)
#define SDR_PAS_AMBER  lv_color_hex(0xF0B767)
#define SDR_PAS_MINT   lv_color_hex(0x9BE4C4)
#define SDR_LCD_BG     lv_color_hex(0x050809)
#define SDR_LCD_EDGE   lv_color_hex(0x0D1517)

/*LS-605*/
#define SDR_CHASSIS    lv_color_hex(0x06080A)
#define SDR_RULE       lv_color_hex(0x23282C)
#define SDR_ACCENT     SDR_GOLD
#define SDR_ACCENT_DIM lv_color_hex(0x8A5C18)
#define SDR_ACCENT_BG  lv_color_hex(0x241905)
#define SDR_OK         SDR_PAS_GREEN
#define SDR_WARN       SDR_PAS_AMBER
#define SDR_ERR        SDR_PAS_ROSE

#define SDR_STATUS_H   30
#define SDR_RAIL_H     76

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

/*LS-605*/
lv_obj_t *sdr_row(lv_obj_t *parent, lv_flex_align_t justify);

/*LS-605*/
lv_obj_t *sdr_rule(lv_obj_t *parent);

/*LS-605*/
lv_obj_t *sdr_micro(lv_obj_t *parent, const char *txt);

/*LS-605*/
lv_obj_t *sdr_value(lv_obj_t *parent, const lv_font_t *font, lv_color_t color);

/*LS-605*/
lv_obj_t *sdr_chip(lv_obj_t *parent, const char *txt, lv_color_t color);
void      sdr_chip_set(lv_obj_t *chip, const char *txt, lv_color_t color);
void      sdr_chip_live(lv_obj_t *chip, bool live);

/*LS-605*/
lv_obj_t *sdr_tile(lv_obj_t *parent, const char *icon_key, const char *title,
                   const char *sub, lv_event_cb_t cb, void *ud);
void      sdr_tile_accent(lv_obj_t *tile, lv_color_t color);

/*LS-605*/
void sdr_text_if_changed(lv_obj_t *label, const char *txt);
void sdr_color_if_changed(lv_obj_t *obj, lv_color_t color);

/*LS-605*/
void sdr_ascii_bar(char *dst, int cap, int pct, int width);

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
