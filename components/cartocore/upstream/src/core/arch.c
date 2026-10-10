#include "cartocore/arch.h"
#include "cartocore/simd.h"
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#ifdef CC_HAVE_ESP32P4_PIE
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "riscv/rvruntime-frames.h"
#include "riscv/csr_pie.h"
extern RvCoprocSaveArea *pxPortGetCoprocArea(StaticTask_t *,bool,int);
extern void cc_pie_task_enter(void);
extern void cc_pie_or(void *,const void *,size_t);
extern void cc_pie_and(void *,const void *,size_t);
extern void cc_pie_xor(void *,const void *,size_t);
extern void cc_pie_clear(void *,size_t);
extern void cc_pie_copy(void *,const void *,size_t);
extern size_t cc_pie_equal48(const void *,const void *,size_t);
#endif
int cc_arch_pie_enabled(void) {
#ifdef CC_HAVE_ESP32P4_PIE
    if(xPortInIsrContext()) return 0;
    TaskHandle_t task=xTaskGetCurrentTaskHandle();
    if(!task) return 0;
    /* sa_enable is IDF's task ownership bitmap, not a process-wide flag. */
    RvCoprocSaveArea *sa=pxPortGetCoprocArea((StaticTask_t *)task,false,PIE_COPROC_IDX);
    if(!(sa->sa_enable & (1u<<PIE_COPROC_IDX))) return 0;
    unsigned state;
    __asm__ volatile("csrr %0, 0x7f2":"=r"(state));
    /* A switch may leave an opted-in task's CSR off until the next PIE
     * instruction. Let IDF restore ownership instead of falling back forever. */
    if(!(state&3)) {
        cc_pie_task_enter();
        __asm__ volatile("csrr %0, 0x7f2":"=r"(state));
    }
    return (state&3)!=0;
#else
    return 0;
#endif
}
int cc_arch_task_enable(void) {
#ifdef CC_HAVE_ESP32P4_PIE
    if(xPortInIsrContext() || !xTaskGetCurrentTaskHandle()) return 0;
    cc_pie_task_enter();
#endif
    return cc_arch_pie_enabled();
}
#if defined(__GNUC__)
typedef uint32_t cc_alias32 __attribute__((__may_alias__));
#else
typedef uint32_t cc_alias32;
#endif
/* C byte references also avoid alignment/aliasing assumptions. */
static void reference(unsigned op,void *dst,const void *src,size_t n) {
    if(op==3) { memset(dst,0,n); return; }
    if(op==4) { memcpy(dst,src,n); return; }
    uint8_t *d=dst; const uint8_t *s=src;
    /* Aligned alias-safe words emit native lw/sw; byte tails preserve guards. */
    if((((uintptr_t)d|(uintptr_t)s)&3)==0) {
        while(n>=4) {
            uint32_t x=*(const cc_alias32 *)d,y=*(const cc_alias32 *)s;
            if(op==0) x|=y; else if(op==1) x&=y; else x^=y;
            *(cc_alias32 *)d=x; d+=4; s+=4; n-=4;
        }
    }
    for(size_t i=0;i<n;i++) {
        if(op==0) d[i]|=s[i];
        else if(op==1) d[i]&=s[i];
        else d[i]^=s[i];
    }
}
static void dispatch(unsigned op,void *dst,const void *src,size_t n) {
    uint8_t *d=dst; const uint8_t *s=src;
#ifdef CC_HAVE_ESP32P4_PIE
    if(n>=16 && cc_simd_enabled && cc_arch_pie_enabled()) {
        /* Peel only when both addresses can become aligned together. */
        if(op==3 || (((uintptr_t)d^(uintptr_t)s)&15)==0) {
            size_t head=(16-((uintptr_t)d&15))&15;
            reference(op,d,s,head); d+=head; if(op!=3) s+=head; n-=head;
            size_t blocks=n/16;
            if(blocks) {
                if(op==0) cc_pie_or(d,s,blocks);
                else if(op==1) cc_pie_and(d,s,blocks);
                else if(op==2) cc_pie_xor(d,s,blocks);
                else if(op==3) cc_pie_clear(d,blocks);
                else cc_pie_copy(d,s,blocks);
                size_t bytes=blocks*16; d+=bytes; if(op!=3) s+=bytes; n-=bytes;
            }
        }
    }
#endif
    reference(op,d,s,n);
}
void cc_arch_or(void *d,const void *s,size_t n) { dispatch(0,d,s,n); }
void cc_arch_and(void *d,const void *s,size_t n) { dispatch(1,d,s,n); }
void cc_arch_xor(void *d,const void *s,size_t n) { dispatch(2,d,s,n); }
void cc_arch_clear(void *d,size_t n) { dispatch(3,d,NULL,n); }
void cc_arch_copy(void *d,const void *s,size_t n) { dispatch(4,d,s,n); }
void cc_arch_set(void *dst,uint8_t value,size_t n) {
    if(!value) { cc_arch_clear(dst,n); return; }
#ifdef CC_HAVE_ESP32P4_PIE
    if(n>=32 && cc_simd_enabled && cc_arch_pie_enabled()) {
        /* Reuse verified copy kernel with a constant 128-bit source. */
        _Alignas(16) uint8_t row[16]; memset(row,value,sizeof(row));
        uint8_t *d=dst;
        size_t head=(16-((uintptr_t)d&15))&15;
        memset(d,value,head); d+=head; n-=head;
        extern void cc_pie_set(void *,const void *,size_t);
        size_t blocks=n/16;
        cc_pie_set(d,row,blocks); d+=blocks*16; n-=blocks*16;
        memset(d,value,n); return;
    }
#endif
    uint8_t *d=dst;
    while(n && ((uintptr_t)d&3)) { *d++=value; n--; }
    uint32_t word=(uint32_t)value*UINT32_C(0x01010101);
    while(n>=4) { *(cc_alias32 *)d=word; d+=4; n-=4; }
    while(n--) *d++=value;
}
size_t cc_arch_equal_run(const cc_cell *a,const cc_cell *b,size_t n) {
    size_t i=0;
#ifdef CC_HAVE_ESP32P4_PIE
    _Static_assert(sizeof(cc_cell)==12,"PIE frame comparison requires packed cells");
    if(n>=4 && cc_simd_enabled && cc_arch_pie_enabled() &&
       (((uintptr_t)a^(uintptr_t)b)&15)==0) {
        while(i<n && ((uintptr_t)(a+i)&15)) {
            if(!cc_equal_run_scalar(a+i,b+i,1)) return i;
            i++;
        }
        i+=4*cc_pie_equal48(a+i,b+i,(n-i)/4);
    }
#endif
    return i+cc_equal_run_scalar(a+i,b+i,n-i);
}
static uint32_t random_word(uint32_t *s) { *s^=*s<<13; *s^=*s>>17; *s^=*s<<5; return *s; }
static uint64_t ticks(void) {
#ifdef CC_HAVE_ESP32P4_PIE
    unsigned t; __asm__ volatile("rdcycle %0":"=r"(t)); return t;
#else
    return (uint64_t)clock();
#endif
}
static uint64_t elapsed(uint64_t start) {
#ifdef CC_HAVE_ESP32P4_PIE
    return (uint32_t)(ticks()-start);
#else
    return ticks()-start;
#endif
}
static const char *const names[]={"row-or","row-and","row-xor","row-clear","row-copy","frame-equal"};
int cc_arch_selftest(FILE *out) {
    /* Keep console stacks small, including IDF's reserved coprocessor area. */
    void *storage=malloc(3*560+2*48*sizeof(cc_cell)+15);
    if(!storage) { if(out) fputs("carto arch selftest FAIL: allocation\n",out); return 0; }
    uint8_t *a=(uint8_t *)(((uintptr_t)storage+15)&~(uintptr_t)15),*b=a+560,*c=b+560;
    cc_cell *x=(cc_cell *)(c+560),*y=x+48;
    uint32_t seed=0x94135a7u; int failures=0;
    if(out) fprintf(out,"carto PIE task-enabled=%d (0 means C fallback)\n",cc_arch_pie_enabled());
    for(unsigned op=0;op<5;op++) {
        int bad=0;
        for(unsigned trial=0;trial<1024;trial++) {
            size_t n=trial<258?trial:random_word(&seed)%513;
            size_t off=trial%16,soff=(trial&16)?off:(trial/16)%16;
            for(size_t k=0;k<560;k++) { a[k]=(uint8_t)random_word(&seed); b[k]=(uint8_t)random_word(&seed); }
            memcpy(c,a,560); reference(op,c+off,b+soff,n); dispatch(op,a+off,b+soff,n);
            if(memcmp(a,c,560)) { bad=1; break; }
            /* In-place bit ops, including tails. */
            if(op<3) {
                reference(op,c+off,c+off,n); dispatch(op,a+off,a+off,n);
                if(memcmp(a,c,560)) { bad=1; break; }
            }
        }
        failures+=bad; if(out) fprintf(out,"  %s %s randomized/alignment/tails/guards\n",names[op],bad?"FAIL":"PASS");
    }
    int bad=0;
    for(unsigned trial=0;trial<2048;trial++) {
        for(unsigned k=0;k<48;k++) x[k]=(cc_cell){random_word(&seed),random_word(&seed),random_word(&seed)};
        memcpy(y,x,48*sizeof(*x)); size_t off=trial%4,boff=(trial&4)?off:(trial/4)%4,n=random_word(&seed)%41;
        memcpy(y+boff,x+off,n*sizeof(*x));
        if(n && (trial&8)) {
            size_t changed=random_word(&seed)%n;
            switch(trial%3) { case 0:y[boff+changed].codepoint^=1;break; case 1:y[boff+changed].fg^=1;break; default:y[boff+changed].bg^=1; }
        }
        if(cc_arch_equal_run(x+off,y+boff,n)!=cc_equal_run_scalar(x+off,y+boff,n)) { bad=1; break; }
    }
    failures+=bad; if(out) fprintf(out,"  frame-equal %s randomized/alignment/each-field\n",bad?"FAIL":"PASS");
    free(storage); return failures==0;
}
static volatile uint32_t sink;
void cc_arch_bench(FILE *out) {
    void *storage=malloc(2*528+2*44*sizeof(cc_cell)+15);
    if(!storage) { if(out) fputs("carto arch bench unavailable: allocation\n",out); return; }
    uint8_t *a=(uint8_t *)(((uintptr_t)storage+15)&~(uintptr_t)15),*b=a+528;
    cc_cell *x=(cc_cell *)(b+528),*y=x+44; uint32_t seed=73;
    const unsigned iterations=256;
    if(!out) { free(storage); return; }
    fprintf(out,"carto arch bench %u iterations; %s totals (dispatch includes checks)\n",iterations,
#ifdef CC_HAVE_ESP32P4_PIE
            "cycles"
#else
            "host clock ticks"
#endif
    );
    for(unsigned op=0;op<6;op++) for(unsigned offset=0;offset<2;offset++) {
        for(size_t k=0;k<528;k++) { a[k]=(uint8_t)random_word(&seed); b[k]=(uint8_t)random_word(&seed); }
        memcpy(x,a,44*sizeof(*x)); memcpy(y,x,44*sizeof(*y));
        uint64_t start=ticks();
        for(unsigned k=0;k<iterations;k++) {
            if(op<5) { reference(op,a+offset,b+offset,512); sink=a[offset]; }
            else sink=(uint32_t)cc_equal_run_scalar(x+offset,y+offset,40);
        }
        uint64_t c=elapsed(start); start=ticks();
        for(unsigned k=0;k<iterations;k++) {
            if(op<5) { dispatch(op,a+offset,b+offset,512); sink=a[offset]; }
            else sink=(uint32_t)cc_arch_equal_run(x+offset,y+offset,40);
        }
        uint64_t pie=elapsed(start);
        fprintf(out,"  %-11s %s C=%llu dispatch=%llu path=%s\n",names[op],offset?"unaligned":"aligned",
                (unsigned long long)c,(unsigned long long)pie,cc_arch_pie_enabled()?"PIE+tail":"C");
    }
    free(storage);
}
