/* See ls_textpack.h. */

#include "ls_textpack.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#if defined(LS_TEXT_PACKED) && LS_TEXT_PACKED

#include "cartocore/zstd.h"

/* LS_PRINTF and friends: the same bytes out as printf/fprintf/snprintf with
   the literal, the format found here by id. */
int ls_text_printf(unsigned id, ...)
{
    va_list ap;
    va_start(ap, id);
    const int r = vprintf(ls_text_get(id), ap);
    va_end(ap);
    return r;
}

int ls_text_fprintf(unsigned id, FILE *f, ...)
{
    va_list ap;
    va_start(ap, f);
    const int r = vfprintf(f, ls_text_get(id), ap);
    va_end(ap);
    return r;
}

int ls_text_snprintf(unsigned id, char *buf, size_t n, ...)
{
    va_list ap;
    va_start(ap, n);
    const int r = vsnprintf(buf, n, ls_text_get(id), ap);
    va_end(ap);
    return r;
}

int ls_text_puts(unsigned id) { return puts(ls_text_get(id)); }

int ls_text_fputs(unsigned id, FILE *f) { return fputs(ls_text_get(id), f); }

int ls_text_snputs(unsigned id, char *buf, size_t n)
{
    return snprintf(buf, n, "%s", ls_text_get(id));
}

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
static void *big_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}
#else
static void *big_alloc(size_t n) { return malloc(n); }
#endif

extern const ls_text_frame_t ls_text_frames[];
extern const unsigned ls_text_nframes;
extern const char *ls_text_slots[];
extern const unsigned ls_text_count;
extern const uint16_t ls_text_index[];        /* offset in the text's frame */
extern const uint16_t ls_text_frame_first[];  /* ids are numbered frame by frame */

int ls_text_unpack_into(const ls_text_frame_t *f, void *dst, uint32_t n)
{
    if (!f || !f->data || !dst || n != f->raw_len) return 0;
    if (f->size == f->raw_len) {
        memcpy(dst, f->data, n);
        return 1;
    }
    const size_t cap = (size_t)f->raw_len + cc_zstd_context_bytes() + 64u;
    void *work = big_alloc(cap);
    int ok = 0;
    if (work) {
        cc_arena a;
        cc_arena_init(&a, work, cap);
        cc_str z = { f->data, f->size }, d = { 0, 0 };
        if (cc_zstd_decode(z, &a, &d) && d.size == n) {
            memcpy(dst, d.data, n);
            ok = 1;
        }
    }
    free(work);
    return ok;
}

const char *ls_text_frame(const ls_text_frame_t *f, const char **slot)
{
    const char *p = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (p) return p;
    if (!f->data || !f->raw_len) return NULL;
    if (f->size == f->raw_len) {
        p = (const char *)f->data;          /* stored as it is */
        __atomic_store_n(slot, p, __ATOMIC_RELEASE);
        return p;
    }
    char *out = big_alloc(f->raw_len);
    const size_t cap = (size_t)f->raw_len + cc_zstd_context_bytes() + 64u;
    void *work = big_alloc(cap);
    bool ok = false;
    if (out && work) {
        cc_arena a;
        cc_arena_init(&a, work, cap);
        cc_str z = { f->data, f->size }, d = { 0, 0 };
        if (cc_zstd_decode(z, &a, &d) && d.size == f->raw_len) {
            memcpy(out, d.data, d.size);
            ok = true;
        }
    }
    free(work);
    if (!ok) {
        free(out);
        return NULL;
    }
    const char *expected = NULL;
    if (!__atomic_compare_exchange_n(slot, &expected, (const char *)out, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        free(out);                          /* another task got there first */
        return expected;
    }
    return out;
}

const char *ls_text_get(unsigned id)
{
    if (id >= ls_text_count || !ls_text_nframes) return "";
    unsigned fr = 0;
    while (fr + 1 < ls_text_nframes && id >= ls_text_frame_first[fr + 1]) fr++;
    const char *base = ls_text_frame(&ls_text_frames[fr], &ls_text_slots[fr]);
    return base ? base + ls_text_index[id] : "";
}

unsigned ls_text_decoded(unsigned *bytes)
{
    unsigned n = 0, b = 0;
    for (unsigned k = 0; k < ls_text_nframes; k++) {
        if (__atomic_load_n(&ls_text_slots[k], __ATOMIC_ACQUIRE) &&
            ls_text_frames[k].size != ls_text_frames[k].raw_len) {
            n++;
            b += ls_text_frames[k].raw_len;
        }
    }
    if (bytes) *bytes = b;
    return n;
}

void ls_text_warm(void)
{
    for (unsigned k = 0; k < ls_text_nframes; k++)
        (void)ls_text_frame(&ls_text_frames[k], &ls_text_slots[k]);
}

#else /* the text stays in the literals */

int ls_text_unpack_into(const ls_text_frame_t *f, void *dst, uint32_t n)
{
    if (!f || !f->data || !dst || n != f->raw_len || f->size != f->raw_len) return 0;
    memcpy(dst, f->data, n);
    return 1;
}

const char *ls_text_frame(const ls_text_frame_t *f, const char **slot)
{
    (void)slot;
    return (f && f->data && f->size == f->raw_len) ? (const char *)f->data : NULL;
}

const char *ls_text_get(unsigned id)
{
    (void)id;
    return "";
}

unsigned ls_text_decoded(unsigned *bytes)
{
    if (bytes) *bytes = 0;
    return 0;
}

void ls_text_warm(void) {}

#endif
