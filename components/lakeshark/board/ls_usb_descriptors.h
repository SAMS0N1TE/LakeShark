/* forced into hcd_dwc.c only. A P4 interrupt watchdog stopped in
 * cache_sync_xfer_descriptor_list -> esp_cache_msync -> ROM cache handling
 * for a PSRAM descriptor at 0x48325000. Keep these small interrupt-side
 * lists internal without moving the 256 KiB USB sample buffers out of PSRAM.
 * Both aligned-calloc sites in IDF 5.4.3 HCD allocate descriptor/frame lists.
 */
#pragma once
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Two host frame lists, and two lists per endpoint (control, RX, TX).
 * Reserve aligned storage before task stacks can fragment the DMA heap. */
#define LS_USB_DESCRIPTOR_SLOTS 8
#define LS_USB_DESCRIPTOR_BYTES 512
static DRAM_ATTR uint8_t s_ls_usb_descriptors[LS_USB_DESCRIPTOR_SLOTS]
    [LS_USB_DESCRIPTOR_BYTES] __attribute__((aligned(512)));
static DRAM_ATTR uint32_t s_ls_usb_descriptor_used;

static inline void *ls_usb_descriptor_calloc(size_t alignment, size_t n,
                                            size_t size, uint32_t caps)
{
    caps = (caps & ~MALLOC_CAP_SPIRAM) | MALLOC_CAP_INTERNAL;
    if (size && n <= LS_USB_DESCRIPTOR_BYTES / size && n &&
        alignment && alignment <= LS_USB_DESCRIPTOR_BYTES &&
        (alignment & (alignment - 1)) == 0) {
        uint32_t used = __atomic_load_n(&s_ls_usb_descriptor_used, __ATOMIC_ACQUIRE);
        for (;;) {
            uint32_t available = (~used) & ((1u << LS_USB_DESCRIPTOR_SLOTS) - 1);
            if (!available) break;
            unsigned slot = (unsigned)__builtin_ctz(available);
            uint32_t next = used | (1u << slot);
            if (__atomic_compare_exchange_n(&s_ls_usb_descriptor_used, &used,
                    next, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                memset(s_ls_usb_descriptors[slot], 0, n * size);
                return s_ls_usb_descriptors[slot];
            }
        }
    }
    return heap_caps_aligned_calloc(alignment, n, size, caps);
}

static inline void ls_usb_descriptor_free(void *ptr)
{
    uintptr_t offset = (uintptr_t)ptr - (uintptr_t)s_ls_usb_descriptors;
    if (ptr && offset < sizeof(s_ls_usb_descriptors) &&
        offset % LS_USB_DESCRIPTOR_BYTES == 0) {
        unsigned slot = (unsigned)(offset / LS_USB_DESCRIPTOR_BYTES);
        __atomic_fetch_and(&s_ls_usb_descriptor_used, ~(1u << slot), __ATOMIC_RELEASE);
        return;
    }
    heap_caps_free(ptr);
}
#define heap_caps_aligned_calloc ls_usb_descriptor_calloc
#define heap_caps_free ls_usb_descriptor_free
