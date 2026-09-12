/* Real HOME/LVGL/font geometry. Stand-ins model hub snapshots, shell launches
 * and persistence transport only; selection cache/presentation are production. */
#include "home/AppHome.hpp"
#include "home/home_widget_view.h"
#include "shell/ls_shell.hpp"
#include "shell/ls_icons.h"
#include "sdr_ui/sdr_ui.h"
extern "C" {
#include "ls_test.h"
}
#include <cstdio>
#include <cstring>

static ls_hub_state_t hub;
static int stored, writes, unsubscribed, launches;
static bool write_ok;
static bool persist(int id) { ++writes; if (!write_ok) return false; stored = id; return true; }
bool LsApp::back() { return true; }
LsShell &LsShell::instance() { static LsShell shell; return shell; }
bool LsShell::launchByName(const char *) { ++launches; return true; }
void LsShell::cycleApp(int) {}
const lv_img_dsc_t *ls_icon_for(const char *, int) {
    static lv_img_dsc_t icon = {};
    static uint8_t pixels[48 * 48 * sizeof(lv_color_t)];
    icon.header.cf = LV_IMG_CF_TRUE_COLOR;
    icon.header.w = icon.header.h = 48;
    icon.data = pixels;
    icon.data_size = sizeof(pixels);
    return &icon;
}
extern "C" {
int settings_get_home_widget() { return home_widget_pref_get(); }
bool settings_set_home_widget(int id) { return home_widget_pref_set(id, persist); }
const ls_hub_state_t *ls_hub_state() { return &hub; }
int ls_hub_subscribe(ls_hub_fn fn, void *user) { fn(&hub, LS_HUB_ALL, user); return 0; }
void ls_hub_unsubscribe(int) { ++unsubscribed; }
bool ls_hub_last_line(char *, int) { return false; }
}

LS_CASE(preference_missing_corrupt_and_failed_write_fallback) {
    home_widget_pref_init(false, HOME_WIDGET_CLOCK);
    LS_EQ_INT(home_widget_pref_get(), HOME_WIDGET_RECEIVER);
    home_widget_pref_init(true, 255);
    LS_EQ_INT(home_widget_pref_get(), HOME_WIDGET_RECEIVER);
    home_widget_pref_init(true, -1);
    LS_EQ_INT(home_widget_pref_get(), HOME_WIDGET_RECEIVER);
    writes = 0; write_ok = false;
    LS_CHECK(!home_widget_pref_set(HOME_WIDGET_CLOCK, persist));
    LS_EQ_INT(home_widget_pref_get(), HOME_WIDGET_RECEIVER);
    LS_EQ_INT(writes, 1);
    LS_CHECK(!home_widget_pref_set(HOME_WIDGET_COUNT, persist));
    LS_EQ_INT(writes, 1);
    write_ok = true;
    LS_CHECK(home_widget_pref_set(HOME_WIDGET_CLOCK, persist));
    home_widget_pref_init(true, stored);
    LS_EQ_INT(home_widget_pref_get(), HOME_WIDGET_CLOCK);
}

LS_CASE(presentation_never_calls_retained_receiver_state_live_or_unsynced_clock_real) {
    home_widget_view_t view;
    ls_hub_state_t state = {};
    state.active = state.rtl_ready = true;
    state.freq_hz = 131550000;
    std::strcpy(state.target_app, "ACARS");
    home_widget_present(HOME_WIDGET_RECEIVER, &state, false, 0, &view);
    LS_EQ_STR(view.title, "LAST USED: ACARS");
    LS_EQ_STR(view.detail, "Receiver stopped - tap to reopen");
    home_widget_present(HOME_WIDGET_CLOCK, &state, false, 1788732000, &view);
    LS_EQ_STR(view.value, "--:--:--");
    home_widget_present(HOME_WIDGET_CLOCK, &state, true, 1788732000, &view);
    LS_CHECK(std::strstr(view.detail, "UTC") != nullptr);
    LS_CHECK(std::strcmp(view.value, "--:--:--") != 0);
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
static void horizontal_bounds(lv_obj_t *root, int width) {
    if (lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) return;
    lv_area_t a; lv_obj_get_coords(root, &a);
    if (a.x1 < 0 || a.x2 >= width)
        std::printf("overflow x=%d..%d text=%s\n", a.x1, a.x2,
            lv_obj_check_type(root, &lv_label_class) ? lv_label_get_text(root) : "object");
    LS_CHECK(a.x1 >= 0 && a.x2 < width);
    if (lv_obj_check_type(root, &lv_label_class)) {
        lv_area_t parent; lv_obj_get_coords(lv_obj_get_parent(root), &parent);
        LS_CHECK(a.x1 >= parent.x1 && a.x2 <= parent.x2);
    }
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(root); ++i)
        horizontal_bounds(lv_obj_get_child(root, i), width);
}
static void exercise(int width, int height) {
    static bool initialized;
    if (!initialized) { lv_init(); initialized = true; }
    static lv_color_t pixels[720 * 12];
    lv_disp_draw_buf_t draw;
    lv_disp_draw_buf_init(&draw, pixels, nullptr, 720 * 12);
    lv_disp_drv_t driver; lv_disp_drv_init(&driver);
    driver.hor_res = width; driver.ver_res = height; driver.draw_buf = &draw;
    driver.flush_cb = [](lv_disp_drv_t *d, const lv_area_t *, lv_color_t *) { lv_disp_flush_ready(d); };
    auto *display = lv_disp_drv_register(&driver);
    lv_disp_set_default(display);
    auto *root = lv_obj_create(lv_scr_act());
    lv_obj_set_pos(root, 0, SDR_STATUS_H);
    lv_obj_set_size(root, width, height - SDR_STATUS_H - SDR_RAIL_H);
    hub = {}; std::strcpy(hub.target_app, "ACARS"); hub.freq_hz = 131550000;
    hub.active = true; hub.c6_state = 1; hub.sd_present = true;
    write_ok = true; launches = 0;
    home_widget_pref_init(false, 0);
    AppHome app; app.run(root);
    lv_obj_update_layout(root);
    LS_EQ_INT(launches, 0);
    LS_CHECK(label(root, "LAST USED: ACARS") != nullptr);
    hub.freq_hz = 0; hub.target_app[0] = 0;
    app.resume();
    LS_CHECK(label(root, "131.5500 MHz") != nullptr);
    const char *choices[] = { "RECEIVER", "SYSTEM", "CLOCK" };
    for (const auto *choice : choices) {
        click(root, choice); lv_obj_update_layout(root);
        horizontal_bounds(root, width);
        auto *value = label(root, choice);
        lv_area_t a; lv_obj_get_coords(value, &a);
        LS_CHECK(a.y2 < height - SDR_RAIL_H);
        auto *content = lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(value)));
        LS_CHECK(lv_obj_get_scroll_bottom(content) <= 0);
        std::printf("HOME %dx%d %s picker bottom=%d shell bottom=%d scroll=%d\n",
            width, height, choice, a.y2, height - SDR_RAIL_H - 1,
            (int)lv_obj_get_scroll_bottom(content));
    }
    LS_EQ_INT(stored, HOME_WIDGET_CLOCK);
    app.close(); lv_obj_clean(root);
    app.run(root); lv_obj_update_layout(root);
    LS_CHECK(label(root, "UTC CLOCK") != nullptr);
    LS_EQ_INT(launches, 0);
    write_ok = false;
    click(root, "SYSTEM");
    LS_CHECK(label(root, "Selection not saved - try again") != nullptr);
    LS_CHECK(label(root, "UTC CLOCK") != nullptr);
    app.close();
    lv_disp_remove(display);
}
LS_CASE(home_lcd43_widgets_and_reopen) { exercise(480, 800); }
/* keep the square-panel overflow regression as a normal gate. */
LS_CASE(home_lcd4b_widgets_and_reopen) { exercise(720, 720); }
