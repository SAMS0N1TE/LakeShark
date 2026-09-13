/* LS_TEST_INCLUDE: ${FW}/components/lakeshark/board */
#include "ls_test.h"
#include "esp_heap_caps.h"
#include <stdint.h>
#include <string.h>

static unsigned fallback_calls, fallback_caps;
void *heap_caps_aligned_calloc(size_t alignment, size_t n, size_t size, unsigned caps)
{
    (void)alignment; (void)n; (void)size;
    fallback_calls++;
    fallback_caps = caps;
    return NULL; /* Reproduce a fragmented heap that cannot satisfy alignment. */
}
#include "ls_usb_descriptors.h"

LS_CASE(descriptors_survive_heap_failure_and_reuse_zeroed_slots)
{
    void *slots[8];
    fallback_calls = 0;
    for (unsigned i = 0; i < 8; ++i) {
        slots[i] = heap_caps_aligned_calloc(512, 16, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        LS_CHECK(slots[i] != NULL);
        LS_CHECK(((uintptr_t)slots[i] & 511) == 0);
        for (unsigned j = 0; j < i; ++j) LS_CHECK(slots[i] != slots[j]);
        memset(slots[i], 0xa5, 16);
    }
    LS_CHECK(fallback_calls == 0);
    LS_CHECK(heap_caps_aligned_calloc(512, 16, 1, MALLOC_CAP_SPIRAM) == NULL);
    LS_CHECK(fallback_calls == 1);
    LS_CHECK((fallback_caps & MALLOC_CAP_SPIRAM) == 0);
    LS_CHECK((fallback_caps & MALLOC_CAP_INTERNAL) != 0);
    heap_caps_free(slots[3]);
    void *reused = heap_caps_aligned_calloc(512, 16, 1, MALLOC_CAP_DMA);
    LS_CHECK(reused == slots[3]);
    for (unsigned i = 0; i < 16; ++i) LS_CHECK(((uint8_t *)reused)[i] == 0);
    for (unsigned i = 0; i < 8; ++i) heap_caps_free(slots[i]);
}

LS_CASE(oversized_descriptor_falls_back_without_consuming_pool)
{
    fallback_calls = 0;
    LS_CHECK(heap_caps_aligned_calloc(512, 513, 1, MALLOC_CAP_DMA) == NULL);
    LS_CHECK(fallback_calls == 1);
    LS_CHECK(s_ls_usb_descriptor_used == 0);
    void *ordinary = heap_caps_malloc(16, MALLOC_CAP_INTERNAL);
    LS_CHECK(ordinary != NULL);
    heap_caps_free(ordinary);
    heap_caps_free(NULL);
}
