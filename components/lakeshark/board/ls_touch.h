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
#ifdef __cplusplus
}
#endif
