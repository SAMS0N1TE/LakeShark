

extern "C" {
#include "ls_test.h"
#include "ls_lv_mem_probe.h"
#include "sdr_ui/sdr_ui.h"
#include "ui/ls_ui.h"
}

#include <cstdint>
#include <cstring>

LS_CASE(allocation_probe_rejects_header_overflow_without_losing_live_block)
{
    const size_t bytes = ls_lv_probe_live_bytes();
    const size_t blocks = ls_lv_probe_live_blocks();
    auto *p = static_cast<unsigned char *>(ls_lv_probe_malloc(8));
    LS_CHECK(p != nullptr);
    if (!p) return;
    p[0] = 77;
    LS_CHECK(ls_lv_probe_malloc(SIZE_MAX) == nullptr);
    LS_CHECK(ls_lv_probe_realloc(p, SIZE_MAX) == nullptr);
    LS_EQ_UINT(p[0], 77);
    LS_EQ_UINT(ls_lv_probe_live_bytes(), bytes + 8);
    LS_EQ_UINT(ls_lv_probe_live_blocks(), blocks + 1);
    ls_lv_probe_free(p);
    LS_EQ_UINT(ls_lv_probe_live_bytes(), bytes);
    LS_EQ_UINT(ls_lv_probe_live_blocks(), blocks);
}

/* --------------------------------------------------------------- sdr stubs */
/* Same fonts and the same 50 px control metrics as the firmware's sdr_ui, so a
 * label allocates here what it allocates on the board. */

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

extern "C" void sdr_style_tabview(lv_obj_t *) {}

extern "C" void sdr_text_if_changed(lv_obj_t *label, const char *text)
{
    if (!label || !text) return;
    const char *old = lv_label_get_text(label);
    if (!old || std::strcmp(old, text) != 0) lv_label_set_text(label, text);
}

/* ------------------------------------------------------------- host display */

static lv_disp_t *s_display;
static lv_color_t s_draw_buffer[480 * 12];
static lv_disp_draw_buf_t s_draw;
static lv_disp_drv_t s_driver;

static void flush_cb(lv_disp_drv_t *driver, const lv_area_t *, lv_color_t *)
{
    lv_disp_flush_ready(driver);
}

static void host_display(void)
{
    if (s_display) return;
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

static lv_obj_t *fresh_root(void)
{
    host_display();
    lv_obj_t *screen = lv_disp_get_scr_act(s_display);
    lv_obj_clean(screen);
    lv_obj_set_style_pad_all(screen, 0, 0);

    lv_obj_t *root = lv_obj_create(screen);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(root, 6, 0);
    lv_obj_set_style_pad_row(root, 6, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(root, LV_DIR_VER);
    return root;
}

struct Usage {
    size_t bytes;
    size_t blocks;
};

static Usage settle(void)
{
    lv_mem_buf_free_all();
    Usage u;
    u.bytes = ls_lv_probe_live_bytes();
    u.blocks = ls_lv_probe_live_blocks();
    return u;
}

/* ------------------------------------------- the shape, as it shipped */

struct LegacyRow {
    lv_obj_t *row;
    lv_obj_t *name;
    lv_obj_t *value;
    lv_obj_t *controls;
};

static void legacy_value(lv_obj_t *parent, const char *name, LegacyRow *out)
{
    lv_obj_t *row = ls_ui_row(parent, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_color(row, LS_UI_PANEL, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(row, LS_UI_PANEL_BORDER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_radius(row, 2, 0);
    lv_obj_set_style_pad_hor(row, 10, 0);
    lv_obj_set_style_pad_ver(row, 6, 0);
    lv_obj_set_style_pad_row(row, 4, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *heading = ls_ui_row(row, LV_FLEX_ALIGN_START);
    lv_obj_set_width(heading, lv_pct(100));
    lv_obj_set_style_min_width(heading, 0, 0);

    lv_obj_t *name_label = sdr_label(heading, sdr_font_mono_sm(), LS_UI_DIM_TEXT);
    lv_obj_set_width(name_label, 0);
    lv_obj_set_style_min_width(name_label, 0, 0);
    lv_obj_set_style_text_letter_space(name_label, 1, 0);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(name_label, name ? name : "");
    lv_obj_set_flex_grow(name_label, 1);

    lv_obj_t *value_label = sdr_label(heading, sdr_font_mono(), LS_UI_TEXT);
    lv_obj_set_width(value_label, lv_pct(60));
    lv_obj_set_style_min_width(value_label, 0, 0);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(value_label, "");
    lv_label_set_long_mode(value_label, LV_LABEL_LONG_CLIP);

    lv_obj_t *controls = ls_ui_row(row, LV_FLEX_ALIGN_END);
    lv_obj_set_width(controls, lv_pct(100));
    lv_obj_set_style_min_width(controls, 0, 0);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(controls, 4, 0);
    lv_obj_set_style_pad_column(controls, 6, 0);

    out->row = row;
    out->name = name_label;
    out->value = value_label;
    out->controls = controls;
}

static void legacy_stepper(LegacyRow *row)
{
    lv_obj_t *group = ls_ui_row(row->controls, LV_FLEX_ALIGN_END);
    lv_obj_set_width(group, lv_pct(100));
    lv_obj_set_style_min_width(group, 0, 0);
    lv_obj_set_style_pad_column(group, 6, 0);
    static const char *const glyph[2] = { "<", ">" };
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *button = ls_ui_button(group, glyph[i], LS_BTN_DEFAULT,
                                        nullptr, nullptr, nullptr);
        lv_obj_set_width(button, LV_SIZE_CONTENT);
        lv_obj_set_style_min_width(button, 64, 0);
    }
}

/* ------------------------------------------------- the rows a P25 tab builds */
/* One of each shape the settings pages actually use, with the strings they
 * actually carry, so the measurement exercises the small font, the mono font,
 * a value long enough to be clipped, and rows carrying one, two and four
 * controls. */

enum Shape {
    SHAPE_READOUT = 0,   /* value only, no control at all */
    SHAPE_TOGGLE,
    SHAPE_PAIR,          /* two buttons, e.g. GAIN: STEP and AGC */
    SHAPE_STEPPER,       /* prev/next */
    SHAPE_GROUP,         /* four equal-width nudges on one line */
    SHAPE_COUNT
};

static const char *const SHAPE_NAME[SHAPE_COUNT] = {
    "PREFERENCE WRITE", "USB AUTO-REBOOT", "GAIN", "ENCRYPTED SKIP",
    "FREQUENCY",
};

static const char *const SHAPE_VALUE[SHAPE_COUNT] = {
    "READY",
    "OFF",
    "AGC 42.1 dB",
    "FIRE/EMS 154-155  154.0000-155.0000 deliberately overlong",
    "154.785000 MHz",
};

static void build_kit_row(lv_obj_t *parent, int shape)
{
    ls_ui_value_t r;
    ls_ui_value(parent, SHAPE_NAME[shape], &r);
    lv_label_set_text(r.value, SHAPE_VALUE[shape]);

    switch (shape) {
    case SHAPE_READOUT:
        break;
    case SHAPE_TOGGLE:
        ls_ui_toggle(&r, "AUTO-REBOOT", false, nullptr, nullptr);
        break;
    case SHAPE_PAIR:
        ls_ui_button(r.controls, "STEP", LS_BTN_DEFAULT, nullptr, nullptr, nullptr);
        ls_ui_button(r.controls, "AGC", LS_BTN_TOGGLE_OFF, nullptr, nullptr, nullptr);
        break;
    case SHAPE_STEPPER:
        ls_ui_stepper(&r, nullptr, nullptr, nullptr, nullptr);
        break;
    default: {
        static const char *const nudge[4] = { "-1M", "-25k", "+25k", "+1M" };
        lv_obj_t *group = ls_ui_button_group(r.controls);
        for (int i = 0; i < 4; ++i)
            ls_ui_group_button(group, nudge[i], LS_BTN_DEFAULT, nullptr,
                               nullptr, nullptr);
        break;
    }
    }
}

static void build_legacy_row(lv_obj_t *parent, int shape)
{
    LegacyRow r;
    legacy_value(parent, SHAPE_NAME[shape], &r);
    lv_label_set_text(r.value, SHAPE_VALUE[shape]);

    switch (shape) {
    case SHAPE_READOUT:
        break;
    case SHAPE_TOGGLE:
        ls_ui_button(r.controls, "TOGGLE", LS_BTN_TOGGLE_OFF, nullptr, nullptr,
                     nullptr);
        break;
    case SHAPE_PAIR:
        ls_ui_button(r.controls, "STEP", LS_BTN_DEFAULT, nullptr, nullptr, nullptr);
        ls_ui_button(r.controls, "AGC", LS_BTN_TOGGLE_OFF, nullptr, nullptr, nullptr);
        break;
    case SHAPE_STEPPER:
        legacy_stepper(&r);
        break;
    default: {
        static const char *const nudge[4] = { "-1M", "-25k", "+25k", "+1M" };
        lv_obj_t *group = ls_ui_button_group(r.controls);
        for (int i = 0; i < 4; ++i)
            ls_ui_group_button(group, nudge[i], LS_BTN_DEFAULT, nullptr,
                               nullptr, nullptr);
        break;
    }
    }
}

typedef void (*row_builder_t)(lv_obj_t *, int);

static Usage measure_rows(row_builder_t build, int shape, int count)
{
    lv_obj_t *root = fresh_root();
    lv_obj_update_layout(root);
    const Usage before = settle();

    for (int i = 0; i < count; ++i) build(root, shape);
    lv_obj_update_layout(root);
    const Usage after = settle();

    Usage delta;
    delta.bytes = after.bytes - before.bytes;
    delta.blocks = after.blocks - before.blocks;
    return delta;
}

/* The P25 settings page as a count: how many rows of each shape the eagerly
 * built tabs carry.  Taken from AppP25::buildSettings and ScanPanel, and it
 * comes to the forty-one rows the ELF review attributed to . */
static const int P25_ROWS[SHAPE_COUNT] = { 9, 8, 4, 14, 6 };

LS_CASE(flat_value_row_costs_fewer_bytes_than_the_nested_shape_it_replaces)
{
    static const int COUNT = 8;
    size_t kit_total = 0;
    size_t legacy_total = 0;
    size_t kit_row_total = 0;
    size_t legacy_row_total = 0;

    for (int shape = 0; shape < SHAPE_COUNT; ++shape) {
        const Usage kit = measure_rows(build_kit_row, shape, COUNT);
        const Usage old = measure_rows(build_legacy_row, shape, COUNT);

        const size_t kit_row = kit.bytes / COUNT;
        const size_t old_row = old.bytes / COUNT;

        ls_note("%-18s flat %4u B / %2u blocks   nested %4u B / %2u blocks"
                "   saved %4d B per row",
                SHAPE_NAME[shape],
                (unsigned)kit_row, (unsigned)(kit.blocks / COUNT),
                (unsigned)old_row, (unsigned)(old.blocks / COUNT),
                (int)old_row - (int)kit_row);

        /* Every shape must get cheaper, not just the average: a row that only
           breaks even would mean the wrapper came back somewhere else. */
        LS_CHECK_MSG(kit.bytes < old.bytes,
                     "%s: flat row costs %u B, nested %u B",
                     SHAPE_NAME[shape], (unsigned)kit.bytes,
                     (unsigned)old.bytes);
        /* And it must be two whole objects cheaper - the heading and the
           controls container - not a few bytes of style list. */
        LS_CHECK_MSG(old.blocks - kit.blocks >= 2u * (size_t)COUNT,
                     "%s: only %u fewer blocks across %d rows",
                     SHAPE_NAME[shape],
                     (unsigned)(old.blocks - kit.blocks), COUNT);

        kit_total += kit_row * (size_t)P25_ROWS[shape];
        legacy_total += old_row * (size_t)P25_ROWS[shape];
        kit_row_total += kit_row;
        legacy_row_total += old_row;
    }

    int rows = 0;
    for (int i = 0; i < SHAPE_COUNT; ++i) rows += P25_ROWS[i];
    ls_note("P25 settings rows as built (%d rows): flat %u B, nested %u B, "
            "saved %d B", rows, (unsigned)kit_total, (unsigned)legacy_total,
            (int)legacy_total - (int)kit_total);
    ls_note("mean over the five shapes: flat %u B/row, nested %u B/row",
            (unsigned)(kit_row_total / SHAPE_COUNT),
            (unsigned)(legacy_row_total / SHAPE_COUNT));

    LS_CHECK(kit_total < legacy_total);
}

/* The forty-one rows the ELF review named, built as one page and measured as
   one number - the figure to compare a staged hardware reading against. */
LS_CASE(the_p25_settings_page_is_measurably_smaller_than_it_was)
{
    lv_obj_t *root = fresh_root();
    lv_obj_update_layout(root);
    const Usage before = settle();
    for (int shape = 0; shape < SHAPE_COUNT; ++shape)
        for (int i = 0; i < P25_ROWS[shape]; ++i) build_kit_row(root, shape);
    lv_obj_update_layout(root);
    const Usage kit = settle();

    lv_obj_t *root2 = fresh_root();
    lv_obj_update_layout(root2);
    const Usage before2 = settle();
    for (int shape = 0; shape < SHAPE_COUNT; ++shape)
        for (int i = 0; i < P25_ROWS[shape]; ++i) build_legacy_row(root2, shape);
    lv_obj_update_layout(root2);
    const Usage old = settle();

    const size_t kit_bytes = kit.bytes - before.bytes;
    const size_t old_bytes = old.bytes - before2.bytes;
    ls_note("whole page: flat %u B in %u blocks, nested %u B in %u blocks, "
            "saved %u B and %u allocations",
            (unsigned)kit_bytes, (unsigned)(kit.blocks - before.blocks),
            (unsigned)old_bytes, (unsigned)(old.blocks - before2.blocks),
            (unsigned)(old_bytes - kit_bytes),
            (unsigned)((old.blocks - before2.blocks) -
                       (kit.blocks - before.blocks)));
    LS_CHECK(kit_bytes < old_bytes);
}

/* A screen that is built and closed must give every byte back.  The apps
   rebuild these pages on every open, so a per-row leak here is a per-open leak
   on a board with 215 B of internal headroom. */
LS_CASE(building_and_tearing_down_the_rows_returns_every_byte)
{
    host_display();
    lv_obj_t *screen = lv_disp_get_scr_act(s_display);
    lv_obj_clean(screen);
    const Usage before = settle();

    for (int pass = 0; pass < 3; ++pass) {
        lv_obj_t *root = fresh_root();
        for (int shape = 0; shape < SHAPE_COUNT; ++shape)
            for (int i = 0; i < P25_ROWS[shape]; ++i) build_kit_row(root, shape);
        lv_obj_update_layout(root);
        lv_obj_del(root);
    }

    const Usage after = settle();
    ls_note("three build/teardown passes: %u B and %u blocks live before, "
            "%u B and %u blocks after",
            (unsigned)before.bytes, (unsigned)before.blocks,
            (unsigned)after.bytes, (unsigned)after.blocks);
    LS_EQ_UINT(after.bytes, before.bytes);
    LS_EQ_UINT(after.blocks, before.blocks);
}

/* The row's own overhead must not depend on what the caller puts in it: a long
   value, a different font and a fifth control may each cost what they cost,
   but they must not multiply the row.  This is what stops the wrapper from
   coming back by another name. */
static void build_bare_stepper(lv_obj_t *parent, int)
{
    /* The stepper on its own, with a host that is not a value row, so its cost
       can be subtracted from the row that carries one. */
    lv_obj_t *host = ls_ui_row(parent, LV_FLEX_ALIGN_END);
    ls_ui_value_t fake = { host, nullptr, nullptr, host };
    ls_ui_stepper(&fake, nullptr, nullptr, nullptr, nullptr);
}

LS_CASE(row_overhead_is_the_same_whatever_the_row_carries)
{
    static const int COUNT = 8;
    const Usage bare = measure_rows(build_kit_row, SHAPE_READOUT, COUNT);
    const Usage wide = measure_rows(build_kit_row, SHAPE_STEPPER, COUNT);
    const Usage alone = measure_rows(build_bare_stepper, 0, COUNT);

    /* A stepper row is a row plus a stepper.  `alone` carries one extra host
       container per stepper, so a row that added a wrapper of its own would
       need MORE blocks than the two measured separately, not fewer. */
    const size_t extra_blocks = (wide.blocks - bare.blocks) / (size_t)COUNT;
    const size_t stepper_blocks = alone.blocks / (size_t)COUNT;
    ls_note("bare row %u B / %u blocks; stepper row %u B / %u blocks; "
            "a stepper and its host alone %u blocks",
            (unsigned)(bare.bytes / COUNT), (unsigned)(bare.blocks / COUNT),
            (unsigned)(wide.bytes / COUNT), (unsigned)(wide.blocks / COUNT),
            (unsigned)stepper_blocks);
    LS_CHECK_MSG(extra_blocks < stepper_blocks,
                 "carrying a stepper costs a row %u allocations when the "
                 "stepper and a container of its own cost %u",
                 (unsigned)extra_blocks, (unsigned)stepper_blocks);

    /* And a row with five separate controls costs five controls, not five
       controls plus a line each. */
    lv_obj_t *root = fresh_root();
    lv_obj_update_layout(root);
    const Usage before = settle();
    for (int i = 0; i < COUNT; ++i) {
        ls_ui_value_t r;
        ls_ui_value(root, "FREQUENCY", &r);
        lv_label_set_text(r.value, SHAPE_VALUE[SHAPE_STEPPER]);
        static const char *const label[5] = {"ENTER", "-1M", "-25k", "+25k", "+1M"};
        for (int b = 0; b < 5; ++b)
            ls_ui_button(r.controls, label[b], LS_BTN_DEFAULT, nullptr, nullptr,
                         nullptr);
    }
    lv_obj_update_layout(root);
    const Usage after = settle();
    ls_note("five-control row: %u B / %u blocks",
            (unsigned)((after.bytes - before.bytes) / COUNT),
            (unsigned)((after.blocks - before.blocks) / COUNT));
    LS_CHECK(after.bytes > before.bytes);
}
