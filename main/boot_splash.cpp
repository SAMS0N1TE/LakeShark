#include "boot_splash.h"

#include "lvgl.h"
#include "bsp/display.h"

#define BOOT_BG     lv_color_hex(0x1A1A1A)
#define BOOT_NAVY   lv_color_hex(0x101010)
#define BOOT_CYAN   lv_color_hex(0xC9A24A)
#define BOOT_CYAN_D lv_color_hex(0x555555)
#define BOOT_GREEN  lv_color_hex(0x68B25E)
#define BOOT_TEXT   lv_color_hex(0xF0F0F0)
#define BOOT_DIM    lv_color_hex(0x808080)

static lv_obj_t *s_splash = nullptr;

static void ring(lv_obj_t *parent, int d, lv_color_t c, lv_opa_t opa, int bw)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_size(r, d, d);
    lv_obj_center(r);
    lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_0, 0);
    lv_obj_set_style_border_color(r, c, 0);
    lv_obj_set_style_border_opa(r, opa, 0);
    lv_obj_set_style_border_width(r, bw, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color,
                          const char *txt, int letter_space)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_letter_space(l, letter_space, 0);
    lv_label_set_text(l, txt);
    return l;
}

void lakeshark_boot_splash_show(void)
{
    lv_obj_t *scr = lv_obj_create(nullptr);
    s_splash = scr;
    lv_obj_set_style_bg_color(scr, BOOT_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    ring(scr, 560, BOOT_CYAN_D, LV_OPA_30, 2);
    ring(scr, 430, BOOT_CYAN_D, LV_OPA_50, 2);
    ring(scr, 300, BOOT_CYAN,   LV_OPA_40, 3);

    lv_obj_t *title = mk_label(scr, &lv_font_montserrat_48, BOOT_TEXT, "LAKESHARK", 8);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -36);

    lv_obj_t *sub = mk_label(scr, &lv_font_montserrat_20, BOOT_CYAN, "S D R   T E R M I N A L", 2);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 8);

    lv_obj_t *caps = mk_label(scr, &lv_font_montserrat_16, BOOT_DIM, "ADS-B  /  P25  /  MESH", 2);
    lv_obj_align(caps, LV_ALIGN_CENTER, 0, 44);

    lv_obj_t *spin = lv_spinner_create(scr, 1000, 60);
    lv_obj_set_size(spin, 56, 56);
    lv_obj_align(spin, LV_ALIGN_CENTER, 0, 118);
    lv_obj_set_style_arc_color(spin, BOOT_CYAN_D, LV_PART_MAIN);
    lv_obj_set_style_arc_color(spin, BOOT_CYAN, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spin, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spin, 5, LV_PART_INDICATOR);

    lv_obj_t *boot = mk_label(scr, &lv_font_montserrat_14, BOOT_GREEN, "BOOTING RADIO BACKEND", 1);
    lv_obj_align(boot, LV_ALIGN_CENTER, 0, 168);

    lv_obj_t *foot = mk_label(scr, &lv_font_montserrat_14, BOOT_DIM,
                              "ESP32-P4  /  WIFI6 TOUCH LCD-4B", 1);
    lv_obj_align(foot, LV_ALIGN_CENTER, 0, 232);

    lv_scr_load(scr);
}

void lakeshark_boot_splash_hide(void)
{
    if (s_splash) {

        if (s_splash != lv_scr_act())
            lv_obj_del(s_splash);
        s_splash = nullptr;
    }
}
