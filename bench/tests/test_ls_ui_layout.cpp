/* Real LVGL 8.4 layout and software-render coverage for . */

extern "C" {
#include "ls_test.h"
#include "ui/ls_ui.h"
#include "sdr_ui/sdr_ui.h"
}

#include <cstdint>
#include <cstring>

static lv_disp_t *s_display;
static lv_color_t s_draw_buffer[480 * 16];
static lv_disp_draw_buf_t s_draw;
static lv_disp_drv_t s_driver;
static uint32_t s_flush_count;
static uint32_t s_flush_pixels;
static uint32_t s_pixel_hash;

extern "C" const lv_font_t *sdr_font_mono(void) { return LV_FONT_DEFAULT; }
extern "C" const lv_font_t *sdr_font_mono_sm(void) { return LV_FONT_DEFAULT; }
extern "C" const lv_font_t *sdr_font_ui(void) { return LV_FONT_DEFAULT; }

extern "C" lv_obj_t *sdr_label(lv_obj_t *parent, const lv_font_t *font,
                                  lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font ? font : LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(label, color, 0);
    /* production sdr_label() sets this, and the stub did not. That
     * single divergence is why the bench never saw the row names wrap on
     * hardware: every label under test was one that could not wrap. A stub
     * that is easier to satisfy than the real thing is worse than no stub. */
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

extern "C" lv_obj_t *sdr_btn(lv_obj_t *parent, const char *text,
                                lv_event_cb_t callback, void *user_data,
                                lv_obj_t **out_label)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_height(button, 50);
    lv_obj_set_width(button, LV_SIZE_CONTENT);
    lv_obj_set_style_min_width(button, 56, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_radius(button, 2, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_hor(button, 16, 0);
    lv_obj_set_style_pad_ver(button, 6, 0);
    if (callback)
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    lv_obj_t *label = lv_label_create(button);
    lv_obj_set_style_text_font(label, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_letter_space(label, 1, 0);
    lv_label_set_text(label, text ? text : "");
    lv_obj_center(label);
    if (out_label) *out_label = label;
    return button;
}

extern "C" void sdr_style_tabview(lv_obj_t *) {}

extern "C" void sdr_text_if_changed(lv_obj_t *label, const char *text)
{
    if (!label || !text) return;
    const char *old = lv_label_get_text(label);
    if (!old || std::strcmp(old, text) != 0) lv_label_set_text(label, text);
}

static void flush_cb(lv_disp_drv_t *driver, const lv_area_t *area,
                     lv_color_t *pixels)
{
    const uint32_t count = (uint32_t)lv_area_get_width(area) *
                           (uint32_t)lv_area_get_height(area);
    ++s_flush_count;
    s_flush_pixels += count;
    for (uint32_t i = 0; i < count; ++i) {
        s_pixel_hash ^= pixels[i].full;
        s_pixel_hash *= 16777619u;
    }
    lv_disp_flush_ready(driver);
}

static lv_obj_t *fresh_root(void)
{
    if (!s_display) {
        lv_init();
        lv_disp_draw_buf_init(&s_draw, s_draw_buffer, nullptr,
                              sizeof(s_draw_buffer) / sizeof(s_draw_buffer[0]));
        lv_disp_drv_init(&s_driver);
        s_driver.hor_res = 480;
        s_driver.ver_res = 800;
        s_driver.draw_buf = &s_draw;
        s_driver.flush_cb = flush_cb;
        s_display = lv_disp_drv_register(&s_driver);
    }

    lv_obj_t *screen = lv_disp_get_scr_act(s_display);
    lv_obj_clean(screen);
    lv_obj_set_style_pad_all(screen, 0, 0);

    lv_obj_t *root = lv_obj_create(screen);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(root, 6, 0);
    lv_obj_set_style_pad_row(root, 6, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    return root;
}

static lv_area_t coords(lv_obj_t *object)
{
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    return area;
}

static bool inside(lv_obj_t *child, lv_obj_t *parent)
{
    const lv_area_t a = coords(child);
    const lv_area_t b = coords(parent);
    return a.x1 >= b.x1 && a.y1 >= b.y1 && a.x2 <= b.x2 && a.y2 <= b.y2;
}

static bool overlaps(lv_obj_t *a, lv_obj_t *b)
{
    const lv_area_t x = coords(a);
    const lv_area_t y = coords(b);
    return !(x.x2 < y.x1 || y.x2 < x.x1 || x.y2 < y.y1 || y.y2 < x.y1);
}

static void check_children_do_not_overlap(lv_obj_t *parent)
{
    const uint32_t count = lv_obj_get_child_cnt(parent);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t *a = lv_obj_get_child(parent, (int32_t)i);
        LS_CHECK(inside(a, parent));
        for (uint32_t j = i + 1; j < count; ++j)
            LS_CHECK(!overlaps(a, lv_obj_get_child(parent, (int32_t)j)));
    }
}

static void render_and_check(void)
{
    s_flush_count = 0;
    s_flush_pixels = 0;
    s_pixel_hash = 2166136261u;
    lv_obj_invalidate(lv_disp_get_scr_act(s_display));
    lv_refr_now(s_display);
    LS_CHECK(s_flush_count > 0);
    LS_CHECK(s_flush_pixels >= 480u * 800u);
    LS_CHECK(s_pixel_hash != 2166136261u);
    ls_note("rendered 480x800 via LVGL: flushes=%lu pixels=%lu hash=%08lx",
            (unsigned long)s_flush_count, (unsigned long)s_flush_pixels,
            (unsigned long)s_pixel_hash);
}

LS_CASE(reapplying_button_role_does_not_redraw_but_changed_role_does)
{
    lv_obj_t *root=fresh_root();
    auto *button=ls_ui_button(root,"MODE",LS_BTN_TOGGLE_OFF,nullptr,nullptr,nullptr);
    lv_refr_now(s_display);
    uint32_t before=s_flush_count;
    for(int i=0;i<20;i++)ls_ui_button_set_role(button,LS_BTN_TOGGLE_OFF);
    lv_refr_now(s_display);
    LS_EQ_INT(s_flush_count,before);
    ls_ui_button_set_role(button,LS_BTN_TOGGLE_ON);
    lv_refr_now(s_display);
    LS_CHECK(s_flush_count>before);
}

LS_CASE(shell_apps_never_duplicate_the_status_header)
{
    const char *names[] = {"FM", "ACARS", "ADS-B", "REC", "P25",
                           "MAP", "HOME", "FILES", "SETTINGS", "MUSIC"};
    for (const char *name : names) {
        for (int tabbed = 0; tabbed < 2; ++tabbed) {
            lv_obj_t *root = fresh_root();
            ls_ui_screen_t screen;
            ls_ui_screen_create(root, name, tabbed != 0,
                                LS_UI_COLOR_ACCENT, &screen);
            LS_CHECK(screen.header == nullptr);
            LS_CHECK(screen.name == nullptr);
            LS_CHECK(screen.readout == nullptr);
            LS_CHECK(screen.lamp == nullptr);
            ls_ui_screen_set_readout(&screen, "123.450 MHz");
            ls_ui_screen_set_lamp(&screen, true, LS_UI_COLOR_ACCENT);
            if (tabbed) LS_CHECK(ls_ui_screen_add_tab(&screen, "MAIN") != nullptr);
            else LS_CHECK(screen.content != nullptr);
            lv_obj_update_layout(root);
        }
    }
}

LS_CASE(standalone_recovery_retains_its_header)
{
    lv_obj_t *root = fresh_root();
    ls_ui_screen_t screen;
    ls_ui_standalone_screen_create(root, "SAFE MODE", false,
                                  LS_UI_COLOR_ALARM, &screen);
    LS_CHECK(screen.header != nullptr);
    LS_CHECK(screen.readout != nullptr);
    LS_CHECK(screen.lamp != nullptr);
    LS_EQ_STR(lv_label_get_text(screen.name), "SAFE MODE");
    ls_ui_screen_set_readout(&screen, "RECOVERY");
    LS_EQ_STR(lv_label_get_text(screen.readout), "RECOVERY");
}

LS_CASE(basic_lvgl_host_layout_is_live)
{
    lv_obj_t *root = fresh_root();
    lv_obj_t *label = lv_label_create(root);
    lv_label_set_text(label, "LVGL");
    lv_obj_update_layout(root);
    LS_CHECK(inside(label, root));
    render_and_check();
}

LS_CASE(value_heading_and_five_p25_controls_have_disjoint_bounds)
{
    lv_obj_t *root = fresh_root();
    ls_ui_value_t row;
    ls_ui_value(root, "FREQUENCY", &row);
    lv_label_set_text(row.value,
                      "1234.567890 MHz / a deliberately overlong value");
    static const char *labels[] = {"ENTER", "-1M", "-25k", "+25k", "+1M"};
    for (const char *label : labels)
        ls_ui_button(row.controls, label, LS_BTN_DEFAULT, nullptr, nullptr,
                     nullptr);

    lv_obj_update_layout(root);

    /* there is no heading container and no controls container any
       more - the name, the value and the controls are the row's own children.
       Every claim made is still made here, against the flat tree: the
       name and the value never share space, no control ever lands on the
       value, every caption stays inside its own button, nothing escapes the
       row, and a control that does not fit starts a line instead of
       overlapping or running off the right edge. */
    LS_CHECK(lv_obj_get_parent(row.name) == row.row);
    LS_CHECK(lv_obj_get_parent(row.value) == row.row);
    LS_CHECK(row.controls == row.row);
    LS_CHECK(!overlaps(row.name, row.value));
    check_children_do_not_overlap(row.row);

    lv_obj_t *first = nullptr;
    lv_obj_t *previous = nullptr;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(row.row); ++i) {
        lv_obj_t *child = lv_obj_get_child(row.row, (int32_t)i);
        if (child == row.name || child == row.value) continue;
        LS_CHECK(inside(child, row.row));
        LS_CHECK(inside(lv_obj_get_child(child, 0), child));
        LS_CHECK(!overlaps(child, row.value));
        LS_CHECK(!overlaps(child, row.name));
        if (!first) first = child;
        if (previous) {
            /* Same line, or a line strictly below it. */
            LS_CHECK(coords(child).y1 == coords(previous).y1 ||
                     coords(child).y1 > coords(previous).y2);
        }
        previous = child;
    }
    LS_CHECK(first != nullptr);
    LS_CHECK(coords(previous).x2 <= coords(row.row).x2);
    render_and_check();
}

LS_CASE(scan_panel_four_button_group_is_one_equal_width_line)
{
    lv_obj_t *root = fresh_root();
    ls_ui_value_t row;
    ls_ui_value(root, "START", &row);
    lv_label_set_text(row.value, "154.0000");
    lv_obj_t *group = ls_ui_button_group(row.controls);
    static const char *labels[] = {"-1M", "-100k", "+100k", "+1M"};
    for (const char *label : labels)
        ls_ui_group_button(group, label, LS_BTN_DEFAULT, nullptr, nullptr,
                           nullptr);

    lv_obj_update_layout(root);
    LS_CHECK(inside(group, row.controls));
    check_children_do_not_overlap(group);
    lv_obj_t *first = lv_obj_get_child(group, 0);
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(group); ++i) {
        lv_obj_t *button = lv_obj_get_child(group, (int32_t)i);
        LS_NEAR(lv_obj_get_width(button), lv_obj_get_width(first), 1);
        LS_EQ_INT(coords(button).y1, coords(first).y1);
        LS_CHECK(inside(lv_obj_get_child(button, 0), button));
    }
    render_and_check();
}

LS_CASE(six_shared_buttons_wrap_as_whole_nonoverlapping_controls)
{
    lv_obj_t *root = fresh_root();
    lv_obj_t *controls = ls_ui_controls(root);
    static const char *labels[] = {
        "C-SCAN", "HOLD", "LOCK", "MODE", "AGC", "RESET"
    };
    for (const char *label : labels)
        ls_ui_button(controls, label, LS_BTN_DEFAULT, nullptr, nullptr, nullptr);

    lv_obj_update_layout(root);
    check_children_do_not_overlap(controls);
    LS_CHECK(lv_obj_get_height(controls) >= 2 * 50 + 4);
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(controls); ++i) {
        lv_obj_t *button = lv_obj_get_child(controls, (int32_t)i);
        LS_CHECK(inside(lv_obj_get_child(button, 0), button));
    }
    render_and_check();
}

static void hold_fired_cb(lv_event_t *event)
{
    ++*static_cast<int *>(lv_event_get_user_data(event));
}

static const char *text_of(lv_obj_t *label)
{
    return lv_label_get_text(label);
}

LS_CASE(hold_button_states_its_hold_and_counts_down_without_overstating)
{
    lv_obj_t *root = fresh_root();
    lv_obj_t *hint = ls_ui_hold_hint(root, 1500);
    int fired = 0;
    lv_obj_t *button = ls_ui_hold_button_hinted(root, "HOLD 3", 1500,
                                                LS_BTN_DANGER, hold_fired_cb,
                                                &fired, hint,
                                                "TRY NORMAL BOOT");
    LS_CHECK(button != nullptr);
    lv_obj_update_layout(root);
    lv_obj_t *caption = lv_obj_get_child(button, 0);

    /* Before anything is pressed: the rule, with its duration, and what a tap
       will do - all three on screen without a press to reveal them. */
    LS_CHECK_MSG(std::strstr(text_of(hint), "1.5 s") != nullptr,
                 "idle hint: '%s'", text_of(hint));
    LS_CHECK_MSG(std::strstr(text_of(hint), "TAP") != nullptr,
                 "idle hint: '%s'", text_of(hint));

    /* Contact. The countdown starts on PRESSED, not on the first PRESSING,
       and it does not claim two seconds of a one-and-a-half second hold. */
    lv_tick_inc(500);
    lv_event_send(button, LV_EVENT_PRESSED, nullptr);
    LS_EQ_STR(text_of(caption), "HOLD 1.5");
    LS_CHECK_MSG(std::strstr(text_of(hint), "TRY NORMAL BOOT") != nullptr,
                 "armed hint: '%s'", text_of(hint));

    /* Midpoint: 750 ms in, 750 ms left, and the caption has moved. */
    lv_tick_inc(750);
    lv_event_send(button, LV_EVENT_PRESSING, nullptr);
    LS_EQ_STR(text_of(caption), "HOLD 0.7");

    lv_event_send(button, LV_EVENT_RELEASED, nullptr);
    LS_EQ_INT(fired, 0);
    LS_EQ_STR(text_of(caption), "HOLD 3");
    LS_CHECK_MSG(std::strstr(text_of(hint), "CANCELLED") != nullptr,
                 "cancelled hint: '%s'", text_of(hint));

    /* A completed hold fires once and confirms, and the release that ends it
       must not repaint that confirmation as a refusal. */
    lv_event_send(button, LV_EVENT_PRESSED, nullptr);
    lv_tick_inc(1500);
    lv_event_send(button, LV_EVENT_PRESSING, nullptr);
    LS_EQ_INT(fired, 1);
    LS_EQ_STR(text_of(caption), "DONE");
    LS_CHECK_MSG(std::strstr(text_of(hint), "CONFIRMED") != nullptr,
                 "fired hint: '%s'", text_of(hint));
    lv_event_send(button, LV_EVENT_RELEASED, nullptr);
    LS_CHECK_MSG(std::strstr(text_of(hint), "CANCELLED") == nullptr,
                 "post-fire hint: '%s'", text_of(hint));
    LS_EQ_INT(fired, 1);

    /* The line is one row above the control it explains, inside the same
       frame, and does not land on top of it on a 480 px panel. */
    lv_obj_update_layout(root);
    LS_CHECK(coords(hint).y2 < coords(button).y1);
    check_children_do_not_overlap(root);
    render_and_check();
}

LS_CASE(a_hold_button_without_a_hint_still_counts_down)
{
    /* ls_ui_hold_button() is the no-hint spelling and must stay usable: the
       hint is opt-in, and a caller that passes none must not be reading a
       null label pointer on every press. */
    lv_obj_t *root = fresh_root();
    int fired = 0;
    lv_obj_t *button = ls_ui_hold_button(root, "HOLD 3", 3000, LS_BTN_DANGER,
                                         hold_fired_cb, &fired);
    LS_CHECK(button != nullptr);
    lv_obj_t *caption = lv_obj_get_child(button, 0);

    lv_tick_inc(100);
    lv_event_send(button, LV_EVENT_PRESSED, nullptr);
    LS_EQ_STR(text_of(caption), "HOLD 3.0");
    lv_tick_inc(2999);
    lv_event_send(button, LV_EVENT_PRESSING, nullptr);
    LS_EQ_STR(text_of(caption), "HOLD 0.0");
    LS_EQ_INT(fired, 0);
    lv_tick_inc(1);
    lv_event_send(button, LV_EVENT_PRESSING, nullptr);
    LS_EQ_INT(fired, 1);
    lv_event_send(button, LV_EVENT_RELEASED, nullptr);
    LS_EQ_STR(text_of(caption), "HOLD 3");
    lv_obj_update_layout(root);
    render_and_check();
}

static lv_coord_t line_height(lv_obj_t *row)
{
    return lv_obj_get_height(row);
}

LS_CASE(a_single_control_shares_its_value_line_instead_of_taking_one)
{
    lv_obj_t *root = fresh_root();

    ls_ui_value_t bare;
    ls_ui_value(root, "PREFERENCE WRITE", &bare);
    lv_label_set_text(bare.value, "READY");

    ls_ui_value_t row;
    ls_ui_value(root, "USB AUTO-REBOOT", &row);
    lv_label_set_text(row.value, "OFF");
    lv_obj_t *toggle = ls_ui_button(row.controls, "TOGGLE", LS_BTN_TOGGLE_OFF,
                                    nullptr, nullptr, nullptr);

    lv_obj_update_layout(root);

    /* The button is beside the value, not under it. */
    LS_CHECK(!overlaps(toggle, row.value));
    LS_CHECK(coords(toggle).x1 > coords(row.value).x2);
    LS_CHECK(coords(toggle).y1 <= coords(row.value).y1);
    LS_CHECK(coords(toggle).y2 >= coords(row.value).y2);

    /* And the row is one control tall - the padding and border of the frame
       around a 50 px button, and nothing else. */
    LS_CHECK_MSG(line_height(row.row) <= lv_obj_get_height(toggle) + 16,
                 "a one-button row is %d px around a %d px button",
                 (int)line_height(row.row), (int)lv_obj_get_height(toggle));
    LS_CHECK(line_height(row.row) > line_height(bare.row));
    LS_CHECK(inside(toggle, row.row));
    check_children_do_not_overlap(row.row);
    render_and_check();
}

LS_CASE(a_stepper_rides_beside_its_value_rather_than_under_it)
{
    lv_obj_t *root = fresh_root();
    ls_ui_value_t row;
    ls_ui_value(root, "HANG", &row);
    lv_label_set_text(row.value, "2.0 s");
    lv_obj_t *group = ls_ui_stepper(&row, nullptr, nullptr, nullptr, nullptr);

    lv_obj_update_layout(root);
    LS_CHECK(group != nullptr);
    LS_EQ_UINT(lv_obj_get_child_cnt(group), 2u);
    LS_CHECK(!overlaps(group, row.value));
    LS_CHECK(coords(group).x1 > coords(row.value).x2);
    LS_CHECK(coords(group).x2 <= coords(row.row).x2);
    LS_EQ_INT(coords(lv_obj_get_child(group, 0)).y1,
              coords(lv_obj_get_child(group, 1)).y1);
    LS_CHECK_MSG(line_height(row.row) <=
                     lv_obj_get_height(lv_obj_get_child(group, 0)) + 16,
                 "a stepper row is %d px tall", (int)line_height(row.row));
    check_children_do_not_overlap(group);
    render_and_check();
}

LS_CASE(a_full_width_group_still_owns_its_own_line_after_flattening)
{
    lv_obj_t *root = fresh_root();
    ls_ui_value_t row;
    ls_ui_value(root, "FREQUENCY", &row);
    lv_label_set_text(row.value, "154.785000 MHz");
    lv_obj_t *enter = ls_ui_button(row.controls, "ENTER", LS_BTN_PRIMARY,
                                   nullptr, nullptr, nullptr);
    lv_obj_t *group = ls_ui_button_group(row.controls);
    static const char *nudge[] = {"-1M", "-25k", "+25k", "+1M"};
    for (const char *label : nudge)
        ls_ui_group_button(group, label, LS_BTN_DEFAULT, nullptr, nullptr,
                           nullptr);

    lv_obj_update_layout(root);

    LS_CHECK(!overlaps(enter, row.value));
    LS_CHECK(coords(enter).y1 <= coords(row.value).y1);
    LS_CHECK(coords(group).y1 > coords(enter).y2);
    LS_CHECK(!overlaps(group, row.value));

    lv_obj_t *first = lv_obj_get_child(group, 0);
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(group); ++i) {
        lv_obj_t *button = lv_obj_get_child(group, (int32_t)i);
        LS_NEAR(lv_obj_get_width(button), lv_obj_get_width(first), 1);
        LS_EQ_INT(coords(button).y1, coords(first).y1);
        LS_CHECK(inside(lv_obj_get_child(button, 0), button));
    }
    check_children_do_not_overlap(row.row);
    check_children_do_not_overlap(group);
    LS_CHECK(coords(group).x2 <= coords(row.row).x2);
    render_and_check();
}

/* A toggle says its state on the control, once.

   The rejected form was a button captioned TOGGLE beside a value column
   reading ON or OFF: the state twice, the caption naming nothing, and the
   control itself distinguished only by fill colour.  Here the caption names
   the setting and never changes, the marker carries the state, the frame gains
   an outline as a second non-colour cue, and the value column is left empty
   for a setting that has more than two choices to put its choice in. */
LS_CASE(a_toggle_states_itself_without_spending_the_value_column)
{
    lv_obj_t *root = fresh_root();
    ls_ui_value_t row;
    ls_ui_value(root, "SYNC BEEP", &row);
    lv_obj_t *toggle = ls_ui_toggle(&row, "SYNC BEEP", false, nullptr, nullptr);
    LS_CHECK(toggle != nullptr);
    lv_obj_t *caption = lv_obj_get_child(toggle, 0);
    lv_obj_update_layout(root);

    LS_EQ_STR(text_of(caption), "    SYNC BEEP");
    LS_EQ_STR(lv_label_get_text(row.value), "");
    LS_EQ_INT(lv_obj_get_style_outline_width(toggle, LV_PART_MAIN), 0);

    ls_ui_toggle_set(toggle, true);
    lv_obj_update_layout(root);
    LS_EQ_STR(text_of(caption), "[X] SYNC BEEP");
    LS_CHECK_MSG(lv_obj_get_style_outline_width(toggle, LV_PART_MAIN) > 0,
                 "an engaged toggle is distinguished by colour alone");
    /* The marker is the same width in both states, so the caption keeps its
       character count and the control cannot reflow its row when pressed. */
    LS_EQ_UINT(std::strlen(text_of(caption)), std::strlen("    SYNC BEEP"));

    ls_ui_toggle_set(toggle, false);
    lv_obj_update_layout(root);
    LS_EQ_STR(text_of(caption), "    SYNC BEEP");
    LS_EQ_INT(lv_obj_get_style_outline_width(toggle, LV_PART_MAIN), 0);

    /* An outline is drawn outside the object's box, so the cue costs no room:
       the row is no taller than the button it holds. */
    LS_CHECK(line_height(row.row) <= lv_obj_get_height(toggle) + 16);
    LS_CHECK(inside(toggle, row.row));
    check_children_do_not_overlap(row.row);
    render_and_check();
}

LS_CASE(a_bare_toggle_marker_changes_state_without_a_caption)
{
    lv_obj_t *root = fresh_root();
    ls_ui_value_t row;
    ls_ui_value(root, "MUTE", &row);
    lv_obj_t *toggle = ls_ui_toggle(&row, nullptr, false, nullptr, nullptr);
    lv_obj_t *caption = lv_obj_get_child(toggle, 0);
    LS_EQ_STR(text_of(caption), "   ");
    ls_ui_toggle_set(toggle, true);
    LS_EQ_STR(text_of(caption), "[X]");
    ls_ui_toggle_set(toggle, false);
    LS_EQ_STR(text_of(caption), "   ");
}

/* the row name must never wrap. */

static void name_stays_on_one_line(const char *name, const char *value,
                                   const char *const *buttons, int count)
{
    lv_obj_t *root = fresh_root();
    /* Narrow, so a width expressed as a share of the parent is small enough
     * to force the wrap this is guarding against. A full-width root hides the
     * defect: the first version of this test passed against the broken code. */
    lv_obj_set_width(root, 240);
    ls_ui_value_t row;
    ls_ui_value(root, name, &row);
    lv_label_set_text(row.value, value);
    for (int i = 0; i < count; i++)
        ls_ui_button(row.controls, buttons[i], LS_BTN_DEFAULT, nullptr,
                     nullptr, nullptr);

    lv_obj_update_layout(root);

    const lv_font_t *font = lv_obj_get_style_text_font(row.name, LV_PART_MAIN);
    lv_coord_t line = lv_font_get_line_height(font);
    /* One line, allowing for the label's own vertical padding. */
    LS_CHECK(lv_obj_get_height(row.name) <= line + 4);

    LS_CHECK(lv_obj_get_width(row.name) > line);
    LS_CHECK(inside(row.name, row.row));
}

LS_CASE(row_name_never_wraps_beside_a_long_value)
{
    static const char *cycle[] = {"CYCLE"};
    static const char *next[]  = {"NEXT"};
    static const char *freq[]  = {"-1M", "-25k", "+25k", "+1M"};
    static const char *none[]  = {""};

    name_stays_on_one_line("BAUD", "AUTO last 1200", cycle, 1);
    name_stays_on_one_line("BAND", "VHF HI 150-162", next, 1);
    name_stays_on_one_line("FREQUENCY", "152.6000 MHz", freq, 4);
    /* The longest name in the tree, and one with no controls at all. */
    name_stays_on_one_line("PREFERENCE WRITE", "queued", none, 0);
    name_stays_on_one_line("GAIN", "AGC", none, 0);
}
