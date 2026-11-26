/*
 * e3d private: where the engine's heap memory comes from.
 *
 * On the launcher, malloc puts blocks under 1 KB in internal SRAM. The engine
 * makes many such blocks (mesh arrays, band bins) that are not worth SRAM,
 * and they would use up what the tiles and per-object scratch need. So the
 * engine's malloc/calloc/realloc/free take PSRAM through the port layer
 * (e3d_port_alloc_sram is the one way into SRAM). A 16-byte header keeps the
 * size for realloc. The host build uses the C library as it is.
 *
 * Include e3d_port.h first: the names below are macros from here on.
 */
#pragma once

#ifndef E3D_HOST
#include <stddef.h>
#include <string.h>
#include "e3d_port.h"

#define E3D_HDR 16

static inline void *e3d_malloc(size_t n)
{
    char *p = e3d_port_ext_alloc(n + E3D_HDR);
    if (!p)
        return NULL;
    *(size_t *)p = n;
    return p + E3D_HDR;
}

static inline void e3d_free(void *p)
{
    if (p)
        e3d_port_ext_free((char *)p - E3D_HDR);
}

static inline void *e3d_calloc(size_t count, size_t size)
{
    size_t n = count * size;
    void *p = e3d_malloc(n);
    if (p)
        memset(p, 0, n);
    return p;
}

static inline void *e3d_realloc(void *p, size_t n)
{
    if (!p)
        return e3d_malloc(n);
    size_t old = *(size_t *)((char *)p - E3D_HDR);
    void *q = e3d_malloc(n);
    if (!q)
        return NULL;
    memcpy(q, p, old < n ? old : n);
    e3d_free(p);
    return q;
}

#define malloc(n)     e3d_malloc(n)
#define calloc(c, n)  e3d_calloc(c, n)
#define realloc(p, n) e3d_realloc(p, n)
#define free(p)       e3d_free(p)
#endif
