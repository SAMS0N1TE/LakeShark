/* forced into hcd_dwc.c only. A P4 interrupt watchdog stopped in
 * cache_sync_xfer_descriptor_list -> esp_cache_msync -> ROM cache handling
 * for a PSRAM descriptor at 0x48325000. Keep these small interrupt-side
 * lists internal without moving the 256 KiB USB sample buffers out of PSRAM.
 * Both aligned-calloc sites in IDF 5.4.3 HCD allocate descriptor/frame lists.
 */
#pragma once
#include "esp_heap_caps.h"

static inline void *ls_usb_descriptor_calloc(size_t alignment, size_t n,
                                            size_t size, uint32_t caps)
{
    caps = (caps & ~MALLOC_CAP_SPIRAM) | MALLOC_CAP_INTERNAL;
    return heap_caps_aligned_calloc(alignment, n, size, caps);
}
#define heap_caps_aligned_calloc ls_usb_descriptor_calloc
