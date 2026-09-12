/* measure the real app/shared-widget tree with firmware fonts.
 * Only the radio, icon source and modal transport are mocked. */
#include "acars_gui/AppACARS.hpp"
#include "sdr_ui/sdr_ui.h"
#include "shell/ls_icons.h"
extern "C" {
#include "ls_test.h"
#include "acars_app.h"
#include "fm_state.h"
}
#include <cstdio>
#include <cstring>

static ls_iq_control_status_t radio;
static ls_text_entry_done_cb_t entry_done;
static void *entry_user;
static int entry_closes;
bool LsApp::back() { return true; }
const lv_img_dsc_t *ls_icon_for(const char *, int) { return nullptr; }
extern "C" {
fm_state_t FM;
void lakeshark_acars_start() { radio.receiver_streaming = true; }
void lakeshark_acars_stop() { radio.receiver_streaming = false; }
uint32_t lakeshark_acars_get_freq() { return (uint32_t)radio.requested_center_hz; }
void lakeshark_acars_set_freq(uint32_t hz) { radio.requested_center_hz = hz; }
void lakeshark_fm_set_gain(int gain) { radio.requested_gain_tenths_db = gain; }
void lakeshark_fm_agc() { radio.requested_gain_tenths_db = 0; }
int lakeshark_fm_gain_tenths() { return radio.requested_gain_tenths_db; }
void fm_get_receiver_status(ls_iq_control_status_t *out) { *out = radio; }
const char *ls_radio_err_name(ls_radio_err_t) { return "ERROR"; }
ls_text_entry_t *ls_text_entry_open(const ls_text_entry_config_t *,
    ls_text_entry_done_cb_t done, void *user) {
    entry_done = done; entry_user = user;
    return reinterpret_cast<ls_text_entry_t *>(&entry_user);
}
void ls_text_entry_close(ls_text_entry_t *) { ++entry_closes; }
}

static lv_obj_t *label(lv_obj_t *root, const char *text) {
    if (lv_obj_check_type(root, &lv_label_class) &&
        std::strcmp(lv_label_get_text(root), text) == 0) return root;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(root); ++i)
        if (auto *found = label(lv_obj_get_child(root, i), text)) return found;
    return nullptr;
}

static void click(lv_obj_t *root, const char *text) {
    auto *found = label(root, text);
    LS_CHECK(found != nullptr);
    if (found) lv_event_send(lv_obj_get_parent(found), LV_EVENT_CLICKED, nullptr);
}

static void check_bounds(lv_obj_t *root, int left, int right) {
    if (lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) return;
    lv_area_t area;
    lv_obj_get_coords(root, &area);
    LS_CHECK(area.x1 >= left);
    LS_CHECK(area.x2 <= right);
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(root); ++i)
        check_bounds(lv_obj_get_child(root, i), left, right);
}

static void exercise(int width, int height) {
    static bool initialized;
    if (!initialized) { lv_init(); initialized = true; }
    static lv_color_t pixels[720 * 12];
    lv_disp_draw_buf_t draw;
    lv_disp_draw_buf_init(&draw, pixels, nullptr, 720 * 12);
    lv_disp_drv_t driver;
    lv_disp_drv_init(&driver);
    driver.hor_res = width;
    driver.ver_res = height;
    driver.draw_buf = &draw;
    driver.flush_cb = [](lv_disp_drv_t *d, const lv_area_t *, lv_color_t *) {
        lv_disp_flush_ready(d);
    };
    auto *display = lv_disp_drv_register(&driver);
    lv_disp_set_default(display);
    auto *root = lv_obj_create(lv_scr_act());
    lv_obj_set_pos(root, 0, SDR_STATUS_H);
    lv_obj_set_size(root, width, height - SDR_STATUS_H - SDR_RAIL_H);
    radio = {};
    radio.requested_center_hz = radio.effective_center_hz = 131550000;
    radio.effective_center_known = radio.effective_gain_known = true;
    radio.requested_gain_tenths_db = radio.effective_gain_tenths_db = 200;
    radio.gain_state = radio.tune_state = LS_IQ_RESULT_EFFECTIVE;
    acars_app_clear();
    acars_app_inject(".N12345", "H1", "UA857 POSITION N43.5 W71.4 FL350 WEATHER REQUEST WITH LONG TEXT TO EXERCISE MESSAGE WRAPPING");
    AppACARS app;
    app.run(root);
    lv_obj_update_layout(root);
    check_bounds(root, 0, width - 1);
    auto *freq = label(root, "131.5500");
    LS_CHECK(freq != nullptr);
    lv_point_t text_size;
    lv_txt_get_size(&text_size, "131.5500", &lv_font_montserrat_48,
        0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    LS_CHECK(text_size.x <= lv_obj_get_content_width(freq));
    auto *clear = label(root, "CLEAR LOG");
    auto *controls = lv_obj_get_parent(clear);
    lv_area_t action;
    lv_obj_get_coords(controls, &action);
    LS_CHECK(action.y2 < height - SDR_RAIL_H);
    auto *agc = lv_obj_get_parent(label(root, "AGC"));
    const auto manual_color = lv_obj_get_style_bg_color(agc, 0).full;
    click(root, "AGC");
    LS_CHECK(lv_obj_get_style_bg_color(agc, 0).full != manual_color);
    click(root, "GAIN dB");
    entry_done(true, "25.4", entry_user);
    LS_EQ_INT(radio.requested_gain_tenths_db, 254);
    LS_EQ_INT(lv_obj_get_style_bg_color(agc, 0).full, manual_color);
    click(root, "GAIN dB");
    entry_done(true, "50.0", entry_user);
    LS_EQ_INT(radio.requested_gain_tenths_db, 254);
    click(root, "FREQ MHz");
    entry_done(true, "131..5", entry_user);
    LS_EQ_INT(radio.requested_center_hz, 131550000);
    click(root, "FREQ MHz");
    entry_done(true, "130.025", entry_user);
    LS_EQ_INT(radio.requested_center_hz, 130025000);
    click(root, "FREQ MHz");
    entry_done(false, "129.125", entry_user);
    LS_EQ_INT(radio.requested_center_hz, 130025000);
    click(root, "FREQ MHz");
    const int closes = entry_closes;
    app.pause();
    LS_EQ_INT(entry_closes, closes + 1);
    app.resume();
    lv_obj_update_layout(root);
    std::printf("ACARS %dx%d: frequency glyph width=%d available=%d; footer y=%d..%d shell bottom=%d; content scroll=%d\n",
        width, height, text_size.x, lv_obj_get_content_width(freq),
        action.y1, action.y2, height - SDR_RAIL_H - 1,
        (int)lv_obj_get_scroll_bottom(lv_obj_get_parent(lv_obj_get_parent(freq))));
    app.close();
    lv_obj_clean(root);
    app.run(root);
    lv_obj_update_layout(root);
    LS_CHECK(label(root, "130.0250") != nullptr);
    LS_CHECK(label(root, "UA857 POSITION N43.5 W71.4 FL350 WEATHER REQUEST WITH LONG TEXT TO EXERCISE MESSAGE WRAPPING") != nullptr);
    click(root, "GAIN dB");
    const int before_close = entry_closes;
    app.close();
    LS_EQ_INT(entry_closes, before_close + 1);
    lv_disp_remove(display);
}

LS_CASE(acars_lcd43_geometry_and_controls) { exercise(480, 800); }
LS_CASE(acars_lcd4b_geometry_and_controls) { exercise(720, 720); }
