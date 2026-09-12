/* The SD card, mounted from this board's own pins. */

#ifndef LS_SDCARD_H
#define LS_SDCARD_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS_SDCARD_MOUNT "/sdcard"

esp_err_t ls_sdcard_mount(void);
void      ls_sdcard_unmount(void);

bool ls_sdcard_mounted(void);

/* Capacity and free space in bytes, both zero when nothing is mounted.
   Cheap enough to call from a diagnostics screen, not from a frame. */
bool ls_sdcard_size(uint64_t *total, uint64_t *free_bytes);

const char *ls_sdcard_name(void);

void ls_sdcard_diagnostics(void);

#ifdef __cplusplus
}
#endif

#endif /* LS_SDCARD_H */
