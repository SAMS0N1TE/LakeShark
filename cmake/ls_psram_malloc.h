/* Force-included into one component (libssh, the SSH viewer): its plain
   malloc/calloc/realloc/strdup come from PSRAM. Below 16 KB, malloc otherwise
   prefers internal RAM, which the P4-NANO has none of to spare: an SSH
   session used the last of it and the next TCP connection was refused.
   free() releases from any heap, so it stays as it is. */
#pragma once
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"

static inline void *ls_ps_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}
static inline void *ls_ps_calloc(size_t a, size_t b)
{
    void *p = heap_caps_calloc(a, b, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : calloc(a, b);
}
static inline void *ls_ps_realloc(void *p, size_t n)
{
    void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return q || !n ? q : realloc(p, n);
}
static inline char *ls_ps_strdup(const char *s)
{
    const size_t n = strlen(s) + 1;
    char *d = ls_ps_malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}
#define malloc  ls_ps_malloc
#define calloc  ls_ps_calloc
#define realloc ls_ps_realloc
#define strdup  ls_ps_strdup
