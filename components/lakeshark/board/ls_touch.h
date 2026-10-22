#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t ls_touch_init(void);
/* Native portrait pixels. No-data preserves contact until a release report. */
bool ls_touch_read(uint16_t *x, uint16_t *y, bool *pressed);
/* Runtime-chosen DSI panels (LS_BOARD_PANEL_RUNTIME): a GT911 on the primary
   bus, its address, product ID and reported range. */
esp_err_t ls_touch_gt911_info(uint8_t *addr, char id[5], uint16_t *xres, uint16_t *yres);
#ifdef __cplusplus
}
#endif
