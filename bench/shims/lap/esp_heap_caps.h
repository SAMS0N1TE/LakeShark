#pragma once
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define MALLOC_CAP_DMA 8
void *heap_caps_malloc(size_t n,int caps);
void *heap_caps_calloc(size_t n,size_t size,int caps);
void *heap_caps_realloc(void *p,size_t n,int caps);
void heap_caps_free(void *p);
