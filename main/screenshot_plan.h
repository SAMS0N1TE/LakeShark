#ifndef SCREENSHOT_PLAN_H
#define SCREENSHOT_PLAN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* LS-735: resource failures are part of the screenshot API, not an implied
   "out of memory?" in a log.  The console can name the resource that refused
   the shot without attempting a render to discover it. */
typedef enum {
    SCREENSHOT_OK = 0,
    SCREENSHOT_ERR_NOT_READY,
    SCREENSHOT_ERR_BUSY,
    SCREENSHOT_ERR_LVGL_LOCK,
    SCREENSHOT_ERR_DIMENSIONS,
    SCREENSHOT_ERR_STORAGE_UNKNOWN,
    SCREENSHOT_ERR_STORAGE_SHORT,
    SCREENSHOT_ERR_PSRAM_SHORT,
    SCREENSHOT_ERR_NAME,
    SCREENSHOT_ERR_QUEUE,
    SCREENSHOT_ERR_RENDER,
    SCREENSHOT_ERR_WRITE,
} screenshot_result_t;

typedef struct {
    size_t   snapshot_bytes;
    uint64_t bmp_bytes;
} screenshot_plan_t;

/* The snapshot buffer itself is large, but LVGL also allocates a small draw
   context while that buffer is live.  Keep enough external RAM available for
   that work instead of admitting an allocation that is guaranteed to fail in
   the renderer. */
#define SCREENSHOT_PSRAM_RESERVE_BYTES 4096u

/* Build the exact RGB565 snapshot / 24-bit BMP sizes and reject a shot whose
   filesystem or external-RAM budget cannot accommodate it.  UINT64_MAX for
   storage_available means the filesystem probe itself failed. */
screenshot_result_t screenshot_plan_make(uint32_t width, uint32_t height,
                                         uint64_t storage_available,
                                         size_t psram_available,
                                         size_t psram_largest,
                                         screenshot_plan_t *out);

const char *screenshot_result_message(screenshot_result_t result);

#ifdef __cplusplus
}
#endif

#endif
