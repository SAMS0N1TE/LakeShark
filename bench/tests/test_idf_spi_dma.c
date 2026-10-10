/* This harness includes the SDK functions verbatim; allocator/cache faults
   exercise ownership and reply preservation rather than a reimplementation. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SPI_MASTER_ISR_ATTR
#define SPI_TRANS_USE_RXDATA 4
#define SPI_TRANS_USE_TXDATA 8
#define SPI_TRANS_DMA_USE_PSRAM 16
#define SPI_TRANS_DMA_BUFFER_ALIGN_MANUAL 32
#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_SPIRAM 4
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_NOT_SUPPORTED 3
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 1
#define ESP_CACHE_MSYNC_FLAG_UNALIGNED 2
#define ESP_CACHE_MSYNC_FLAG_DIR_M2C 4
#define MAX(a,b) ((a)>(b)?(a):(b))
#define ESP_EARLY_LOGV(...) ((void)0)
#define ESP_RETURN_ON_FALSE_ISR(cond,err,...) do { if (!(cond)) return err; } while(0)
typedef int esp_err_t;
typedef struct {
    uint32_t flags, length, rxlength;
    union { const void *tx_buffer; uint8_t tx_data[4]; };
    union { void *rx_buffer; uint8_t rx_data[4]; };
} spi_transaction_t;
typedef struct { spi_transaction_t *trans; const uint32_t *buffer_to_send; uint32_t *buffer_to_rcv; } spi_trans_priv_t;
typedef struct { bool dma_enabled; unsigned cache_align_ext, cache_align_int; } spi_bus_attr_t;
typedef struct { unsigned dma_align_tx_ext, dma_align_tx_int, dma_align_rx_ext, dma_align_rx_int; } dma_t;
typedef struct { int id; dma_t *dma_ctx; const spi_bus_attr_t *bus_attr; } spi_host_t;
static int allocations, outstanding, fail_at, sync_calls, fail_sync;
static bool bounce;
static void *owned[4];
static bool esp_ptr_external_ram(void *p) { (void)p; return false; }
static bool esp_ptr_dma_ext_capable(void *p) { (void)p; return false; }
static bool esp_ptr_dma_capable(void *p) { (void)p; return !bounce; }
static void *heap_caps_aligned_alloc(unsigned align, unsigned len, unsigned caps) {
    (void)align; (void)caps;
    if (++allocations == fail_at) return NULL;
    void *p = malloc(len); assert(p); memset(p, 0xCC, len);
    owned[outstanding++] = p; return p;
}
static void release(void *p) {
    if (!p) return;
    for (int i=0; i<outstanding; i++) if (owned[i] == p) {
        owned[i] = owned[--outstanding]; free(p); return;
    }
    assert(!"free of caller buffer");
}
static int esp_cache_msync(void *p, unsigned len, unsigned flags) {
    (void)p; (void)len; (void)flags;
    return ++sync_calls == fail_sync ? ESP_ERR_INVALID_ARG : ESP_OK;
}
#define free release
#include "spi_under_test.h"
#undef free
static void reset(void) {
    assert(outstanding == 0); allocations=sync_calls=fail_at=fail_sync=0; bounce=true;
}
int main(void) {
    dma_t dma = {4,4,4,4}; spi_bus_attr_t bus = {true,4,1};
    spi_host_t host = {0,&dma,&bus};
    uint32_t tx = 0x12345678, rx = 0xA5A5A5A5;
    spi_transaction_t t = {.length=24,.rxlength=24,.tx_buffer=&tx,.rx_buffer=&rx};
    spi_trans_priv_t p;
    for (int failed=1; failed<=2; failed++) {
        reset(); fail_at=failed; p=(spi_trans_priv_t){.trans=&t};
        assert(setup_priv_desc(&host,&p)==ESP_ERR_NO_MEM);
        assert(outstanding==0 && rx==0xA5A5A5A5);
    }
    for (int failed=1; failed<=2; failed++) {
        reset(); bus.cache_align_int=4; fail_sync=failed;
        p=(spi_trans_priv_t){.trans=&t};
        assert(setup_priv_desc(&host,&p)==ESP_ERR_INVALID_ARG);
        assert(outstanding==0 && rx==0xA5A5A5A5);
    }
    reset(); bus.cache_align_int=1; p=(spi_trans_priv_t){.trans=&t};
    assert(setup_priv_desc(&host,&p)==ESP_OK && outstanding==2);
    /* TX padding must not be read from the caller beyond its three bytes. */
    assert(((uint8_t*)p.buffer_to_send)[3]==0xCC);
    memset(p.buffer_to_rcv,0x42,3); uninstall_priv_desc(&p);
    assert(outstanding==0 && rx==0xA5424242);
    reset(); bounce=false; t.flags=SPI_TRANS_USE_TXDATA|SPI_TRANS_USE_RXDATA;
    p=(spi_trans_priv_t){.trans=&t};
    assert(setup_priv_desc(&host,&p)==ESP_OK && allocations==0);
    uninstall_priv_desc(&p); assert(outstanding==0);
    /* Reproduce stock cleanup with a NULL unpublished RX private buffer. */
    t.flags=0; t.rx_buffer=&rx; p=(spi_trans_priv_t){.trans=&t};
    uninstall_priv_desc(&p); assert(rx==0xA5424242);
    puts("PASS SPI DMA: TX/RX allocation and cache failures, no leak/copy-back, inline no-bounce, NULL cleanup");
}
