#ifndef CC_ARCH_H
#define CC_ARCH_H
#include "out.h"
#include <stdio.h>
/* Byte counts; bit operations permit dst == src. Copy requires no overlap.
 * Arbitrary alignment and tails are supported by the dispatch layer. */
void cc_arch_or(void *dst,const void *src,size_t bytes);
void cc_arch_and(void *dst,const void *src,size_t bytes);
void cc_arch_xor(void *dst,const void *src,size_t bytes);
void cc_arch_clear(void *dst,size_t bytes);
void cc_arch_set(void *dst,uint8_t value,size_t bytes);
void cc_arch_copy(void *dst,const void *src,size_t bytes);
size_t cc_arch_equal_run(const cc_cell *a,const cc_cell *b,size_t count);
/* Task context only. IDF's first-use trap registers PIE ownership; do not
 * bypass it by setting the enable CSR. No global enable shared by tasks. */
int cc_arch_task_enable(void);
int cc_arch_pie_enabled(void);
int cc_arch_selftest(FILE *out);
void cc_arch_bench(FILE *out);
#endif
