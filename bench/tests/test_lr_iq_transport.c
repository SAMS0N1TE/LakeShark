/* Compile the production P4 FIFO split against an explicit CS/wire model. */
#include "ls_test.h"
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#define ESP_PLATFORM 1
#define CONFIG_IDF_TARGET_ESP32P4 1
#define IRAM_ATTR
#define SPI_TRANS_CS_KEEP_ACTIVE 1u
#define SPI_TRANS_DMA_BUFFER_ALIGN_MANUAL 2u
typedef struct {
    unsigned flags;
    size_t length;
    const void *tx_buffer;
    void *rx_buffer;
    uint32_t override_freq_hz;
} spi_transaction_t;
static bool locked = true, s_capture_fast = true, s_capture_dma = true;
static bool ls_lora_hw_locked_by_me(void) { return locked; }
static uint8_t tx_dma[1024] __attribute__((aligned(64)));
static uint8_t rx_dma[1024] __attribute__((aligned(64)));
static uint8_t *s_capture_tx = tx_dma, *s_capture_rx = rx_dma;
static uint32_t s_spi_bytes, s_capture_hz = 26666666, s_capture_clock_pending;
static void *s_dev = (void *)1;
static unsigned dma_calls, cpu_calls;
static size_t wire_bytes, frame_bytes;
static bool cs_active, dma_fail;

static esp_err_t wire(spi_transaction_t *t)
{
    const size_t n = t->length / 8;
    LS_CHECK(n && !(t->length & 7));
    if (wire_bytes) LS_CHECK(cs_active);
    const uint8_t *tx = t->tx_buffer;
    uint8_t *rx = t->rx_buffer;
    for (size_t i = 0; i < n; ++i) rx[i] = tx[i] ^ 0xA5;
    wire_bytes += n;
    cs_active = (t->flags & SPI_TRANS_CS_KEEP_ACTIVE) != 0;
    LS_EQ_INT(cs_active, wire_bytes < frame_bytes);
    return ESP_OK;
}
static esp_err_t spi_device_polling_transmit(void *dev, spi_transaction_t *t)
{
    LS_CHECK(dev == s_dev);
    LS_CHECK(t->flags & SPI_TRANS_DMA_BUFFER_ALIGN_MANUAL);
    LS_CHECK(((uintptr_t)t->tx_buffer & 63) == 0);
    LS_CHECK(((uintptr_t)t->rx_buffer & 63) == 0);
    LS_CHECK(t->length / 8 >= 256 && (t->length / 8 & 63) == 0);
    LS_EQ_UINT(t->override_freq_hz, s_capture_hz);
    dma_calls++;
    return dma_fail ? ESP_FAIL : wire(t);
}
static esp_err_t capture_cpu_transmit(spi_transaction_t *t)
{
    LS_CHECK(t->length / 8 <= 64);
    cpu_calls++;
    return wire(t);
}
#include "ls_lora_spi_capture.h"

LS_CASE(every_fifo_frame_length_preserves_one_cs_and_all_received_bytes)
{
    for (unsigned clock = 0; clock < 2; ++clock) {
        s_capture_hz = clock ? 16000000 : 26666666;
        for (size_t n = 3; n <= 1022; ++n) {
            uint8_t data[1024], saved[1024];
            for (size_t i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)(i * 73);
            data[0] = 0; data[1] = 1;
            memcpy(saved, data, sizeof(data));
            dma_calls = cpu_calls = 0; wire_bytes = 0; frame_bytes = n;
            cs_active = false; s_spi_bytes = 0; s_capture_clock_pending = s_capture_hz;
            LS_EQ_INT(capture_fifo_transmit(data, n), ESP_OK);
            LS_EQ_UINT(dma_calls, n >= 256 ? 1 : 0);
            LS_EQ_UINT(cpu_calls, n >= 256 ? !!(n & 63) : (n + 63) / 64);
            LS_EQ_UINT(wire_bytes, n); LS_EQ_UINT(s_spi_bytes, n);
            LS_CHECK(!cs_active); LS_EQ_UINT(s_capture_clock_pending, 0);
            for (size_t i = 0; i < n; ++i) LS_EQ_INT(data[i], saved[i] ^ 0xA5);
            LS_CHECK(memcmp(data + n, saved + n, sizeof(data) - n) == 0);
        }
    }
}

LS_CASE(fifo_transport_refuses_bad_state_size_and_non_fifo_commands)
{
    uint8_t data[1024] = {0, 1};
    locked = false;
    LS_EQ_INT(capture_fifo_transmit(data, 1022), ESP_ERR_INVALID_STATE);
    locked = true; s_capture_fast = false;
    LS_EQ_INT(capture_fifo_transmit(data, 1022), ESP_ERR_INVALID_STATE);
    s_capture_fast = true;
    LS_EQ_INT(capture_fifo_transmit(data, 2), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(capture_fifo_transmit(data, 1023), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(capture_fifo_transmit(NULL, 256), ESP_ERR_INVALID_ARG);
    data[1] = 2;
    LS_EQ_INT(capture_fifo_transmit(data, 256), ESP_ERR_INVALID_ARG);
}

/* lora lrdma off: no DMA, so no esp_cache_msync and its global mutex; one
   command frame still, in 64-byte CPU pieces with CS held throughout. */
LS_CASE(cpu_only_frames_keep_one_cs_and_every_byte)
{
    s_capture_dma = false;
    for (size_t n = 3; n <= 1022; ++n) {
        uint8_t data[1024], saved[1024];
        for (size_t i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)(i * 29);
        data[0] = 0; data[1] = 1;
        memcpy(saved, data, sizeof(data));
        dma_calls = cpu_calls = 0; wire_bytes = 0; frame_bytes = n;
        cs_active = false; s_spi_bytes = 0;
        LS_EQ_INT(capture_fifo_transmit(data, n), ESP_OK);
        LS_EQ_UINT(dma_calls, 0); LS_EQ_UINT(cpu_calls, (n + 63) / 64);
        LS_EQ_UINT(wire_bytes, n); LS_CHECK(!cs_active);
        for (size_t i = 0; i < n; ++i) LS_EQ_INT(data[i], saved[i] ^ 0xA5);
    }
    s_capture_dma = true;
}

LS_CASE(dma_failure_stops_before_any_cpu_tail)
{
    uint8_t data[1024] = {0, 1};
    dma_calls = cpu_calls = 0; dma_fail = true;
    LS_EQ_INT(capture_fifo_transmit(data, 1022), ESP_FAIL);
    LS_EQ_UINT(dma_calls, 1); LS_EQ_UINT(cpu_calls, 0);
    dma_fail = false;
}
