#include "shell/ls_text_entry.h"
#include "sdr_ui/sdr_ui.h"
#include "ui/ls_ui.h"
#include <initializer_list>
const lv_img_dsc_t *ls_icon_for(const char *, int) { return nullptr; }
extern "C" {
#include "ls_test.h"
void ls_input_focus_modal(lv_obj_t *) {}
void ls_input_focus_screen(void) {}
}

static lv_obj_t *find_type(lv_obj_t *root, const lv_obj_class_t *type) {
    if (lv_obj_check_type(root, type)) return root;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(root); ++i)
        if (auto *found = find_type(lv_obj_get_child(root, i), type)) return found;
    return nullptr;
}
static bool same_color(lv_color_t a, lv_color_t b) { return a.full == b.full; }
static void check_keyboard_glyphs(lv_obj_t *keyboard)
{

    for(auto mode:{LV_KEYBOARD_MODE_TEXT_LOWER,LV_KEYBOARD_MODE_TEXT_UPPER,
                   LV_KEYBOARD_MODE_SPECIAL,LV_KEYBOARD_MODE_NUMBER}) {
        lv_keyboard_set_mode(keyboard,mode);
        const char *const *map=lv_btnmatrix_get_map(keyboard);
        const lv_font_t *font=lv_obj_get_style_text_font(keyboard,LV_PART_ITEMS);
        for(unsigned key=0;map[key] && map[key][0];key++) {
            uint32_t offset=0;
            while(map[key][offset]) {
                uint32_t character=_lv_txt_encoded_next(map[key],&offset);
                if(character<' ')continue;
                lv_font_glyph_dsc_t glyph={};
                LS_CHECK_MSG(lv_font_get_glyph_dsc(font,&glyph,character,0),
                             "keyboard mode%d key%s missingU+%04lx",(int)mode,map[key],(unsigned long)character);
            }
        }
    }
}
static void exercise(int width, int height, int safe_x=0, int safe_y=0) {
    static bool initialized;
    if (!initialized) { lv_init(); initialized = true; }
    ls_ui_set_safe_insets(safe_x,safe_y);
    static lv_color_t pixels[720 * 8];
    lv_disp_draw_buf_t draw;
    lv_disp_draw_buf_init(&draw, pixels, nullptr, 720 * 8);
    lv_disp_drv_t driver; lv_disp_drv_init(&driver);
    driver.hor_res = width; driver.ver_res = height; driver.draw_buf = &draw;
    driver.flush_cb = [](lv_disp_drv_t *d, const lv_area_t *, lv_color_t *) { lv_disp_flush_ready(d); };
    auto *display = lv_disp_drv_register(&driver);
    lv_disp_set_default(display);
    for (bool large : {false, true}) for (int mode = 0; mode < 2; ++mode) {
        ls_text_entry_config_t config = {};
        config.title = "ENTRY"; config.text = "123"; config.width = lv_pct(95);
        config.large = large; config.mode = (ls_text_entry_mode_t)mode;
        config.password = mode == LS_TEXT_ENTRY_TEXT;
        auto *entry = ls_text_entry_open(&config, nullptr, nullptr);
        LS_CHECK(entry != nullptr);
        auto *ta = find_type(lv_layer_top(), &lv_textarea_class);
        auto *kb = find_type(lv_layer_top(), &lv_keyboard_class);
        LS_CHECK(ta && kb);
        if (ta && kb) {
            LS_CHECK(same_color(lv_obj_get_style_bg_color(ta, LV_PART_MAIN), SDR_PANEL));
            LS_CHECK(same_color(lv_obj_get_style_text_color(ta, LV_PART_MAIN), SDR_TEXT));
            LS_CHECK(same_color(lv_obj_get_style_bg_color(kb, LV_PART_MAIN), SDR_BG));
            LS_CHECK(same_color(lv_obj_get_style_bg_color(kb, LV_PART_ITEMS), SDR_PANEL));
            LS_CHECK(same_color(lv_obj_get_style_text_color(kb, LV_PART_ITEMS), SDR_TEXT));
            LS_EQ_INT(lv_textarea_get_password_mode(ta), config.password);
            LS_EQ_INT(lv_keyboard_get_mode(kb), mode == LS_TEXT_ENTRY_NUMBER ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_UPPER);
            lv_obj_update_layout(lv_layer_top());
            lv_area_t a; lv_obj_get_coords(kb, &a);
            LS_CHECK(a.x1 >= safe_x && a.x2 < width-safe_x && a.y1 >= safe_y && a.y2 < height-safe_y);
            check_keyboard_glyphs(kb);
        }
        ls_text_entry_close(entry);
        LS_CHECK(find_type(lv_layer_top(), &lv_keyboard_class) == nullptr);
    }
    lv_disp_remove(display);
    ls_ui_set_safe_insets(0,0);
}
LS_CASE(shared_entry_dark_lcd43) { exercise(480, 800); }
LS_CASE(shared_entry_dark_lcd4b) { exercise(720, 720); }
LS_CASE(shared_mono_keeps_app_and_keyboard_symbols)
{
    for(const lv_font_t *font:{sdr_font_mono(),sdr_font_mono_sm()}) {
        for(const char *symbol:{LV_SYMBOL_BACKSPACE,LV_SYMBOL_NEW_LINE,
            LV_SYMBOL_KEYBOARD,LV_SYMBOL_LEFT,LV_SYMBOL_RIGHT,LV_SYMBOL_OK,
            LV_SYMBOL_PLUS,LV_SYMBOL_UP,LV_SYMBOL_AUDIO,LV_SYMBOL_WARNING,
            LV_SYMBOL_DIRECTORY,LV_SYMBOL_FILE,LV_SYMBOL_USB,LV_SYMBOL_SD_CARD,
            LV_SYMBOL_BLUETOOTH,LV_SYMBOL_VOLUME_MAX,LV_SYMBOL_VOLUME_MID,LV_SYMBOL_MUTE}) {
            uint32_t offset=0;
            uint32_t character=_lv_txt_encoded_next(symbol,&offset);
            lv_font_glyph_dsc_t glyph={};
            LS_CHECK_MSG(lv_font_get_glyph_dsc(font,&glyph,character,0),
                         "sharedfont missingU+%04lx",(unsigned long)character);
        }
        lv_font_glyph_dsc_t narrow={},wide={};
        LS_CHECK(lv_font_get_glyph_dsc(font,&narrow,'1',0));
        LS_CHECK(lv_font_get_glyph_dsc(font,&wide,'W',0));
        LS_EQ_UINT(narrow.adv_w,wide.adv_w);
    }
}
LS_CASE(tdisplay_keyboard_clears_rounded_corners) {
    exercise(568,1232,24,32);
    exercise(1232,568,24,32);
}
