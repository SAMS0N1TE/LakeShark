/* Private P4 CPU transport; caller owns the acquired radio bus. */
#pragma once

/* IDF 5.5.4 DMA is host-wide, even for USE_RXDATA/TXDATA. Its
   setup_dma_priv_buffer calls msync whenever cache_align_int > 1 on P4.
   Use the CPU registers instead, retaining the driver's primed device timing.
   No allocation, DMA descriptors, cache operations or flash calls here. */
static esp_err_t IRAM_ATTR capture_cpu_transmit(spi_transaction_t *t)
{
    spi_dev_t *hw = SPI_LL_GET_HW(SPI2_HOST);
    const uint8_t *tx = (t->flags & SPI_TRANS_USE_TXDATA) ? t->tx_data : t->tx_buffer;
    uint8_t *rx = (t->flags & SPI_TRANS_USE_RXDATA) ? t->rx_data : t->rx_buffer;
    size_t n = t->length / 8;
    if (!tx || !rx || !n || n > 64 || (t->length & 7)) return ESP_ERR_INVALID_SIZE;
    spi_ll_dma_rx_enable(hw, false);
    spi_ll_dma_tx_enable(hw, false);
    spi_ll_clear_int_stat(hw);
    spi_ll_enable_mosi(hw, true);
    spi_ll_enable_miso(hw, true);
    spi_ll_set_mosi_bitlen(hw, t->length);
    spi_ll_set_miso_bitlen(hw, t->length);
#ifdef SPI_TRANS_CS_KEEP_ACTIVE
    spi_ll_master_keep_cs(hw, (t->flags & SPI_TRANS_CS_KEEP_ACTIVE) != 0);
#else
    spi_ll_master_keep_cs(hw, false);
#endif
    /* LL's word writer reads a padded final word; both transport buffers have
       that padding (inline 4 bytes or the 260-byte packet allocation). */
    spi_ll_write_buffer(hw, tx, t->length);
    spi_ll_apply_config(hw);
    spi_ll_user_start(hw);
    while (!spi_ll_usr_is_done(hw)) { }
    spi_ll_read_buffer(hw, rx, t->length);
    return ESP_OK;
}
