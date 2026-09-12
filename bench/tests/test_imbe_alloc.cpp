extern "C" {
#include "ls_test.h"
#include "esp_heap_caps.h"
#include "imbe_shim.h"
}
#include "imbe_vocoder_impl.h"
#include <cstdlib>
#include <new>
#include <cstring>
#include <cstdio>
static bool reject_default_new;
static size_t rejected_size;
void *operator new(size_t n)
{
    if (reject_default_new) { rejected_size=n; throw std::bad_alloc(); }
    void *p=std::malloc(n); if (!p) throw std::bad_alloc(); return p;
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }
LS_CASE(failed_initialization_is_silent_bounded_and_retryable)
{
    uint8_t bits[11]={0}; int16_t pcm[160];
    std::memset(pcm,0x55,sizeof(pcm));
    ls_shim_heap_reset(); ls_shim_heap_fail_from(1);
    reject_default_new=true; bool threw=false;
    try { imbe_shim_decode_88(bits,pcm); } catch (const std::bad_alloc &) { threw=true; }
    std::printf("default-new failure escaped=%d size=%zu\n",threw,rejected_size);
    LS_CHECK(!threw);
    for (int16_t sample: pcm) LS_EQ_INT(sample,0);
    LS_EQ_UINT(ls_shim_heap_outstanding(),0);
    LS_EQ_UINT(ls_shim_heap_call_caps(1),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    LS_EQ_UINT(ls_shim_heap_call_size(1),sizeof(imbe_vocoder_impl));
    LS_CHECK(!imbe_shim_try_decode_88(bits,pcm));
    LS_CHECK(!imbe_shim_try_decode_88(NULL,pcm));
    LS_CHECK(!imbe_shim_try_decode_88(bits,NULL));
    ls_shim_heap_fail_from(0);
    LS_CHECK(imbe_shim_try_decode_88(bits,pcm));
    LS_EQ_UINT(ls_shim_heap_outstanding(),1);
    unsigned calls=ls_shim_heap_call_count();
    imbe_shim_decode_88(bits,pcm); LS_EQ_UINT(ls_shim_heap_call_count(),calls);
    LS_EQ_UINT(rejected_size,0);
    reject_default_new=false;
}
