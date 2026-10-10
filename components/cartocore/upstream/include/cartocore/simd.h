#ifndef CC_SIMD_H
#define CC_SIMD_H
#include "out.h"
/* 0 forces portable C, 1 enables runtime CPUID dispatch. */
extern int cc_simd_enabled;
void cc_simd_init(void);
int cc_simd_capabilities(void); /* bit 0 SSE2, bit 1 PCLMUL, bit 2 AVX2 */
void cc_prefix_scalar(uint64_t *words,size_t count);
void cc_prefix32(uint32_t *words,size_t n);
void cc_prefix_words(uint64_t *words,size_t count);
size_t cc_equal_run_scalar(const cc_cell *a,const cc_cell *b,size_t count);
size_t cc_equal_run(const cc_cell *a,const cc_cell *b,size_t count);
void cc_braille32_scalar(const uint64_t rows[4],uint8_t cells[32]);
void cc_braille32(const uint64_t rows[4],uint8_t cells[32]);
void cc_integral_scalar(const uint8_t *mask,uint32_t *grid,int cols,int rows);
void cc_integral(const uint8_t *mask,uint32_t *grid,int cols,int rows);
#endif
