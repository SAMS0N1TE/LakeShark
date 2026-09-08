#include "boot_splash.h"

#include "lvgl.h"
#include "bsp/display.h"
#include "ls_board.h"

/*LS-605*/
#define BOOT_BG     lv_color_hex(0x0A0C0D)
#define BOOT_RULE   lv_color_hex(0x23282C)
#define BOOT_ACCENT lv_color_hex(0xF2A233)
#define BOOT_DIMACC lv_color_hex(0x8A5C18)
#define BOOT_GREEN  lv_color_hex(0x5FCB86)
#define BOOT_TEXT   lv_color_hex(0xF4F7F9)
#define BOOT_DIM    lv_color_hex(0x515A61)

static lv_obj_t *s_splash = nullptr;

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

/*LS-605*/
static void rule(lv_obj_t *parent, int w, int y, lv_color_t c)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_size(r, w, 2);
    lv_obj_align(r, LV_ALIGN_CENTER, 0, y);
    lv_obj_set_style_bg_color(r, c, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_radius(r, 0, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
}

/*LS-605*/
static void corner(lv_obj_t *parent, lv_align_t align, int dx, int dy,
                   lv_border_side_t sides)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, 28, 28);
    lv_obj_align(c, align, dx, dy);
    lv_obj_set_style_bg_opa(c, LV_OPA_0, 0);
    lv_obj_set_style_border_color(c, BOOT_DIMACC, 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_border_side(c, sides, 0);
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
}

void lakeshark_boot_splash_show(void)
{
    lv_obj_t *scr = lv_obj_create(nullptr);
    s_splash = scr;
    lv_obj_set_style_bg_color(scr, BOOT_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    corner(scr, LV_ALIGN_TOP_LEFT,     18,  18, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT);
    corner(scr, LV_ALIGN_TOP_RIGHT,   -18,  18, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_RIGHT);
    corner(scr, LV_ALIGN_BOTTOM_LEFT,  18, -18, LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_LEFT);
    corner(scr, LV_ALIGN_BOTTOM_RIGHT,-18, -18, LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_RIGHT);

    lv_obj_t *title = mk_label(scr, &lv_font_montserrat_48, BOOT_TEXT, "LAKESHARK", 8);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -70);

    rule(scr, 300, -30, BOOT_ACCENT);

    lv_obj_t *sub = mk_label(scr, &lv_font_montserrat_18, BOOT_ACCENT,
                             "S D R   F I E L D   T E R M I N A L", 1);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, -2);

    lv_obj_t *caps = mk_label(scr, &lv_font_montserrat_16, BOOT_DIM,
                              "P25   /   FM   /   ADS-B", 3);
    lv_obj_align(caps, LV_ALIGN_CENTER, 0, 36);

    lv_obj_t *spin = lv_spinner_create(scr, 1000, 60);
    lv_obj_set_size(spin, 46, 46);
    lv_obj_align(spin, LV_ALIGN_CENTER, 0, 120);
    lv_obj_set_style_arc_color(spin, BOOT_RULE, LV_PART_MAIN);
    lv_obj_set_style_arc_color(spin, BOOT_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spin, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spin, 4, LV_PART_INDICATOR);

    lv_obj_t *boot = mk_label(scr, &lv_font_montserrat_16, BOOT_GREEN,
                              "STARTING RADIO BACKEND", 2);
    lv_obj_align(boot, LV_ALIGN_CENTER, 0, 172);

    lv_obj_t *foot = mk_label(scr, &lv_font_montserrat_12, BOOT_DIM,
                              LS_BOARD_NAME, 2);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -40);

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
