#include "ui/ls_spectrum_waterfall.h"

#include <cstdio>
#include <cstdint>
#include <cstring>

#include "esp_heap_caps.h"
#include "ui/ls_ui.h"

static float clamp_level(float value)
{
    return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
}

static float apply_contrast(const ls_spectrum_waterfall_t *view, float value)
{
    float scale = view ? (float)view->contrast_pct / 100.0f : 1.0f;
    return clamp_level((value - 0.5f) * scale + 0.5f);
}

static lv_color_t heat_color(float value)
{
    value = clamp_level(value);
    lv_color_t low, high;
    float local;
    if (value < 0.50f) {
        low = LS_UI_BACKGROUND; high = LS_UI_ACCENT; local = value * 2.0f;
    } else if (value < 0.80f) {
        low = LS_UI_ACCENT; high = LS_UI_WARN;
        local = (value - 0.50f) / 0.30f;
    } else {
        low = LS_UI_WARN; high = LS_UI_ALARM;
        local = (value - 0.80f) / 0.20f;
    }
    return lv_color_mix(high, low, (uint8_t)(local * 255.0f));
}

static void update_labels(ls_spectrum_waterfall_t *view)
{
    char text[24];
    if (view->split_label) {
        snprintf(text, sizeof(text), "%d/%d", view->split_pct,
                 100 - view->split_pct);
        lv_label_set_text(view->split_label, text);
    }
    if (view->contrast_label) {
        snprintf(text, sizeof(text), "%d%%", view->contrast_pct);
        lv_label_set_text(view->contrast_label, text);
    }
    if (view->full_label)
        lv_label_set_text(view->full_label, view->fullscreen ? "EXIT" : "FULL");
    if (view->full_button)
        ls_ui_button_set_role(view->full_button,
            view->fullscreen ? LS_BTN_TOGGLE_ON : LS_BTN_TOGGLE_OFF);
}

static void apply_layout(ls_spectrum_waterfall_t *view)
{
    if (!view) return;
    ls_spectrum_layout_t layout;
    if (!ls_spectrum_layout(view->width, view->height, view->split_pct,
                            view->fullscreen, sizeof(lv_color_t), &layout))
        return;

    if (view->chart) {
        if (layout.spectrum_visible) {
            lv_obj_clear_flag(view->chart, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(view->chart, layout.width, layout.spectrum_height);
        } else {
            lv_obj_add_flag(view->chart, LV_OBJ_FLAG_HIDDEN);
        }
    }
    view->waterfall_height = layout.waterfall_height;
    if (view->canvas) {
        if (layout.waterfall_visible) {
            /* Start the window at the newest row. */
            lv_canvas_set_buffer(view->canvas,
                                 &view->pixels[(size_t)view->wf_top * (size_t)view->width],
                                 layout.width, layout.waterfall_height,
                                 LV_IMG_CF_TRUE_COLOR);
            lv_obj_set_size(view->canvas, layout.width,
                            layout.waterfall_height);
            lv_obj_clear_flag(view->canvas, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(view->canvas, LV_OBJ_FLAG_HIDDEN);
        }
    }
    update_labels(view);
}

bool ls_spectrum_waterfall_resize(ls_spectrum_waterfall_t *view,
                                  int width, int height)
{
    if (!view) return false;
    ls_spectrum_layout_t layout;
    if (!ls_spectrum_layout(width, height, view->split_pct, view->fullscreen,
                            sizeof(lv_color_t), &layout))
        return false;
    ls_spectrum_buffer_state_t next_state = view->buffer_state;
    if (!ls_spectrum_buffer_resize(&next_state, width, height,
                                   sizeof(lv_color_t)))
        return false;
    if (view->pixels && width == view->width && height == view->height) {
        apply_layout(view);
        return true;
    }

    /* Two copies of the image, back to back. Scrolling then costs one
       row written twice instead of moving the whole canvas every update, and
       the visible window is always a contiguous run so LVGL needs no wrapping
       support. Costs a second buffer in PSRAM, which is plentiful; the traffic
       it removes is what starved the display. */
    const size_t ring_bytes = layout.allocation_bytes * 2u;
    lv_color_t *next = static_cast<lv_color_t *>(
        heap_caps_malloc(ring_bytes, MALLOC_CAP_SPIRAM));
    if (!next) return false;
    lv_color_t empty = heat_color(0.0f);
    for (size_t i = 0; i < (size_t)width * (size_t)height * 2u; ++i) next[i] = empty;

    if (view->pixels) heap_caps_free(view->pixels);
    view->pixels = next;
    view->width = width;
    view->height = height;
    view->allocation_bytes = ring_bytes;
    view->buffer_state = next_state;
    view->has_data = false;
    view->wf_top = 0;   /**/

    if (!view->canvas && view->plot) {
        view->canvas = lv_canvas_create(view->plot);
        ls_ui_style_plot(view->canvas);
    }
    apply_layout(view);
    return true;
}

bool ls_spectrum_waterfall_build(ls_spectrum_waterfall_t *view,
                                 lv_obj_t *parent, const char *title,
                                 int width, int height, int points,
                                 int split_pct, int contrast_pct,
                                 bool fullscreen,
                                 lv_event_cb_t tap_cb, void *tap_user)
{
    if (!view || !parent || points < 2) return false;
    ls_spectrum_waterfall_forget(view);
    view->split_pct = split_pct < 0 ? 0 : split_pct > 100 ? 100 : split_pct;
    view->contrast_pct = contrast_pct < 50 ? 50 : contrast_pct > 200 ? 200
                                                                     : contrast_pct;
    view->fullscreen = fullscreen;
    view->points = points;

    view->panel = ls_ui_panel(parent, title);
    /* The app supplies the available parent width; the shared frame owns its
     * padding. Measure after creating it so the canvas cannot extend under the
     * frame on either 480- or 720-wide panels. */
    lv_obj_update_layout(view->panel);
    int content_width = lv_obj_get_content_width(view->panel);
    if (content_width >= 2 && content_width < width) width = content_width;
    view->plot = lv_obj_create(view->panel);
    lv_obj_set_width(view->plot, lv_pct(100));
    lv_obj_set_height(view->plot, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(view->plot, LV_OPA_0, 0);
    lv_obj_set_style_border_width(view->plot, 0, 0);
    lv_obj_set_style_radius(view->plot, 0, 0);
    lv_obj_set_style_pad_all(view->plot, 0, 0);
    lv_obj_set_style_pad_row(view->plot, 0, 0);
    lv_obj_set_flex_flow(view->plot, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(view->plot, LV_OBJ_FLAG_SCROLLABLE);

    view->chart = lv_chart_create(view->plot);
    ls_ui_style_plot(view->chart);
    lv_chart_set_type(view->chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(view->chart, points);
    lv_chart_set_range(view->chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_obj_set_style_size(view->chart, 0, LV_PART_INDICATOR);
    view->series = lv_chart_add_series(view->chart, LS_UI_ACCENT,
                                       LV_CHART_AXIS_PRIMARY_Y);
    if (tap_cb) {
        lv_obj_add_flag(view->chart, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(view->chart, tap_cb, LV_EVENT_CLICKED, tap_user);
    }

    /* Spectrum remains useful if PSRAM is exhausted. */
    (void)ls_spectrum_waterfall_resize(view, width, height);
    if (!view->pixels) {
        view->width = width;
        view->height = height;
        apply_layout(view);
    }
    return true;
}

static void control_cb(lv_event_t *event)
{
    ls_spectrum_waterfall_t *view = static_cast<ls_spectrum_waterfall_t *>(
        lv_event_get_user_data(event));
    if (!view) return;
    int action = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(event));
    switch (action) {
    case 1: view->split_pct = ls_spectrum_split_step(view->split_pct, -1); break;
    case 2: view->split_pct = ls_spectrum_split_step(view->split_pct, +1); break;
    case 3: view->contrast_pct = ls_spectrum_contrast_step(view->contrast_pct, -1); break;
    case 4: view->contrast_pct = ls_spectrum_contrast_step(view->contrast_pct, +1); break;
    case 5: view->fullscreen = !view->fullscreen; break;
    default: return;
    }
    apply_layout(view);
    if (view->change_cb) view->change_cb(view, view->change_user);
}

static lv_obj_t *control_button(lv_obj_t *group, const char *text, int action,
                                ls_spectrum_waterfall_t *view,
                                lv_obj_t **label)
{
    lv_obj_t *button = ls_ui_group_button(group, text, LS_BTN_DEFAULT,
                                          control_cb, view, label);
    if (button) lv_obj_set_user_data(button, (void *)(intptr_t)action);
    return button;
}

void ls_spectrum_waterfall_add_controls(
    ls_spectrum_waterfall_t *view, unsigned controls,
    lv_event_cb_t gain_down_cb, lv_event_cb_t gain_up_cb, void *gain_user,
    ls_spectrum_change_cb_t change_cb, void *change_user)
{
    if (!view || !view->panel) return;
    view->change_cb = change_cb;
    view->change_user = change_user;
    view->controls=lv_obj_create(view->panel);
    lv_obj_remove_style_all(view->controls);
    lv_obj_set_size(view->controls,lv_pct(100),LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(view->controls,LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(view->controls,6,0);
    lv_obj_clear_flag(view->controls,LV_OBJ_FLAG_SCROLLABLE);
    if (controls & LS_SPECTRUM_CTL_SPLIT) {
        lv_obj_t *group = ls_ui_button_group(view->controls);
        control_button(group, "VIEW -", 1, view, nullptr);
        control_button(group, "50/50", 0, view, &view->split_label);
        control_button(group, "VIEW +", 2, view, nullptr);
        if (controls & LS_SPECTRUM_CTL_FULL)
            view->full_button = control_button(group, "FULL", 5, view,
                                                &view->full_label);
    } else if (controls & LS_SPECTRUM_CTL_FULL) {
        lv_obj_t *group = ls_ui_button_group(view->controls);
        view->full_button = control_button(group, "FULL", 5, view,
                                            &view->full_label);
    }
    if (controls & LS_SPECTRUM_CTL_CONTRAST) {
        lv_obj_t *group = ls_ui_button_group(view->controls);
        control_button(group, "CONTRAST -", 3, view, nullptr);
        control_button(group, "100%", 0, view, &view->contrast_label);
        control_button(group, "CONTRAST +", 4, view, nullptr);
    }
    if ((controls & LS_SPECTRUM_CTL_GAIN) && gain_down_cb && gain_up_cb) {
        lv_obj_t *group = ls_ui_button_group(view->controls);
        ls_ui_group_button(group, "GAIN -", LS_BTN_DEFAULT, gain_down_cb,
                           gain_user, nullptr);
        ls_ui_group_button(group, "GAIN", LS_BTN_DEFAULT, nullptr, gain_user,
                           &view->gain_label);
        ls_ui_group_button(group, "GAIN +", LS_BTN_DEFAULT, gain_up_cb,
                           gain_user, nullptr);
    }
    update_labels(view);
}

static void fit_page(ls_spectrum_waterfall_t *v)
{
    if (!v || !v->panel || !v->status_panel || v->fitting) return;
    v->fitting=true;
    lv_obj_t *page=lv_obj_get_parent(v->panel);
    int w=lv_obj_get_content_width(page), h=lv_obj_get_content_height(page);
    bool wide=w>=900 && w>h;
    lv_obj_set_flex_flow(page,wide?LV_FLEX_FLOW_ROW:LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(v->status_panel,wide?lv_pct(35):lv_pct(100));
    lv_obj_set_width(v->panel,wide?lv_pct(63):lv_pct(100));
    if (v->controls) {
        lv_obj_set_flex_flow(v->controls,wide?LV_FLEX_FLOW_ROW_WRAP:LV_FLEX_FLOW_COLUMN);
        for (uint32_t i=0;i<lv_obj_get_child_cnt(v->controls);++i)
            lv_obj_set_width(lv_obj_get_child(v->controls,i),wide?lv_pct(49):lv_pct(100));
    }
    lv_obj_update_layout(page);
    int desired=h-lv_obj_get_height(v->panel)+v->height;
    if (!wide) desired-=lv_obj_get_height(v->status_panel)+2*lv_obj_get_style_pad_row(page,0);
    if (desired<64) desired=64;
    if (desired>LS_SPECTRUM_CANVAS_MAX_HEIGHT) desired=LS_SPECTRUM_CANVAS_MAX_HEIGHT;
    int width=lv_obj_get_content_width(v->panel);
    if (width>=2) ls_spectrum_waterfall_resize(v,width,desired);
    lv_obj_update_layout(page);
    v->fitting=false;
}
static void fit_height(ls_spectrum_waterfall_t *v)
{
    if (!v || !v->panel || v->fitting) return;
    lv_obj_t *page=lv_obj_get_parent(v->panel);
    if (!page) return;
    v->fitting=true;
    lv_obj_update_layout(page);
    int room=lv_obj_get_content_height(page);
    int pad=lv_obj_get_style_pad_row(page,0);
    uint32_t kids=lv_obj_get_child_cnt(page);
    /* Everything the page draws beside this widget is already the size the
       page wants. Take it off the top, and take off the widget's own chrome
       - its title and control rows - by removing the canvas from the panel's
       measured height. What survives is what the canvas may become. */
    for (uint32_t i=0;i<kids;++i) {
        lv_obj_t *kid=lv_obj_get_child(page,i);
        if (kid==v->panel || lv_obj_has_flag(kid,LV_OBJ_FLAG_HIDDEN)) continue;
        room-=lv_obj_get_height(kid)+pad;
    }
    room-=lv_obj_get_height(v->panel)-v->height;
    if (room<64) room=64;
    if (room>LS_SPECTRUM_CANVAS_MAX_HEIGHT) room=LS_SPECTRUM_CANVAS_MAX_HEIGHT;
    int width=lv_obj_get_content_width(v->panel);
    if (width>=2 && room!=v->height) ls_spectrum_waterfall_resize(v,width,room);
    /* Predicting the total from the parts left FM's action row 11 px over the
       fold: panels carry their own borders and padding, and guessing at those
       is how the last round of constants got here. Measure what the layout
       actually produced and take the difference off once. One correction, not
       a loop, so a page that cannot fit settles instead of oscillating. */
    lv_obj_update_layout(page);
    int spill=0;
    for (uint32_t i=0;i<kids;++i) {
        lv_obj_t *kid=lv_obj_get_child(page,i);
        if (lv_obj_has_flag(kid,LV_OBJ_FLAG_HIDDEN)) continue;
        int bottom=lv_obj_get_y(kid)+lv_obj_get_height(kid)-
                   lv_obj_get_content_height(page);
        if (bottom>spill) spill=bottom;
    }
    if (spill>0 && width>=2 && v->height-spill>=64)
        ls_spectrum_waterfall_resize(v,width,v->height-spill);
    v->fitting=false;
}
static void fit_height_event(lv_event_t *e)
{
    fit_height(static_cast<ls_spectrum_waterfall_t *>(lv_event_get_user_data(e)));
}
void ls_spectrum_waterfall_fit_height(ls_spectrum_waterfall_t *view)
{
    if (!view || !view->panel) return;
    lv_obj_t *page=lv_obj_get_parent(view->panel);
    if (!page) return;
    lv_obj_add_event_cb(page,fit_height_event,LV_EVENT_SIZE_CHANGED,view);
    fit_height(view);
}
static void fit_page_event(lv_event_t *e)
{
    fit_page(static_cast<ls_spectrum_waterfall_t *>(lv_event_get_user_data(e)));
}
void ls_spectrum_waterfall_fit_page(ls_spectrum_waterfall_t *view,lv_obj_t *status)
{
    if (!view || !view->panel || !status) return;
    view->status_panel=status;
    lv_obj_t *page=lv_obj_get_parent(view->panel);
    lv_obj_add_event_cb(page,fit_page_event,LV_EVENT_SIZE_CHANGED,view);
    fit_page(view);
}

void ls_spectrum_waterfall_set_gain_text(ls_spectrum_waterfall_t *view,
                                         const char *text)
{
    if (view && view->gain_label) {
        const char *wanted = text ? text : "GAIN --";
        const char *shown = lv_label_get_text(view->gain_label);
        if (!shown || std::strcmp(shown, wanted) != 0)
            lv_label_set_text(view->gain_label, wanted);
    }
}

void ls_spectrum_waterfall_set_split(ls_spectrum_waterfall_t *view,
                                     int split_pct)
{
    if (!view) return;
    view->split_pct = split_pct < 0 ? 0 : split_pct > 100 ? 100 : split_pct;
    apply_layout(view);
}

void ls_spectrum_waterfall_set_contrast(ls_spectrum_waterfall_t *view,
                                        int contrast_pct)
{
    if (!view) return;
    view->contrast_pct = contrast_pct < 50 ? 50 : contrast_pct > 200 ? 200
                                                                       : contrast_pct;
    apply_layout(view);
}

void ls_spectrum_waterfall_set_fullscreen(ls_spectrum_waterfall_t *view,
                                          bool fullscreen)
{
    if (!view) return;
    view->fullscreen = fullscreen;
    apply_layout(view);
}

void ls_spectrum_waterfall_clear(ls_spectrum_waterfall_t *view)
{
    if (!view) return;
    if (view->chart && view->series) {
        for (int i = 0; i < view->points; ++i)
            lv_chart_set_value_by_id(view->chart, view->series, i,
                                     LV_CHART_POINT_NONE);
        lv_chart_refresh(view->chart);
    }
    if (view->pixels) {
        lv_color_t empty = heat_color(0.0f);
        size_t count = view->allocation_bytes / sizeof(lv_color_t);
        for (size_t i = 0; i < count; ++i) view->pixels[i] = empty;
        if (view->canvas) lv_obj_invalidate(view->canvas);
    }
    view->has_data = false;
    ls_spectrum_buffer_data(&view->buffer_state, false);
}

void ls_spectrum_waterfall_push(ls_spectrum_waterfall_t *view,
                                const float *bins, int n)
{
    if (!view || !bins || n < 1) {
        if (view) ls_spectrum_waterfall_clear(view);
        return;
    }
    if (view->chart && view->series) {
        if (view->points != n) {
            lv_chart_set_point_count(view->chart, n);
            view->points = n;
        }
        for (int i = 0; i < n; ++i)
            lv_chart_set_value_by_id(view->chart, view->series, i,
                (lv_coord_t)(apply_contrast(view, bins[i]) * 100.0f));
        lv_chart_refresh(view->chart);
    }
    if (!view->pixels || !view->canvas || view->width < 1 || view->height < 2) {
        view->has_data = true;
        ls_spectrum_buffer_data(&view->buffer_state, true);
        return;
    }

    const int wf_h = view->height;
    view->wf_top = (view->wf_top == 0) ? (wf_h - 1) : (view->wf_top - 1);
    lv_color_t *row_a = &view->pixels[(size_t)view->wf_top * (size_t)view->width];
    lv_color_t *row_b = &view->pixels[((size_t)view->wf_top + (size_t)wf_h) *
                                      (size_t)view->width];

    for (int x = 0; x < view->width; ++x) {
        int lo, hi;
        if (!ls_spectrum_column_bins(x, view->width, n, &lo, &hi)) continue;
        float peak = 0.0f;
        for (int i = lo; i < hi; ++i) if (bins[i] > peak) peak = bins[i];
        lv_color_t c = heat_color(apply_contrast(view, peak));
        row_a[x] = c;
        row_b[x] = c;
    }

    /* Move the view, not the image. */
    if (view->canvas && view->waterfall_height > 0)
        lv_canvas_set_buffer(view->canvas, row_a, view->width,
                             view->waterfall_height, LV_IMG_CF_TRUE_COLOR);

    view->has_data = true;
    ls_spectrum_buffer_data(&view->buffer_state, true);
    lv_obj_invalidate(view->canvas);
}

void ls_spectrum_waterfall_forget(ls_spectrum_waterfall_t *view)
{
    if (!view) return;
    if (view->panel && view->status_panel)
        lv_obj_remove_event_cb_with_user_data(lv_obj_get_parent(view->panel),fit_page_event,view);
    if (view->pixels) heap_caps_free(view->pixels);
    ls_spectrum_buffer_forget(&view->buffer_state);
    std::memset(view, 0, sizeof(*view));
}
