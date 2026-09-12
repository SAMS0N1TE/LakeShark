/* see ls_lv_mem_probe.h.  Counting wrapper around malloc for LVGL's
 * LV_MEM_CUSTOM_ALLOC/FREE/REALLOC hooks. */
#include "ls_lv_mem_probe.h"

#include <string.h>
#include <stdint.h>

/* 16 bytes so the pointer handed back keeps malloc's alignment on both 32 and
 * 64 bit hosts.  A smaller header would misalign lv_color_t buffers. */
#define LS_LV_PROBE_HDR 16u

static size_t s_live_bytes;
static size_t s_live_blocks;

void *ls_lv_probe_malloc(size_t size)
{
    if (size > SIZE_MAX - LS_LV_PROBE_HDR) return NULL;
    unsigned char *raw = (unsigned char *)malloc(size + LS_LV_PROBE_HDR);
    if (!raw) return NULL;
    memcpy(raw, &size, sizeof(size));
    s_live_bytes += size;
    ++s_live_blocks;
    return raw + LS_LV_PROBE_HDR;
}

void ls_lv_probe_free(void *ptr)
{
    if (!ptr) return;
    unsigned char *raw = (unsigned char *)ptr - LS_LV_PROBE_HDR;
    size_t size = 0;
    memcpy(&size, raw, sizeof(size));
    s_live_bytes -= size;
    --s_live_blocks;
    free(raw);
}

void *ls_lv_probe_realloc(void *ptr, size_t size)
{
    if (size > SIZE_MAX - LS_LV_PROBE_HDR) return NULL;
    if (!ptr) return ls_lv_probe_malloc(size);
    unsigned char *raw = (unsigned char *)ptr - LS_LV_PROBE_HDR;
    size_t old = 0;
    memcpy(&old, raw, sizeof(old));

    unsigned char *grown = (unsigned char *)realloc(raw, size + LS_LV_PROBE_HDR);
    if (!grown) return NULL;
    memcpy(grown, &size, sizeof(size));
    s_live_bytes -= old;
    s_live_bytes += size;
    return grown + LS_LV_PROBE_HDR;
}

size_t ls_lv_probe_live_bytes(void)  { return s_live_bytes; }
size_t ls_lv_probe_live_blocks(void) { return s_live_blocks; }
