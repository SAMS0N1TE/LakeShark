#ifndef LS_SPECTRUM_WATERFALL_H
#define LS_SPECTRUM_WATERFALL_H

#include "lvgl.h"
#include "ui/ls_spectrum_waterfall_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ls_spectrum_waterfall ls_spectrum_waterfall_t;
typedef void (*ls_spectrum_change_cb_t)(ls_spectrum_waterfall_t *view,
                                        void *user_data);

enum {
    LS_SPECTRUM_CTL_SPLIT    = 1u << 0,
    LS_SPECTRUM_CTL_CONTRAST = 1u << 1,
    LS_SPECTRUM_CTL_FULL     = 1u << 2,
    LS_SPECTRUM_CTL_GAIN     = 1u << 3,
};

/* one display-derived renderer used by REC, FM and P25. RF sample
 * production, frequency mapping and tuner ownership remain in each app. */
struct ls_spectrum_waterfall {
    lv_obj_t          *panel;
    lv_obj_t          *plot;
    lv_obj_t          *chart;
    lv_chart_series_t *series;
    lv_obj_t          *canvas;
    lv_color_t        *pixels;
    /* Index of the newest row. The buffer holds two copies of the
       image back to back, so any window of height rows starting here is
       contiguous and the view can be moved without copying pixels. */
    int                wf_top;
    lv_obj_t          *split_label;
    lv_obj_t          *contrast_label;
    lv_obj_t          *full_button;
    lv_obj_t          *full_label;
    lv_obj_t          *gain_label;
    lv_obj_t          *controls;
    lv_obj_t          *status_panel;
    bool               fitting;
    int                width;
    int                height;
    int                waterfall_height;
    int                points;
    int                split_pct;
    int                contrast_pct;
    bool               fullscreen;
    bool               has_data;
    size_t             allocation_bytes;
    ls_spectrum_buffer_state_t buffer_state;
    ls_spectrum_change_cb_t change_cb;
    void              *change_user;
};

/* Width/height must be measurements from the active layout. The only
 * size-proportional allocation is the canvas in MALLOC_CAP_SPIRAM. Allocation
 * failure leaves a usable spectrum chart. */
bool ls_spectrum_waterfall_build(ls_spectrum_waterfall_t *view,
                                 lv_obj_t *parent, const char *title,
                                 int width, int height, int points,
                                 int split_pct, int contrast_pct,
                                 bool fullscreen,
                                 lv_event_cb_t tap_cb, void *tap_user);

/* Add shared, explicitly grouped controls. Optional gain callbacks still call
 * the owning app, so the widget never gains tuning ownership. */
void ls_spectrum_waterfall_add_controls(
    ls_spectrum_waterfall_t *view, unsigned controls,
    lv_event_cb_t gain_down_cb, lv_event_cb_t gain_up_cb, void *gain_user,
    ls_spectrum_change_cb_t change_cb, void *change_user);
void ls_spectrum_waterfall_set_gain_text(ls_spectrum_waterfall_t *view,
                                         const char *text);
/* Stack reception above the plot in portrait; place it alongside in landscape.
 * Resize on orientation changes without stopping capture or the decoder. */
void ls_spectrum_waterfall_fit_page(ls_spectrum_waterfall_t *view, lv_obj_t *status);

void ls_spectrum_waterfall_fit_height(ls_spectrum_waterfall_t *view);

bool ls_spectrum_waterfall_resize(ls_spectrum_waterfall_t *view,
                                  int width, int height);
void ls_spectrum_waterfall_set_split(ls_spectrum_waterfall_t *view,
                                     int split_pct);
void ls_spectrum_waterfall_set_contrast(ls_spectrum_waterfall_t *view,
                                        int contrast_pct);
void ls_spectrum_waterfall_set_fullscreen(ls_spectrum_waterfall_t *view,
                                          bool fullscreen);

void ls_spectrum_waterfall_push(ls_spectrum_waterfall_t *view,
                                const float *bins, int n);
void ls_spectrum_waterfall_clear(ls_spectrum_waterfall_t *view);

/* Idempotent. Call before the LVGL tree is discarded so the PSRAM allocation
 * is released on every lifecycle/error path. */
void ls_spectrum_waterfall_forget(ls_spectrum_waterfall_t *view);

#ifdef __cplusplus
}
#endif

#endif
