
#include "../ui.h"

void ui_ScreenSettingVerification_screen_init(void)
{
    ui_ScreenSettingVerification = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenSettingVerification, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_ScreenSettingVerification, lv_color_hex(0xE5F3FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_ScreenSettingVerification, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_LabelScreenSettingVerification = lv_label_create(ui_ScreenSettingVerification);
    lv_obj_set_width(ui_LabelScreenSettingVerification, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_LabelScreenSettingVerification, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_LabelScreenSettingVerification, -150);
    lv_obj_set_y(ui_LabelScreenSettingVerification, -150);
    lv_obj_set_align(ui_LabelScreenSettingVerification, LV_ALIGN_CENTER);
    lv_label_set_text(ui_LabelScreenSettingVerification, "SSID:");
    lv_obj_set_style_text_font(ui_LabelScreenSettingVerification, &lv_font_montserrat_30, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_LabelScreenSettingVerificationSSID = lv_label_create(ui_ScreenSettingVerification);
    lv_obj_set_width(ui_LabelScreenSettingVerificationSSID, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_LabelScreenSettingVerificationSSID, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_LabelScreenSettingVerificationSSID, 0);
    lv_obj_set_y(ui_LabelScreenSettingVerificationSSID, -150);
    lv_obj_set_align(ui_LabelScreenSettingVerificationSSID, LV_ALIGN_CENTER);
    lv_obj_set_style_text_font(ui_LabelScreenSettingVerificationSSID, &lv_font_montserrat_30,
                               LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_TextAreaScreenSettingVerificationPassword = lv_textarea_create(ui_ScreenSettingVerification);
    lv_obj_set_width(ui_TextAreaScreenSettingVerificationPassword, 600);
    lv_obj_set_height(ui_TextAreaScreenSettingVerificationPassword, 70);
    lv_obj_set_x(ui_TextAreaScreenSettingVerificationPassword, 0);
    lv_obj_set_y(ui_TextAreaScreenSettingVerificationPassword, -50);
    lv_obj_set_align(ui_TextAreaScreenSettingVerificationPassword, LV_ALIGN_CENTER);
    lv_textarea_set_placeholder_text(ui_TextAreaScreenSettingVerificationPassword, "Password...");
    lv_obj_set_style_text_font(ui_TextAreaScreenSettingVerificationPassword, &lv_font_montserrat_30,
                               LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_KeyboardScreenSettingVerification = lv_keyboard_create(ui_ScreenSettingVerification);
    lv_obj_set_width(ui_KeyboardScreenSettingVerification, 680);
    lv_obj_set_height(ui_KeyboardScreenSettingVerification, 240);
    lv_obj_set_x(ui_KeyboardScreenSettingVerification, 0);
    lv_obj_set_y(ui_KeyboardScreenSettingVerification, 120);
    lv_obj_set_align(ui_KeyboardScreenSettingVerification, LV_ALIGN_CENTER);

    ui_SpinnerScreenSettingVerification = lv_spinner_create(ui_ScreenSettingVerification, 1000, 90);
    lv_obj_set_width(ui_SpinnerScreenSettingVerification, 150);
    lv_obj_set_height(ui_SpinnerScreenSettingVerification, 150);
    lv_obj_set_x(ui_SpinnerScreenSettingVerification, 0);
    lv_obj_set_y(ui_SpinnerScreenSettingVerification, -50);
    lv_obj_set_align(ui_SpinnerScreenSettingVerification, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_SpinnerScreenSettingVerification, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_SpinnerScreenSettingVerification, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(ui_SpinnerScreenSettingVerification, 5, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_set_style_arc_color(ui_SpinnerScreenSettingVerification, lv_color_hex(0xB8B8B8),
                               LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_opa(ui_SpinnerScreenSettingVerification, 255, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_width(ui_SpinnerScreenSettingVerification, 5, LV_PART_INDICATOR | LV_STATE_DEFAULT);
}