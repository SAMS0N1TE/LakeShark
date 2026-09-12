

extern "C" {
#include "ls_test.h"
#include "p25_tabs.h"
#include "sdr_ui/sdr_ui.h"
#include "ui/ls_spectrum_waterfall.h"
#include "ui/ls_ui.h"
}

#include <cstdint>
#include <cstring>

/* ------------------------------------------------------------ sdr_ui stubs */
/* Behaviour-identical to components/apps/sdr_ui/sdr_ui.cpp for everything that
 * has a size: same fonts, same 50 px control height, same paddings, same
 * letter spacing.  Only the theme colours and the pressed-state style are left
 * out, and neither moves a pixel. */

LV_FONT_DECLARE(lv_font_lsmono_16);

extern "C" const lv_font_t *sdr_font_mono(void)    { return &lv_font_lsmono_16; }
extern "C" const lv_font_t *sdr_font_mono_sm(void) { return &lv_font_lsmono_16; }
extern "C" const lv_font_t *sdr_font_ui(void)      { return &lv_font_lsmono_16; }

extern "C" lv_obj_t *sdr_label(lv_obj_t *parent, const lv_font_t *font,
                               lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font ? font : LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(label, color, 0);
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
    lv_obj_set_style_text_font(label, sdr_font_mono_sm(), 0);
    lv_obj_set_style_text_letter_space(label, 1, 0);
    lv_label_set_text(label, text ? text : "");
    lv_obj_center(label);
    if (out_label) *out_label = label;
    return button;
}

extern "C" lv_obj_t *sdr_lcd_panel(lv_obj_t *parent, lv_color_t edge)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_width(panel, lv_pct(100));
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_style_border_color(panel, edge, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_outline_width(panel, 3, 0);
    lv_obj_set_style_radius(panel, 3, 0);
    lv_obj_set_style_pad_all(panel, 9, 0);
    lv_obj_set_style_pad_row(panel, 3, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

extern "C" void sdr_style_tabview(lv_obj_t *) {}

extern "C" void sdr_text_if_changed(lv_obj_t *label, const char *text)
{
    if (!label || !text) return;
    const char *old = lv_label_get_text(label);
    if (!old || std::strcmp(old, text) != 0) lv_label_set_text(label, text);
}

/* ------------------------------------------------------------- host display */

struct HostPanel {
    const char *name;
    int hor;
    int ver;
    lv_disp_draw_buf_t draw;
    lv_disp_drv_t drv;
    lv_disp_t *disp;
    lv_color_t buffer[720 * 12];
};

static HostPanel s_panel[6] = {
    { "480x800 (p4-touch-lcd-43)", 480, 800, {}, {}, nullptr, {} },
    { "720x720 (p4-touch-lcd-4b)", 720, 720, {}, {}, nullptr, {} },
    { "568x1232 (T-Display portrait)", 568, 1232, {}, {}, nullptr, {} },
    { "1232x568 (T-Display landscape)", 1232, 568, {}, {}, nullptr, {} },
    { "520x1168 (T-Display portrait safe area)", 520, 1168, {}, {}, nullptr, {} },
    { "1184x504 (T-Display landscape safe area)", 1184, 504, {}, {}, nullptr, {} },
};

static uint32_t s_flush_pixels;
static uint32_t s_pixel_hash;

static void flush_cb(lv_disp_drv_t *driver, const lv_area_t *area,
                     lv_color_t *pixels)
{
    const uint32_t count = (uint32_t)lv_area_get_width(area) *
                           (uint32_t)lv_area_get_height(area);
    s_flush_pixels += count;
    for (uint32_t i = 0; i < count; ++i) {
        s_pixel_hash ^= pixels[i].full;
        s_pixel_hash *= 16777619u;
    }
    lv_disp_flush_ready(driver);
}

/* The window an app is handed: LsShell::begin() places its content container
 * at y=SDR_STATUS_H with height ver - SDR_RAIL_H - SDR_STATUS_H, then gives
 * each app a 100%/100% child of it (LsShell::containerFor). */
static lv_obj_t *shell_content(HostPanel *panel)
{
    static bool inited;
    if (!inited) { lv_init(); inited = true; }

    if (!panel->disp) {
        lv_disp_draw_buf_init(&panel->draw, panel->buffer, nullptr,
                              sizeof(panel->buffer) / sizeof(panel->buffer[0]));
        lv_disp_drv_init(&panel->drv);
        panel->drv.hor_res = (lv_coord_t)panel->hor;
        panel->drv.ver_res = (lv_coord_t)panel->ver;
        panel->drv.draw_buf = &panel->draw;
        panel->drv.flush_cb = flush_cb;
        panel->disp = lv_disp_drv_register(&panel->drv);
    }
    lv_disp_set_default(panel->disp);

    lv_obj_t *screen = lv_disp_get_scr_act(panel->disp);
    lv_obj_clean(screen);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *content = lv_obj_create(screen);
    lv_obj_set_pos(content, 0, SDR_STATUS_H);
    lv_obj_set_size(content, panel->hor,
                    panel->ver - SDR_RAIL_H - SDR_STATUS_H);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *container = lv_obj_create(content);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_size(container, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_radius(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    return container;
}

static void render(HostPanel *panel)
{
    s_flush_pixels = 0;
    s_pixel_hash = 2166136261u;
    lv_obj_invalidate(lv_disp_get_scr_act(panel->disp));
    lv_refr_now(panel->disp);
    LS_CHECK(s_flush_pixels >= (uint32_t)panel->hor * (uint32_t)panel->ver);
    LS_CHECK(s_pixel_hash != 2166136261u);
}

/* ------------------------------------------------------------- measurements */

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

/* Nothing anywhere under `root` may be wider than the box it sits in.  This is
 * the "SCAN controls extend past the right edge" report as an assertion, and
 * it holds for a scrolled subtree too: none of these containers scroll
 * horizontally, so a child crossing its parent's left or right edge is a
 * defect however deep it is. */
static void check_no_horizontal_overflow(lv_obj_t *root, const char *what)
{
    const uint32_t count = lv_obj_get_child_cnt(root);
    const lv_area_t parent = coords(root);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t *child = lv_obj_get_child(root, (int32_t)i);
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) continue;
        const lv_area_t area = coords(child);
        LS_CHECK_MSG(area.x1 >= parent.x1 && area.x2 <= parent.x2,
                     "%s: child %u spans x %d..%d inside %d..%d",
                     what, (unsigned)i, (int)area.x1, (int)area.x2,
                     (int)parent.x1, (int)parent.x2);
        check_no_horizontal_overflow(child, what);
    }
}

static void check_children_disjoint(lv_obj_t *parent, const char *what)
{
    const uint32_t count = lv_obj_get_child_cnt(parent);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t *a = lv_obj_get_child(parent, (int32_t)i);
        LS_CHECK_MSG(inside(a, parent), "%s: child %u escapes its parent",
                     what, (unsigned)i);
        for (uint32_t j = i + 1; j < count; ++j)
            LS_CHECK_MSG(!overlaps(a, lv_obj_get_child(parent, (int32_t)j)),
                         "%s: children %u and %u overlap",
                         what, (unsigned)i, (unsigned)j);
    }
}

/* A button whose label is wider than the button is the tab-overlap defect:
 * LVGL does not clip an unbounded label to its parent, it paints it across the
 * neighbour. */
static void check_label_fits(lv_obj_t *button, const char *what)
{
    lv_obj_t *label = lv_obj_get_child(button, 0);
    if (!label) return;
    LS_CHECK_MSG(inside(label, button),
                 "%s: label [%s] is %d px in a %d px button",
                 what, lv_label_get_text(label),
                 (int)lv_obj_get_width(label), (int)lv_obj_get_width(button));
}

/* ------------------------------------------------------------ the P25 screen */

static void build_p25_tabs(lv_obj_t *container, ls_ui_screen_t *screen,
                           lv_obj_t *tab[P25_TAB_COUNT])
{
    static const char *const names[P25_TAB_COUNT] = P25_TAB_NAMES;
    /* NULL name: AppP25::run passes no title, so no second status strip. */
    ls_ui_screen_create(container, nullptr, true, LS_UI_COLOR_ID_RED, screen);
    for (int i = 0; i < P25_TAB_COUNT; ++i)
        tab[i] = ls_ui_screen_add_tab(screen, names[i]);
}

/* Enough of the DECODE readouts to be taller than the tab: the point of the
 * case is that the action row survives a body that overflows, not that this
 * particular body is the app's. */
static void build_decode_body(lv_obj_t *body)
{
    lv_obj_t *face = sdr_lcd_panel(body, LS_UI_ALARM);
    lv_obj_t *strap = lv_obj_create(face);
    lv_obj_set_size(strap, lv_pct(100), LV_SIZE_CONTENT);
    ls_ui_style_lcd_row(strap);
    lv_obj_set_flex_flow(strap, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(strap, LV_OBJ_FLAG_SCROLLABLE);
    lv_label_set_text(sdr_label(strap, sdr_font_mono(), LS_UI_TEXT), "C4FM");
    ls_ui_lamp(strap, LS_UI_COLOR_ACCENT);

    lv_obj_t *freq = sdr_label(face, sdr_font_mono(), LS_UI_TEXT);
    lv_obj_set_width(freq, lv_pct(100));
    lv_label_set_text(freq, "154.785000");
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *meter = sdr_label(face, sdr_font_mono(), LS_UI_TEXT);
        lv_obj_set_width(meter, lv_pct(100));
        lv_label_set_text(meter, "S [############........]  62%");
    }
    /* Keep adding readout panels until the body genuinely overflows.  The
     * claim under test is that the action row survives a body taller than the
     * space it has, so the body has to be too tall on whichever panel is being
     * measured, not at some count that happens to overflow on one of them. */
    for (int i = 0; i < 40; ++i) {
        lv_obj_t *panel = ls_ui_panel(body, nullptr);
        lv_obj_t *text = sdr_label(panel, sdr_font_mono(), LS_UI_TEXT);
        lv_obj_set_width(text, lv_pct(100));
        lv_label_set_text(text,
            "TRUNK CONTROL  TG 1234  SRC 9876543  154.7850 MHz  IDEN 3\n"
            "SYSTEM WACN 0xBEE00  RFSS 1  SITE 4  NAC 0x293");
        lv_obj_update_layout(body);
        if (lv_obj_get_scroll_bottom(body) > 0) break;
    }
}

/* ------------------------------------------------------------------- cases */

static void tab_strip_case(HostPanel *panel)
{
    lv_obj_t *container = shell_content(panel);
    ls_ui_screen_t screen;
    lv_obj_t *tab[P25_TAB_COUNT];
    build_p25_tabs(container, &screen, tab);
    lv_obj_update_layout(container);

    LS_CHECK_MSG(screen.header == nullptr,
                 "%s: the app still builds its own header strip", panel->name);
    LS_CHECK(screen.tabbar != nullptr);
    LS_EQ_UINT(lv_obj_get_child_cnt(screen.tabbar), P25_TAB_COUNT);

    /* lv_tabview's own button matrix is the thing that could not fit; it must
     * be out of the layout, not merely restyled. */
    lv_obj_t *btns = lv_tabview_get_tab_btns(screen.tabs);
    LS_CHECK(btns != nullptr);
    LS_CHECK(lv_obj_has_flag(btns, LV_OBJ_FLAG_HIDDEN));

    check_children_disjoint(screen.tabbar, "tab strip");
    for (int i = 0; i < P25_TAB_COUNT; ++i)
        check_label_fits(lv_obj_get_child(screen.tabbar, i), "tab");
    check_no_horizontal_overflow(screen.tabbar, "tab strip");
    LS_CHECK(inside(screen.tabbar, screen.root));

    /* Tapping a tab must move the tabview and the highlight together. */
    lv_event_send(lv_obj_get_child(screen.tabbar, P25_TAB_GROUPS),
                  LV_EVENT_CLICKED, nullptr);
    LS_EQ_UINT(lv_tabview_get_tab_act(screen.tabs), P25_TAB_GROUPS);
    lv_obj_update_layout(container);

    /* And a switch driven from the app - switchTab, or the SWEEP auto-jump in
     * the other apps - must move the highlight back. */
    lv_tabview_set_act(screen.tabs, P25_TAB_SCAN, LV_ANIM_OFF);
    LS_EQ_UINT(lv_tabview_get_tab_act(screen.tabs), P25_TAB_SCAN);

    int rows = 1;
    lv_coord_t line_y = coords(lv_obj_get_child(screen.tabbar, 0)).y1;
    for (int i = 1; i < P25_TAB_COUNT; ++i) {
        lv_coord_t y = coords(lv_obj_get_child(screen.tabbar, i)).y1;
        if (y != line_y) { ++rows; line_y = y; }
    }
    ls_note("%s: %d tabs on %d row(s), strip %d px",
            panel->name, P25_TAB_COUNT, rows,
            (int)lv_obj_get_height(screen.tabbar));
    render(panel);
}

LS_CASE(p25_tab_labels_fit_and_do_not_overlap_on_480x800)
{
    tab_strip_case(&s_panel[0]);
}

LS_CASE(p25_tab_labels_fit_and_do_not_overlap_on_720x720)
{
    tab_strip_case(&s_panel[1]);
}

static void decode_actions_case(HostPanel *panel)
{
    lv_obj_t *container = shell_content(panel);
    ls_ui_screen_t screen;
    lv_obj_t *tab[P25_TAB_COUNT];
    build_p25_tabs(container, &screen, tab);

    ls_ui_split_t split;
    ls_ui_tab_split(tab[P25_TAB_DECODE], &split);
    build_decode_body(split.body);

    static const char *const action[P25_DECODE_ACTION_COUNT] =
        P25_DECODE_ACTIONS;
    lv_obj_t *button[P25_DECODE_ACTION_COUNT];
    for (int i = 0; i < P25_DECODE_ACTION_COUNT; ++i)
        button[i] = ls_ui_button(split.actions, action[i], LS_BTN_DEFAULT,
                                 nullptr, nullptr, nullptr);
    lv_obj_update_layout(container);

    /* The body is meant to be taller than the space it has - otherwise the
     * case proves nothing about scrolling. */
    LS_CHECK_MSG(lv_obj_get_scroll_bottom(split.body) > 0,
                 "%s: the modelled DECODE body fits, so the case is not "
                 "measuring what it claims", panel->name);

    /* And the actions are still on screen, unscrolled, inside the window the
     * shell gave the app. */
    LS_CHECK_MSG(inside(split.actions, tab[P25_TAB_DECODE]),
                 "%s: DECODE actions are outside the tab", panel->name);
    LS_CHECK_MSG(inside(split.actions, container),
                 "%s: DECODE actions are outside the shell content area",
                 panel->name);
    LS_CHECK(coords(split.actions).y1 >= coords(split.body).y2);
    LS_EQ_INT(lv_obj_get_scroll_bottom(tab[P25_TAB_DECODE]), 0);

    check_children_disjoint(split.actions, "DECODE actions");
    for (int i = 0; i < P25_DECODE_ACTION_COUNT; ++i)
        check_label_fits(button[i], "DECODE action");
    check_no_horizontal_overflow(tab[P25_TAB_DECODE], "DECODE");

    /* updateDecode rewrites three of these labels while a call is running.
     * The widest form is the one that has to fit. */
    static const char *const wide[P25_DECODE_ACTION_COUNT] =
        P25_DECODE_ACTIONS_WIDE;
    for (int i = 0; i < P25_DECODE_ACTION_COUNT; ++i)
        lv_label_set_text(lv_obj_get_child(button[i], 0), wide[i]);
    lv_obj_update_layout(container);

    LS_CHECK_MSG(inside(split.actions, tab[P25_TAB_DECODE]),
                 "%s: DECODE actions leave the tab once HOLD names a talkgroup",
                 panel->name);
    check_children_disjoint(split.actions, "DECODE actions (live labels)");
    for (int i = 0; i < P25_DECODE_ACTION_COUNT; ++i)
        check_label_fits(button[i], "DECODE action (live label)");
    check_no_horizontal_overflow(tab[P25_TAB_DECODE], "DECODE (live labels)");

    ls_note("%s: DECODE actions %d px tall at y=%d, tab ends at %d",
            panel->name, (int)lv_obj_get_height(split.actions),
            (int)coords(split.actions).y1,
            (int)coords(tab[P25_TAB_DECODE]).y2);
    render(panel);
}

LS_CASE(p25_decode_actions_stay_visible_without_scrolling_on_480x800)
{
    decode_actions_case(&s_panel[0]);
}

LS_CASE(p25_decode_actions_stay_visible_without_scrolling_on_720x720)
{
    decode_actions_case(&s_panel[1]);
}

static void signal_case(HostPanel *panel)
{
    lv_obj_t *container = shell_content(panel);
    ls_ui_screen_t screen;
    lv_obj_t *tab[P25_TAB_COUNT];
    build_p25_tabs(container, &screen, tab);

    lv_obj_t *page = tab[P25_TAB_SIGNAL];
    lv_obj_t *status = ls_ui_panel(page, "RECEPTION");
    static const char *const lines[4] = {
        "MODE C4FM    MOD C4FM    NAC 0x293",
        "ACTUAL 154.785000 MHz   span 24 kHz   RF filter AUTO   CONTROL",
        "PEAK 154.787500 MHz   71%   tap spectrum to tune",
        "INPUT  [############........]  62%",
    };
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *label = sdr_label(status, sdr_font_mono(), LS_UI_TEXT);
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_text(label, lines[i]);
    }

    /* The same two-pass sizing buildSignalTab does: seed, lay the widget's own
     * controls out, then grow into the measured remainder. */
    const int seed_h = 64;
    lv_obj_update_layout(page);
    int plot_w = lv_obj_get_content_width(page);
    int points = plot_w / 2;
    if (points < 32) points = 32;
    if (points > 240) points = 240;

    ls_spectrum_waterfall_t view = {};
    LS_CHECK(ls_spectrum_waterfall_build(&view, page,
                                         "SPECTRUM / WATERFALL  (tap to tune)",
                                         plot_w, seed_h, points, 50, 100,
                                         false, nullptr, nullptr));
    ls_spectrum_waterfall_add_controls(
        &view,
        LS_SPECTRUM_CTL_SPLIT | LS_SPECTRUM_CTL_CONTRAST |
            LS_SPECTRUM_CTL_FULL | LS_SPECTRUM_CTL_GAIN,
        [](lv_event_t *) {}, [](lv_event_t *) {}, nullptr, nullptr, nullptr);

    ls_spectrum_waterfall_fit_page(&view,status);
    int plot_h=view.height;
    lv_obj_update_layout(container);

    LS_CHECK_MSG(plot_h > seed_h,
                 "%s: no room measured for the spectrum at all", panel->name);
    LS_CHECK_MSG(lv_obj_get_scroll_bottom(page) <= 0,
                 "%s: SIGNAL still needs %d px of scrolling to reach its "
                 "controls", panel->name, (int)lv_obj_get_scroll_bottom(page));
    LS_CHECK(inside(view.panel, page));
    check_no_horizontal_overflow(page, "SIGNAL");

    ls_note("%s: SIGNAL plot %dx%d, panel ends at %d, tab ends at %d",
            panel->name, view.width, plot_h,
            (int)coords(view.panel).y2, (int)coords(page).y2);

    render(panel);
    ls_spectrum_waterfall_forget(&view);
}

LS_CASE(p25_signal_controls_fit_without_scrolling_on_480x800)
{
    signal_case(&s_panel[0]);
}

LS_CASE(p25_signal_controls_fit_without_scrolling_on_720x720)
{
    signal_case(&s_panel[1]);
}

/* The SCAN and CONFIG row shapes, built from the same kit calls the app and
 * ScanPanel now make.  Both tabs are long lists that legitimately scroll; what
 * must hold is that no control crosses the right edge and no two controls in
 * a row share space. */
static lv_obj_t *value_with_stepper(lv_obj_t *parent, const char *name,
                                    const char *value)
{
    ls_ui_value_t row;
    ls_ui_value(parent, name, &row);
    lv_label_set_text(row.value, value);
    ls_ui_stepper(&row, nullptr, nullptr, nullptr, nullptr);
    return row.row;
}

static lv_obj_t *value_with_group(lv_obj_t *parent, const char *name,
                                  const char *value,
                                  const char *const *labels, int count)
{
    ls_ui_value_t row;
    ls_ui_value(parent, name, &row);
    lv_label_set_text(row.value, value);
    lv_obj_t *group = ls_ui_button_group(row.controls);
    for (int i = 0; i < count; ++i)
        ls_ui_group_button(group, labels[i], LS_BTN_DEFAULT, nullptr, nullptr,
                           nullptr);
    return row.row;
}

static void scan_case(HostPanel *panel)
{
    lv_obj_t *container = shell_content(panel);
    ls_ui_screen_t screen;
    lv_obj_t *tab[P25_TAB_COUNT];
    build_p25_tabs(container, &screen, tab);

    lv_obj_t *page = tab[P25_TAB_SCAN];
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_t *body = ls_ui_panel(page, nullptr);

    lv_obj_t *rows[12];
    int n = 0;

    lv_obj_t *status_panel = ls_ui_panel(body, "CARRIER SCAN STATUS");
    lv_obj_t *status = sdr_label(status_panel, sdr_font_mono(), LS_UI_ACCENT);
    lv_obj_set_width(status, lv_pct(100));
    lv_label_set_text(status,
        "band 150.0000-162.0000 step 12.5 kHz  960 steps  holding 154.7850");

    static const char *const scan_skip[2] = { "SCAN", "SKIP" };
    rows[n++] = value_with_group(body, "CARRIER SCAN", "", scan_skip, 2);
    static const char *const source[1] = { "PRESET/BAND" };
    rows[n++] = value_with_group(body, "SOURCE", "BAND  960 steps", source, 1);
    rows[n++] = value_with_stepper(body, "RANGE",
                                   "FIRE/EMS 154-155  154.0000-155.0000");
    rows[n++] = value_with_stepper(body, "STEP", "12.5 kHz  (80 steps)");
    rows[n++] = value_with_stepper(body, "NUDGE", "100 kHz");
    rows[n++] = value_with_stepper(body, "START", "154.0000");
    rows[n++] = value_with_stepper(body, "STOP", "155.0000");
    static const char *const autosq[1] = { "AUTO SQ" };
    rows[n++] = value_with_group(body, "NFM SQUELCH", "sq=15   floor=9",
                                 autosq, 1);
    rows[n++] = value_with_stepper(body, "HANG", "2.0 s");
    rows[n++] = value_with_stepper(body, "ZONE", "1/4  DISPATCH");
    static const char *const channel[4] = { "ADD", "NAME", "LOCK", "DEL" };
    LS_CHECK(n < (int)(sizeof(rows) / sizeof(rows[0])));
    value_with_group(body, "CHANNEL", "12/64  FIRE DISPATCH", channel, 4);

    lv_obj_update_layout(container);

    check_no_horizontal_overflow(page, "SCAN");

    for (int i = 0; i < n; ++i) {
        check_children_disjoint(rows[i], "SCAN row");
        const uint32_t count = lv_obj_get_child_cnt(rows[i]);
        for (uint32_t c = 0; c < count; ++c) {
            lv_obj_t *item = lv_obj_get_child(rows[i], (int32_t)c);
            check_children_disjoint(item, "SCAN control group");
            check_label_fits(item, "SCAN control");
            for (uint32_t b = 0; b < lv_obj_get_child_cnt(item); ++b)
                check_label_fits(lv_obj_get_child(item, (int32_t)b),
                                 "SCAN control");
        }
    }
    render(panel);
}

LS_CASE(p25_scan_controls_stay_inside_the_row_on_480x800)
{
    scan_case(&s_panel[0]);
}

LS_CASE(p25_scan_controls_stay_inside_the_row_on_720x720)
{
    scan_case(&s_panel[1]);
}

static void config_case(HostPanel *panel)
{
    lv_obj_t *container = shell_content(panel);
    ls_ui_screen_t screen;
    lv_obj_t *tab[P25_TAB_COUNT];
    build_p25_tabs(container, &screen, tab);

    lv_obj_t *page = tab[P25_TAB_CONFIG];
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_t *body = ls_ui_panel(page, nullptr);

    ls_ui_value_t row;
    ls_ui_value(body, "USB AUTO-REBOOT", &row);
    lv_label_set_text(row.value, "OFF");
    lv_obj_t *toggle = ls_ui_button(row.controls, "TOGGLE", LS_BTN_TOGGLE_OFF,
                                    nullptr, nullptr, nullptr);

    ls_ui_value_t freq;
    ls_ui_value(body, "FREQUENCY", &freq);
    lv_label_set_text(freq.value, "154.785000 MHz");
    ls_ui_button(freq.controls, "ENTER", LS_BTN_PRIMARY, nullptr, nullptr,
                 nullptr);
    static const char *const nudge[4] = { "-1M", "-25k", "+25k", "+1M" };
    lv_obj_t *group = ls_ui_button_group(freq.controls);
    for (int i = 0; i < 4; ++i)
        ls_ui_group_button(group, nudge[i], LS_BTN_DEFAULT, nullptr, nullptr,
                           nullptr);

    lv_obj_update_layout(container);

    LS_CHECK(inside(toggle, row.row));
    LS_CHECK(lv_obj_get_width(toggle) >= 56);
    LS_CHECK(lv_obj_get_height(toggle) >= 44);
    LS_CHECK(lv_obj_has_flag(toggle, LV_OBJ_FLAG_CLICKABLE));
    check_label_fits(toggle, "USB AUTO-REBOOT toggle");
    LS_CHECK(!overlaps(toggle, row.value));

    check_children_disjoint(group, "FREQUENCY nudges");
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(group); ++i)
        check_label_fits(lv_obj_get_child(group, (int32_t)i), "FREQUENCY nudge");
    check_no_horizontal_overflow(page, "CONFIG");

    /* and the form that replaces it.  The caption names the setting
     * and does not change; the state is a marker glyph and an outline, so the
     * value column is free for a setting with more than two choices.  Measured
     * with the real consolas face: the marker is the same width in both
     * states, so pressing the control cannot re-wrap the row it sits in. */
    ls_ui_value_t beep;
    ls_ui_value(body, "SYNC BEEP", &beep);
    lv_obj_t *stateful = ls_ui_toggle(&beep, "SYNC BEEP", false, nullptr, nullptr);
    LS_CHECK(stateful != nullptr);
    lv_obj_update_layout(container);
    const lv_coord_t off_w = lv_obj_get_width(stateful);
    const lv_coord_t off_h = lv_obj_get_height(stateful);
    LS_EQ_INT(lv_obj_get_style_outline_width(stateful, LV_PART_MAIN), 0);
    check_label_fits(stateful, "SYNC BEEP toggle (off)");

    ls_ui_toggle_set(stateful, true);
    lv_obj_update_layout(container);
    LS_CHECK_MSG(lv_obj_get_style_outline_width(stateful, LV_PART_MAIN) > 0,
                 "%s: an engaged toggle is distinguished by colour alone",
                 panel->name);
    LS_CHECK_MSG(lv_obj_get_width(stateful) == off_w &&
                     lv_obj_get_height(stateful) == off_h,
                 "%s: toggling resized the control from %dx%d to %dx%d",
                 panel->name, (int)off_w, (int)off_h,
                 (int)lv_obj_get_width(stateful),
                 (int)lv_obj_get_height(stateful));
    LS_EQ_STR(lv_label_get_text(beep.value), "");
    check_label_fits(stateful, "SYNC BEEP toggle (on)");
    check_children_disjoint(beep.row, "SYNC BEEP row");
    check_no_horizontal_overflow(page, "CONFIG (toggle engaged)");

    ls_note("%s: one-control row %d px tall, %d px button, value column free",
            panel->name, (int)lv_obj_get_height(row.row),
            (int)lv_obj_get_height(toggle));
    render(panel);
}

LS_CASE(p25_config_toggle_is_a_control_not_a_status_line_on_480x800)
{
    config_case(&s_panel[0]);
}

LS_CASE(p25_config_toggle_is_a_control_not_a_status_line_on_720x720)
{
    config_case(&s_panel[1]);
}

LS_CASE(tdisplay_portrait_keeps_shared_p25_controls_readable)
{
    tab_strip_case(&s_panel[2]);
    decode_actions_case(&s_panel[2]);
    signal_case(&s_panel[2]);
    scan_case(&s_panel[2]);
    config_case(&s_panel[2]);
}
LS_CASE(tdisplay_landscape_keeps_shared_p25_controls_readable)
{
    tab_strip_case(&s_panel[3]);
    decode_actions_case(&s_panel[3]);
    signal_case(&s_panel[3]);
    scan_case(&s_panel[3]);
    config_case(&s_panel[3]);
}
LS_CASE(tdisplay_rounded_corners_leave_all_p25_controls_accessible)
{
    for(int i=4;i<6;++i) {
        tab_strip_case(&s_panel[i]); decode_actions_case(&s_panel[i]);
        signal_case(&s_panel[i]); scan_case(&s_panel[i]); config_case(&s_panel[i]);
    }
}
