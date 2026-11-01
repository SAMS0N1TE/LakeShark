/* Waveshare MIPI-DSI panels on a board with a bare DSI connector (ESP32-P4-NANO),
   chosen at runtime. The choice is saved; 'panel' on the console lists, sets,
   detects and tests. With none saved the board stays headless. */
#ifndef LS_PANEL_WS_H
#define LS_PANEL_WS_H
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Virtual pixels only: leave PSRAM for capture, apps and networking. */
#define LS_TERM_PIXEL_MAX (4u * 1024u * 1024u)
#ifdef __cplusplus
extern "C" {
#endif

/* Touch mapping from the controller's raw report to native panel pixels:
   swap x/y first, then mirror. Defaults come from the panel's catalog entry. */
typedef struct {
    bool swap_xy, mirror_x, mirror_y;
} ls_panel_ws_touch_map_t;

/* The panel this boot runs (NULL when none is saved or it failed). */
const char *ls_panel_ws_name(void);
bool ls_panel_ws_touch_map(ls_panel_ws_touch_map_t *out);
/* Native size of the running panel; false when none is up. */
bool ls_panel_ws_size(int *w, int *h);
/* The terminal-only screen ('panel set term') has a landscape and a portrait
   size of its own, both unturned; this picks the one the framebuffer reports.
   False when the running panel is real glass. */
bool ls_panel_ws_term_view(bool portrait);
/* UI task only: lend a PNG destination for one paint, then detach with NULL.
   No allocation; size must match the current virtual panel. */
bool ls_panel_ws_term_pixels(uint16_t *pixels, int w, int h);
/* The terminal-only screen's landscape grid, live and kept for the next
   start (40-384 x 12-112). True when it changed: the UI then regrids. */
bool ls_panel_ws_term_resize(int cols, int rows, int cw, int ch);
/* Reapply the saved cell counts after choosing the session font. */
void ls_panel_ws_term_refont(int cw, int ch);

void ls_panel_ws_register_console(void);

#ifdef __cplusplus
}
#endif
#endif
