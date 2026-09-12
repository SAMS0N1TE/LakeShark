#ifndef LS_PANEL_H
#define LS_PANEL_H
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/* First panel milestone: DSI-generated color bars, no LVGL allocation. */
esp_err_t ls_panel_test_start(void);

/* The panel, brought up for the TUI and nothing else.

   This was ls_panel_ui_start, which handed the panel to LVGL - the port, its
   task, a 64-line draw buffer and a flush callback - and only then fetched
   the framebuffers the TUI actually paints. The TUI used none of the rest,
   and the rest kept a whole second interface alive underneath it (HANDOFF
   6.2). */
esp_err_t ls_panel_start(void);
esp_err_t ls_panel_set_brightness(unsigned percent);

/* The refresh count, and how many the scan managed in the last second. Takes
   a second to answer. */
void ls_panel_diagnostics(void);

/* Direct framebuffer access for the TUI backend. */

typedef struct {
    uint16_t *pixels;   /* RGB565, native orientation */
    int width;          /* native, always the panel's own width  */
    int height;         /* native, always the panel's own height */
} ls_panel_fb_t;

bool ls_panel_fb(ls_panel_fb_t *out);
void ls_panel_fb_present(void);
#ifdef __cplusplus
}
#endif
#endif
