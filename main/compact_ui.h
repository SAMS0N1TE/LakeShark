#pragma once
#include "esp_err.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t compact_ui_start(void (*mode_changed)(const char *));
/* The boot splash from as soon as the panel is up, instead of a dark screen
   until compact_ui_start: the globe and its progress line follow
   compact_ui_boot_progress, and compact_ui_start takes the screen over when
   they reach the end. Call after NVS is up. */
void compact_ui_splash_early(void);
/* How far boot has got, 0-100, and what it is doing (kept by pointer, so a
   string literal). Never goes backwards. */
void compact_ui_boot_progress(int pct, const char *label);
bool compact_ui_select_mode(const char *mode);
/* Follow an explicit control-head selection on the UI task. */
void compact_ui_show_mode(const char *mode);
#ifdef __cplusplus
}
#endif
