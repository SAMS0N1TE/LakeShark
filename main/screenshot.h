#ifndef SCREENSHOT_H
#define SCREENSHOT_H

/*LS-831  Capture the panel to a .bmp next to the REC captures.

   Same SD-then-SPIFFS fallback rec_dir() already uses, so the file comes off
   the board over the existing WiFi file transfer. See screenshot.c for why
   this is a file and not a console stream. */

#include <stdbool.h>
#include <stddef.h>

#include "screenshot_plan.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Prepare the statically allocated command/LVGL handoff.  Call once after the
   display starts.  This allocates no task and no dynamic internal memory. */
bool screenshot_init(void);

/* Writes <rec_dir>/<name>_<time>.bmp. path_out may be NULL.  This entry point
   is for an LVGL event/timer callback which already runs on the LVGL task. */
bool screenshot_save(const char *name, char *path_out, size_t path_len);

/* Console/task entry point.  Only the recursive LVGL render is dispatched to
   the existing LVGL task; filesystem writing remains on the caller. */
screenshot_result_t screenshot_save_from_task(const char *name,
                                               char *path_out,
                                               size_t path_len);

/* Lists .bmp files in the capture directory; returns the count. */
int  screenshot_list(char *out, size_t len);

#ifdef __cplusplus
}
#endif

#endif
