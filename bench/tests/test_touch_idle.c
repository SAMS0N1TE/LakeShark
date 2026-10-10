#include "ls_test.h"
#include "ls_touch.h"
#include "ls_i2c.h"
#include <string.h>

static uint8_t report[18];
static unsigned reads, bytes, acks;
static bool fail_read, fail_ack;
static void sum(uint8_t *p, unsigned n)
{
    unsigned total = 0;
    for (unsigned i = 0; i < n - 2; ++i) total += p[i];
    p[n - 2] = total; p[n - 1] = total >> 8;
}
esp_err_t ls_i2c_probe(ls_i2c_bus_id_t bus, uint8_t addr, int timeout) { return ESP_OK; }
esp_err_t ls_i2c_device(ls_i2c_bus_id_t bus, uint8_t addr, uint32_t hz,
                       i2c_master_dev_handle_t *out) { *out = (void *)1; return ESP_OK; }
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev) { return ESP_OK; }
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev,
    const uint8_t *tx, size_t tx_n, uint8_t *rx, size_t rx_n, int timeout)
{
    uint32_t reg = (uint32_t)tx[0] << 24 | (uint32_t)tx[1] << 16 | (uint32_t)tx[2] << 8 | tx[3];
    memset(rx, 0, rx_n);
    if (reg == 0x10014) { memcpy(rx + 10, "9895", 4); sum(rx, rx_n); }
    else if (reg == 0x10070) {
        rx[0] = 87;
        if (rx_n > 2) { rx[83] = 2; sum(rx, rx_n); }
    } else {
        LS_EQ_UINT(reg, 0x20000); ++reads; bytes += rx_n;
        if (fail_read) return ESP_FAIL;
        memcpy(rx, report, rx_n);
    }
    return ESP_OK;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev,
    const uint8_t *tx, size_t n, int timeout)
{
    LS_EQ_UINT(n, 5); LS_EQ_INT(tx[4], 0); ++acks;
    if (fail_ack) return ESP_FAIL;
    report[0] = 0; return ESP_OK;
}
LS_CASE(idle_reads_only_ready_byte_and_reports_keep_checksums_and_ack)
{
    LS_EQ_INT(ls_touch_init(), ESP_OK);
    uint16_t x = 0, y = 0; bool down = false;
    for (int i = 0; i < 100; ++i) LS_CHECK(!ls_touch_read(&x, &y, &down));
    LS_EQ_UINT(reads, 100); LS_EQ_UINT(bytes, 100); LS_EQ_UINT(acks, 0);
    report[0] = 0x80; report[2] = 1;
    report[10] = 0x12; report[11] = 2; /* native centre */
    report[12] = 0xb0; report[13] = 4;
    sum(report, 8); sum(report + 8, 10);
    LS_CHECK(ls_touch_read(&x, &y, &down)); LS_CHECK(down);
    LS_EQ_UINT(x, 284); LS_EQ_UINT(y, 616); LS_EQ_UINT(acks, 1);
    report[0] = 0x80; report[2] = 0; sum(report, 8);
    LS_CHECK(ls_touch_read(&x, &y, &down)); LS_CHECK(!down);
    LS_EQ_UINT(x, 284); LS_EQ_UINT(y, 616); LS_EQ_UINT(acks, 2);
    report[0] = 0x80; report[2] = 1; sum(report, 8); report[7] ^= 1;
    LS_CHECK(!ls_touch_read(&x, &y, &down)); LS_EQ_UINT(acks, 2);
    sum(report, 8); report[17] ^= 1;
    LS_CHECK(!ls_touch_read(&x, &y, &down)); LS_EQ_UINT(acks, 2);
    sum(report + 8, 10); fail_ack = true;
    LS_CHECK(!ls_touch_read(&x, &y, &down)); fail_ack = false;
    fail_read = true; LS_CHECK(!ls_touch_read(&x, &y, &down)); fail_read = false;
    LS_CHECK(ls_touch_read(&x, &y, &down)); LS_CHECK(down);
}
