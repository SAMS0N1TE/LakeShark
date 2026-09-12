/* LS_TEST_LINK: -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=free */
/* LS_TEST_SOURCES: ${APP}/p25/diag.c */
/**/

#include "ls_test.h"
#include "diag.h"
#include "p25_state.h"
#include "driver/uart.h"
#include "freertos/ringbuf.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>

extern void *__real_malloc(size_t size);
extern void *__real_calloc(size_t count, size_t size);
extern void __real_free(void *ptr);

static unsigned g_alloc_calls;
static long g_live_allocs;

/* These fakes make the case runnable against the formerly broken module too:
   its disabled init reached the ring allocator, while its compiled (but
   unstarted) UART task and periodic emitter still needed linkable endpoints. */
p25_state_t P25;
uint32_t s_tune_freq_hz;
dsp_state_t s_dsp;

void sys_log(uint8_t color, const char *fmt, ...)
{
    (void)color;
    (void)fmt;
}

void *__wrap_malloc(size_t size)
{
    void *ptr = __real_malloc(size);
    if (ptr) {
        ++g_alloc_calls;
        ++g_live_allocs;
    }
    return ptr;
}

void *__wrap_calloc(size_t count, size_t size)
{
    void *ptr = __real_calloc(count, size);
    if (ptr) {
        ++g_alloc_calls;
        ++g_live_allocs;
    }
    return ptr;
}

void __wrap_free(void *ptr)
{
    if (ptr) --g_live_allocs;
    __real_free(ptr);
}

RingbufHandle_t xRingbufferCreate(size_t buffer_size,
                                  RingbufferType_t buffer_type)
{
    (void)buffer_type;
    return malloc(buffer_size);
}

BaseType_t xRingbufferSend(RingbufHandle_t ring, const void *item,
                           size_t item_size, TickType_t ticks)
{
    (void)ring;
    (void)item;
    (void)item_size;
    (void)ticks;
    return pdTRUE;
}

void *xRingbufferReceive(RingbufHandle_t ring, size_t *item_size,
                         TickType_t ticks)
{
    (void)ring;
    (void)item_size;
    (void)ticks;
    return NULL;
}

void vRingbufferReturnItem(RingbufHandle_t ring, void *item)
{
    (void)ring;
    (void)item;
}

int uart_write_bytes(int uart_num, const void *source, size_t size)
{
    (void)uart_num;
    (void)source;
    return (int)size;
}

static void call_diag_vline(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    diag_vline(tag, fmt, ap);
    va_end(ap);
}

LS_CASE(disabled_diagnostics_do_not_allocate_or_format)
{
    const unsigned alloc_start = g_alloc_calls;
    const long live_start = g_live_allocs;
    int formatted = -1;
    int dibits[33] = {0};

    diag_init();
    diag_line("TEST", "line formatted%n", &formatted);
    call_diag_vline("TEST", "vline formatted%n", &formatted);
    diag_dump_nid("NID", dibits, 0x293, "00", 0, 1, "test");
    diag_emit_periodic();

    LS_EQ_UINT(g_alloc_calls, alloc_start);
    LS_EQ_INT(g_live_allocs, live_start);
    LS_EQ_INT(formatted, -1);
}
