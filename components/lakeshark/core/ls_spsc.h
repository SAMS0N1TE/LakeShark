/* Shared lock-free SPSC slot ownership. Payload and seam metadata belong to
   the caller; publish only after writing, release only after consuming. */
#ifndef LS_SPSC_H
#define LS_SPSC_H
#include <stdatomic.h>
#include <stdbool.h>
static inline bool ls_spsc_init(atomic_uint *write, atomic_uint *read)
{
    atomic_init(write, 0); atomic_init(read, 0);
    return atomic_is_lock_free(write) && atomic_is_lock_free(read);
}
static inline bool ls_spsc_write_slot(atomic_uint *write, atomic_uint *read,
                                      unsigned capacity, unsigned *sequence)
{
    unsigned w = atomic_load_explicit(write, memory_order_relaxed);
    unsigned r = atomic_load_explicit(read, memory_order_acquire);
    *sequence = w; return w - r < capacity;
}
static inline bool ls_spsc_read_slot(atomic_uint *write, atomic_uint *read,
                                     unsigned *sequence)
{
    unsigned r = atomic_load_explicit(read, memory_order_relaxed);
    unsigned w = atomic_load_explicit(write, memory_order_acquire);
    *sequence = r; return r != w;
}
static inline void ls_spsc_advance(atomic_uint *index, unsigned sequence)
{
    atomic_store_explicit(index, sequence + 1, memory_order_release);
}
#endif
