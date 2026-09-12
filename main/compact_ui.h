#pragma once
#include "esp_err.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t compact_ui_start(void (*mode_changed)(const char *));
bool compact_ui_select_mode(const char *mode);
/* Follow an explicit control-head selection on the UI task. */
void compact_ui_show_mode(const char *mode);
#ifdef __cplusplus
}
#endif
