#include "cartocore/simd.h"
#include <string.h>
#include "cartocore/arch.h"
int cc_simd_enabled=1;
#if defined(__x86_64__) && defined(__GNUC__)
#include <stdatomic.h>
static atomic_int ready;
static atomic_flag initializing=ATOMIC_FLAG_INIT;
static int caps;
#else
static const int caps=0;
#endif
void cc_simd_init(void) {
#if defined(__x86_64__) && defined(__GNUC__)
    if(atomic_load_explicit(&ready,memory_order_acquire)) return;
    if(atomic_flag_test_and_set_explicit(&initializing,memory_order_acquire)) {
        while(!atomic_load_explicit(&ready,memory_order_acquire)) {}
        return;
    }
    __builtin_cpu_init();
    if(__builtin_cpu_supports("sse2")) caps|=1;
    if(__builtin_cpu_supports("pclmul")) caps|=2;
    if(__builtin_cpu_supports("avx2")) caps|=4;
    atomic_store_explicit(&ready,1,memory_order_release);
#endif
}
int cc_simd_capabilities(void) { cc_simd_init(); return caps; }
void cc_prefix32(uint32_t *words,size_t n) {
    uint32_t carry=0;
    for(size_t i=0;i<n;i++) {
        uint32_t v=words[i]; v^=v<<1; v^=v<<2; v^=v<<4; v^=v<<8; v^=v<<16;
        v^=0-carry; words[i]=v; carry=v>>31;
    }
}
void cc_prefix_scalar(uint64_t *words,size_t n) {
    uint64_t carry=0;
    for(size_t i=0;i<n;i++) {
        uint64_t v=words[i]; v^=v<<1; v^=v<<2; v^=v<<4; v^=v<<8; v^=v<<16; v^=v<<32;
        v^=0-carry; words[i]=v; carry=v>>63;
    }
}
size_t cc_equal_run_scalar(const cc_cell *a,const cc_cell *b,size_t n) {
    size_t i=0; while(i<n && a[i].codepoint==b[i].codepoint && a[i].fg==b[i].fg && a[i].bg==b[i].bg) i++; return i;
}
void cc_braille32_scalar(const uint64_t r[4],uint8_t out[32]) {
    for(unsigned i=0;i<32;i++) {
        unsigned shift=i*2;
        out[i]=(uint8_t)(((r[0]>>shift)&1)|((r[0]>>shift&2)<<2)|((r[1]>>shift&1)<<1)|((r[1]>>shift&2)<<3)|
            ((r[2]>>shift&1)<<2)|((r[2]>>shift&2)<<4)|((r[3]>>shift&3)<<6));
    }
}
void cc_integral_scalar(const uint8_t *mask,uint32_t *grid,int cols,int rows) {
    size_t stride=(size_t)cols+1; memset(grid,0,stride*sizeof(uint32_t));
    for(int y=0;y<rows;y++) {
        uint32_t *dst=grid+(size_t)(y+1)*stride,*prev=dst-stride; unsigned sum=0; dst[0]=0;
        for(int x=0;x<cols;x++) { sum+=mask[(size_t)y*cols+x]; dst[x+1]=prev[x+1]+sum; }
    }
}
#if defined(__x86_64__) && defined(__GNUC__)
#include <immintrin.h>
__attribute__((target("pclmul,sse2"))) static void prefix_clmul(uint64_t *words,size_t n) {
    uint64_t carry=0; __m128i ones=_mm_set_epi64x(0,-1);
    for(size_t i=0;i<n;i++) {
        uint64_t v=(uint64_t)_mm_cvtsi128_si64(_mm_clmulepi64_si128(_mm_cvtsi64_si128((long long)words[i]),ones,0));
        v^=0-carry; words[i]=v; carry=v>>63;
    }
}
__attribute__((target("sse2"))) static size_t equal_sse(const cc_cell *a,const cc_cell *b,size_t n) {
    size_t i=0;
    for(;i+4<=n;i+=4) {
        const __m128i *x=(const __m128i*)(a+i),*y=(const __m128i*)(b+i);
        __m128i v=_mm_and_si128(_mm_cmpeq_epi8(_mm_loadu_si128(x),_mm_loadu_si128(y)),_mm_cmpeq_epi8(_mm_loadu_si128(x+1),_mm_loadu_si128(y+1)));
        v=_mm_and_si128(v,_mm_cmpeq_epi8(_mm_loadu_si128(x+2),_mm_loadu_si128(y+2)));
        if(_mm_movemask_epi8(v)!=65535) break;
    }
    return i+cc_equal_run_scalar(a+i,b+i,n-i);
}
__attribute__((target("avx2"))) static size_t equal_avx(const cc_cell *a,const cc_cell *b,size_t n) {
    size_t i=0;
    for(;i+8<=n;i+=8) {
        const __m256i *x=(const __m256i*)(a+i),*y=(const __m256i*)(b+i);
        __m256i v=_mm256_and_si256(_mm256_cmpeq_epi8(_mm256_loadu_si256(x),_mm256_loadu_si256(y)),_mm256_cmpeq_epi8(_mm256_loadu_si256(x+1),_mm256_loadu_si256(y+1)));
        v=_mm256_and_si256(v,_mm256_cmpeq_epi8(_mm256_loadu_si256(x+2),_mm256_loadu_si256(y+2)));
        if((unsigned)_mm256_movemask_epi8(v)!=UINT32_MAX) break;
    }
    return i+cc_equal_run_scalar(a+i,b+i,n-i);
}
__attribute__((target("sse2"))) static __m128i pairs(uint64_t row,int high) {
    __m128i v=_mm_cvtsi64_si128((long long)row);
    v=_mm_unpacklo_epi8(v,v); v=high?_mm_unpackhi_epi8(v,v):_mm_unpacklo_epi8(v,v);
    __m128i a=_mm_set1_epi32(3),b=_mm_set1_epi32(3<<8),c=_mm_set1_epi32(3<<16),d=_mm_set1_epi32(3u<<24);
    return _mm_or_si128(_mm_or_si128(_mm_and_si128(v,a),_mm_and_si128(_mm_srli_epi16(v,2),b)),
                       _mm_or_si128(_mm_and_si128(_mm_srli_epi16(v,4),c),_mm_and_si128(_mm_srli_epi16(v,6),d)));
}
__attribute__((target("sse2"))) static void braille_sse(const uint64_t rows[4],uint8_t out[32]) {
    __m128i one=_mm_set1_epi8(1),two=_mm_set1_epi8(2);
    for(int hi=0;hi<2;hi++) {
        __m128i a=pairs(rows[0],hi),b=pairs(rows[1],hi),c=pairs(rows[2],hi),d=pairs(rows[3],hi);
        a=_mm_or_si128(_mm_and_si128(a,one),_mm_slli_epi16(_mm_and_si128(a,two),2));
        b=_mm_or_si128(_mm_slli_epi16(_mm_and_si128(b,one),1),_mm_slli_epi16(_mm_and_si128(b,two),3));
        c=_mm_or_si128(_mm_slli_epi16(_mm_and_si128(c,one),2),_mm_slli_epi16(_mm_and_si128(c,two),4));
        d=_mm_slli_epi16(d,6);
        _mm_storeu_si128((__m128i*)(out+hi*16),_mm_or_si128(_mm_or_si128(a,b),_mm_or_si128(c,d)));
    }
}
__attribute__((target("sse2"))) static void integral_sse(const uint8_t *mask,uint32_t *grid,int cols,int rows) {
    size_t stride=(size_t)cols+1; memset(grid,0,stride*sizeof(uint32_t)); __m128i zero=_mm_setzero_si128();
    for(int y=0;y<rows;y++) {
        uint32_t *dst=grid+(size_t)(y+1)*stride,*prev=dst-stride; unsigned sum=0; dst[0]=0; int x=0;
        for(;x+16<=cols;x+=16) {
            __m128i bytes=_mm_loadu_si128((const __m128i*)(mask+(size_t)y*cols+x));
            __m128i w[2]={_mm_unpacklo_epi8(bytes,zero),_mm_unpackhi_epi8(bytes,zero)};
            for(int k=0;k<4;k++) {
                __m128i v=(k&1)?_mm_unpackhi_epi16(w[k/2],zero):_mm_unpacklo_epi16(w[k/2],zero);
                v=_mm_add_epi32(v,_mm_slli_si128(v,4)); v=_mm_add_epi32(v,_mm_slli_si128(v,8));
                v=_mm_add_epi32(v,_mm_set1_epi32((int)sum)); sum=(unsigned)_mm_cvtsi128_si32(_mm_srli_si128(v,12));
                v=_mm_add_epi32(v,_mm_loadu_si128((const __m128i*)(prev+x+k*4+1)));
                _mm_storeu_si128((__m128i*)(dst+x+k*4+1),v);
            }
        }
        for(;x<cols;x++) { sum+=mask[(size_t)y*cols+x]; dst[x+1]=prev[x+1]+sum; }
    }
}
#endif
void cc_prefix_words(uint64_t *words,size_t n) {
    cc_simd_init();
#if defined(__x86_64__) && defined(__GNUC__)
    if(cc_simd_enabled && (caps&2)) { prefix_clmul(words,n); return; }
#endif
    cc_prefix_scalar(words,n);
}
size_t cc_equal_run(const cc_cell *a,const cc_cell *b,size_t n) {
#ifdef CC_HAVE_ESP32P4_PIE
    return cc_arch_equal_run(a,b,n);
#endif
    cc_simd_init();
#if defined(__x86_64__) && defined(__GNUC__)
    if(cc_simd_enabled && cc_simd_enabled!=2 && (caps&4)) return equal_avx(a,b,n);
    if(cc_simd_enabled && (caps&1)) return equal_sse(a,b,n);
#endif
    return cc_equal_run_scalar(a,b,n);
}
void cc_braille32(const uint64_t rows[4],uint8_t out[32]) {
    cc_simd_init();
#if defined(__x86_64__) && defined(__GNUC__)
    if(cc_simd_enabled && (caps&1)) { braille_sse(rows,out); return; }
#endif
    cc_braille32_scalar(rows,out);
}
void cc_integral(const uint8_t *mask,uint32_t *grid,int cols,int rows) {
    cc_simd_init();
#if defined(__x86_64__) && defined(__GNUC__)
    if(cc_simd_enabled && (caps&1)) { integral_sse(mask,grid,cols,rows); return; }
#endif
    cc_integral_scalar(mask,grid,cols,rows);
}
