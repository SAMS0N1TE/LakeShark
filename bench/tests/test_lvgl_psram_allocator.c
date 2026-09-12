#include "ls_test.h"
#include "ls_lvgl_heap_test.h"
#include "src/misc/lv_mem.h"
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

/* A capability-enforcing heap with deterministic failure injection. The
 * production CMake contract compiles real LVGL lv_mem.c against these APIs;
 * accidentally using plain realloc bypasses the counters and fails tests. */
static unsigned allocations, resizes, frees, live;
static bool fail_next;
/* This isolated memory test needs no radio/log shim; avoid pulling its
 * separate heap implementation into the capability-enforcing fixture. */
void ls_diag_clear(void) {}
static void check_caps(uint32_t caps)
{ LS_EQ_UINT(caps, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
void *heap_caps_malloc(size_t size, uint32_t caps)
{
    check_caps(caps); ++allocations;
    if (fail_next) { fail_next = false; return NULL; }
    void *p = malloc(size);
    if (p) ++live;
    return p;
}
void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps)
{
    check_caps(caps); ++resizes;
    if (fail_next) { fail_next = false; return NULL; }
    bool was_null = ptr == NULL;
    void *p = realloc(ptr, size);
    if (p && was_null) ++live;
    return p;
}
void heap_caps_free(void *ptr)
{
    if (!ptr) return;
    ++frees; --live; free(ptr);
}
static void reset(void)
{
    LS_EQ_UINT(live, 0);
    allocations = resizes = frees = 0; fail_next = false;
    lv_mem_init();
}

LS_CASE(real_lvgl_realloc_null_grow_and_shrink_stay_psram)
{
    reset();
    unsigned char *p = lv_mem_realloc(NULL, 32);
    LS_CHECK(p); if (!p) return;
    LS_EQ_UINT(resizes, 1); LS_EQ_UINT(allocations, 0);
    for (int i=0;i<32;i++) p[i]=(unsigned char)(i+1);
    p = lv_mem_realloc(p, 512);
    LS_CHECK(p); if (!p) return;
    for (int i=0;i<32;i++) LS_EQ_UINT(p[i], i+1);
    p = lv_mem_realloc(p, 8);
    LS_CHECK(p); if (!p) return;
    for (int i=0;i<8;i++) LS_EQ_UINT(p[i], i+1);
    LS_EQ_UINT(resizes, 3);
    lv_mem_free(p);
    LS_EQ_UINT(frees, 1); LS_EQ_UINT(live, 0);
}

LS_CASE(real_lvgl_failed_resize_preserves_buffer_without_internal_retry)
{
    reset();
    unsigned char *p = lv_mem_alloc(32);
    LS_CHECK(p); if (!p) return;
    memset(p, 0x5a, 32);
    fail_next = true;
    LS_CHECK(lv_mem_realloc(p, 128) == NULL);
    LS_EQ_UINT(resizes, 1); LS_EQ_UINT(allocations, 1);
    LS_EQ_UINT(frees, 0); LS_EQ_UINT(live, 1);
    for (int i=0;i<32;i++) LS_EQ_UINT(p[i], 0x5a);
    lv_mem_free(p);
    LS_EQ_UINT(live, 0);
    fail_next = true;
    LS_CHECK(lv_mem_realloc(NULL, 64) == NULL);
    LS_EQ_UINT(resizes, 2); LS_EQ_UINT(live, 0);
    fail_next = true;
    LS_CHECK(lv_mem_alloc(64) == NULL);
    LS_EQ_UINT(allocations, 2); LS_EQ_UINT(live, 0);
}

LS_CASE(real_lvgl_zero_size_uses_its_sentinel_and_capability_free)
{
    reset();
    void *p = lv_mem_alloc(16);
    LS_CHECK(p); if (!p) return;
    void *zero = lv_mem_realloc(p, 0);
    LS_CHECK(zero); LS_EQ_UINT(frees, 1); LS_EQ_UINT(live, 0);
    LS_EQ_UINT(resizes, 0);
    lv_mem_free(zero); lv_mem_free(NULL);
    LS_EQ_UINT(frees, 1);
    p = lv_mem_realloc(zero, 16);
    LS_CHECK(p); LS_EQ_UINT(allocations, 2);
    lv_mem_free(p); LS_EQ_UINT(live, 0);
}
