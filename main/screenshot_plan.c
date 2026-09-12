/* pure screenshot resource admission.  Keeping the overflow and
   low-resource decisions outside LVGL lets the host bench prove that a shot
   is refused before the recursive object-tree renderer is entered. */

#include "screenshot_plan.h"

#include <limits.h>

screenshot_result_t screenshot_plan_make(uint32_t width, uint32_t height,
                                         uint64_t storage_available,
                                         size_t psram_available,
                                         size_t psram_largest,
                                         screenshot_plan_t *out)
{
    if (!out || width == 0 || height == 0)
        return SCREENSHOT_ERR_DIMENSIONS;

    if ((uint64_t)width > UINT64_MAX / (uint64_t)height)
        return SCREENSHOT_ERR_DIMENSIONS;
    const uint64_t pixels = (uint64_t)width * (uint64_t)height;
    if (pixels > UINT64_MAX / 2ull)
        return SCREENSHOT_ERR_DIMENSIONS;
    const uint64_t snapshot_bytes = pixels * 2ull; /* LV_COLOR_DEPTH == 16 */
    const uint64_t row_bytes = ((uint64_t)width * 3ull + 3ull) & ~3ull;
    if (row_bytes > (UINT64_MAX - 54ull) / (uint64_t)height)
        return SCREENSHOT_ERR_DIMENSIONS;
    const uint64_t bmp_bytes = 54ull + row_bytes * (uint64_t)height;

    if (snapshot_bytes > (uint64_t)SIZE_MAX ||
        snapshot_bytes > (uint64_t)SIZE_MAX - SCREENSHOT_PSRAM_RESERVE_BYTES ||
        bmp_bytes < row_bytes)
        return SCREENSHOT_ERR_DIMENSIONS;

    out->snapshot_bytes = (size_t)snapshot_bytes;
    out->bmp_bytes = bmp_bytes;

    if (storage_available == UINT64_MAX)
        return SCREENSHOT_ERR_STORAGE_UNKNOWN;
    if (storage_available < bmp_bytes)
        return SCREENSHOT_ERR_STORAGE_SHORT;

    if (psram_available < (size_t)snapshot_bytes + SCREENSHOT_PSRAM_RESERVE_BYTES ||
        psram_largest < (size_t)snapshot_bytes)
        return SCREENSHOT_ERR_PSRAM_SHORT;

    return SCREENSHOT_OK;
}

const char *screenshot_result_message(screenshot_result_t result)
{
    switch (result) {
    case SCREENSHOT_OK:                  return "ok";
    case SCREENSHOT_ERR_NOT_READY:       return "screenshot service not ready";
    case SCREENSHOT_ERR_BUSY:            return "another screenshot is in progress";
    case SCREENSHOT_ERR_LVGL_LOCK:       return "panel is busy";
    case SCREENSHOT_ERR_DIMENSIONS:      return "invalid panel dimensions";
    case SCREENSHOT_ERR_STORAGE_UNKNOWN: return "capture storage is unavailable";
    case SCREENSHOT_ERR_STORAGE_SHORT:   return "not enough capture storage";
    case SCREENSHOT_ERR_PSRAM_SHORT:     return "not enough external RAM";
    case SCREENSHOT_ERR_NAME:            return "no free screenshot filename";
    case SCREENSHOT_ERR_QUEUE:           return "panel render queue is full";
    case SCREENSHOT_ERR_RENDER:          return "panel render failed";
    case SCREENSHOT_ERR_WRITE:           return "screenshot file write failed";
    default:                             return "unknown screenshot failure";
    }
}
