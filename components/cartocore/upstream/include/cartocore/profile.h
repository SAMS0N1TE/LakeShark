#ifndef CC_PROFILE_H
#define CC_PROFILE_H
#include <stdint.h>
#if defined(__i386__) || defined(__x86_64__)
#include <x86intrin.h>
static inline uint64_t cc_ticks(void) { return __rdtsc(); }
#elif defined(__riscv) && __riscv_xlen == 32
static inline uint64_t cc_ticks(void) {
    uint32_t hi,lo,again;
    do { __asm__ volatile("rdcycleh %0" : "=r"(hi));
         __asm__ volatile("rdcycle %0" : "=r"(lo));
         __asm__ volatile("rdcycleh %0" : "=r"(again)); } while(hi!=again);
    return ((uint64_t)hi<<32)|lo;
}
#else
#include <time.h>
static inline uint64_t cc_ticks(void) { return (uint64_t)clock(); }
#endif
enum { CC_LOOKUP, CC_DECODE, CC_LINES, CC_FILL, CC_COVERAGE, CC_LABELS, CC_ENCODE, CC_STAGES };
typedef struct { uint64_t ticks[CC_STAGES]; } cc_profile;
extern cc_profile *cc_profiling;
static inline uint64_t cc_profile_sum(void) {
    uint64_t total=0; for(int i=0;i<CC_STAGES;i++) total+=cc_profiling->ticks[i]; return total;
}
#endif
