/* Private transport implementation, also compiled by the host split test. */
#pragma once

/* Ported from MIT lr2021-sdr radio.c bus_transfer: DMA the 64-byte
   multiple of a 256..1024-byte FIFO frame, then CPU-register tail, with CS
   asserted throughout. Packet/TX commands retain their existing framing. */
static esp_err_t IRAM_ATTR capture_fifo_transmit(uint8_t *io, size_t n)
{
    if (!ls_lora_hw_locked_by_me() || !s_capture_fast) return ESP_ERR_INVALID_STATE;
    if (!io || n < 3 || n > 1022 || io[0] != 0 || io[1] != 1) return ESP_ERR_INVALID_ARG;
#if defined(ESP_PLATFORM) && CONFIG_IDF_TARGET_ESP32P4
    size_t off = 0;
    s_spi_bytes += n;
    /* The buffers exist only if DMA was on when the capture began. */
    if (n >= 256 && s_capture_dma && s_capture_tx && s_capture_rx) {
        off = n & ~(size_t)63;
        memcpy(s_capture_tx, io, off);
        spi_transaction_t t = {
            .flags = SPI_TRANS_DMA_BUFFER_ALIGN_MANUAL | (off < n ? SPI_TRANS_CS_KEEP_ACTIVE : 0),
            .length = off * 8, .tx_buffer = s_capture_tx, .rx_buffer = s_capture_rx,
            .override_freq_hz = s_capture_hz,
        };
        esp_err_t err = spi_device_polling_transmit(s_dev, &t);
        if (err != ESP_OK) return err;
        memcpy(io, s_capture_rx, off);
    }
    for (; off < n; off += 64) {
        const size_t m = n - off > 64 ? 64 : n - off;
        spi_transaction_t t = {
            .flags = off + m < n ? SPI_TRANS_CS_KEEP_ACTIVE : 0,
            .length = m * 8, .tx_buffer = io + off, .rx_buffer = io + off,
        };
        esp_err_t err = capture_cpu_transmit(&t);
        if (err != ESP_OK) return err;
    }
    s_capture_clock_pending = 0;
    return ESP_OK;
#else
    /* Host protocol fakes and non-P4 targets see the complete wire frame. */
    spi_transaction_t t = { .length = n * 8, .tx_buffer = io, .rx_buffer = io };
    return ls_lora_hw_transmit(&t);
#endif
}
