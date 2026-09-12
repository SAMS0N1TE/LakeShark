

#pragma once

#include <stdlib.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *ls_lv_probe_malloc(size_t size);
void  ls_lv_probe_free(void *ptr);
void *ls_lv_probe_realloc(void *ptr, size_t size);

/* Bytes LVGL has asked for and not yet returned, and how many blocks that is.
 * The per-block header is not counted: the number is what LVGL requested. */
size_t ls_lv_probe_live_bytes(void);
size_t ls_lv_probe_live_blocks(void);

#ifdef __cplusplus
}
#endif
