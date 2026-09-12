#ifndef LS_SHIM_HEAP_CAPS_H
#define LS_SHIM_HEAP_CAPS_H
#include <stddef.h>
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM   (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_8BIT     (1 << 2)
#define MALLOC_CAP_DMA      (1 << 3)
#define MALLOC_CAP_DEFAULT  0

/* the IDF header these stand in for is extern "C"; without the same
   linkage here a C++ firmware source compiled on the bench links against a
   mangled name that shim_impl.c does not define. */
#ifdef __cplusplus
extern "C" {
#endif

void *heap_caps_malloc(size_t n, unsigned caps);
void *heap_caps_calloc(size_t count, size_t size, unsigned caps);
void heap_caps_free(void *ptr);
size_t heap_caps_get_free_size(unsigned caps);
size_t heap_caps_get_largest_free_block(unsigned caps);

typedef struct {
    size_t total_free_bytes;
    size_t total_allocated_bytes;
    size_t largest_free_block;
    size_t minimum_free_bytes;
    size_t allocated_blocks;
    size_t free_blocks;
    size_t total_blocks;
} multi_heap_info_t;

void heap_caps_get_info(multi_heap_info_t *info, unsigned caps);

/* Host-test controls for allocation capability and cleanup contracts. */
void ls_shim_heap_reset(void);
void ls_shim_heap_fail_from(unsigned call_index);
unsigned ls_shim_heap_call_count(void);
unsigned ls_shim_heap_call_caps(unsigned call_index);
size_t ls_shim_heap_call_size(unsigned call_index);
unsigned ls_shim_heap_outstanding(void);

#ifdef __cplusplus
}
#endif

#endif
