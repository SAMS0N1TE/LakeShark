/* SPDX-License-Identifier: GPL-3.0-or-later
 * GT9895 protocol adapted from LILYGO_L cpp_bus_driver gt9895.cpp.
 * Read-only runtime discovery and single-finger input; no firmware upload.
 */
#include "ls_touch.h"
#include "ls_board.h"
#if defined(LS_BOARD_PANEL_RM69A10)
#include "ls_i2c.h"
#include "esp_log.h"
#include <string.h>
static i2c_master_dev_handle_t s_dev;
static uint32_t s_data;

static uint16_t s_last_x, s_last_y;
static uint16_t le16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | (uint32_t)le16(p+2) << 16; }
static bool checksum(const uint8_t *p, size_t n)
{
    if (n < 2) return false;
    uint16_t sum = 0;
    for (size_t i=0; i<n-2; ++i) sum += p[i];
    return sum == le16(p+n-2);
}
static esp_err_t read_reg(uint32_t a, uint8_t *p, size_t n)
{
    uint8_t reg[] = {a>>24, a>>16, a>>8, a};
    return i2c_master_transmit_receive(s_dev, reg, 4, p, n, 30);
}
esp_err_t ls_touch_init(void)
{
    if (s_data) return ESP_OK;
    esp_err_t e = ls_i2c_probe(LS_I2C_PRIMARY, LS_BOARD_TOUCH_I2C_ADDR, 30);
    if (e != ESP_OK) return e;
    e = ls_i2c_device(LS_I2C_PRIMARY, LS_BOARD_TOUCH_I2C_ADDR, 100000, &s_dev);
    if (e != ESP_OK) return e;
    uint8_t fw[28], info[1024];
    e = read_reg(0x10014, fw, sizeof(fw));
    if (e != ESP_OK) goto fail;
    if (!checksum(fw, sizeof(fw)) || memcmp(fw+10, "9895", 4)) { e=ESP_ERR_INVALID_RESPONSE; goto fail; }
    e = read_reg(0x10070, info, 2);
    if (e != ESP_OK) goto fail;
    size_t n = le16(info);
    if (n < 87 || n > sizeof(info)) { e=ESP_ERR_INVALID_SIZE; goto fail; }
    e = read_reg(0x10070, info, n);
    if (e != ESP_OK) goto fail;
    if (!checksum(info,n)) { e=ESP_ERR_INVALID_CRC; goto fail; }
    size_t off=32;
    for (int i=0; i<5; ++i) {
        if (off >= n-2) { e=ESP_ERR_INVALID_SIZE; goto fail; }
        size_t bytes=info[off++]*2;
        if (bytes > n-2-off) { e=ESP_ERR_INVALID_SIZE; goto fail; }
        off += bytes;
    }
    if (n-2-off < 48) { e=ESP_ERR_INVALID_SIZE; goto fail; }
    s_data=le32(info+off+44);
    if (!s_data) { e=ESP_ERR_INVALID_RESPONSE; goto fail; }
    ESP_LOGI("gt9895", "touch ready at 0x%lx", (unsigned long)s_data);
    return ESP_OK;
fail:
    i2c_master_bus_rm_device(s_dev); s_dev=NULL;
    return e;
}
bool ls_touch_read(uint16_t *x, uint16_t *y, bool *pressed)
{
    if (!s_data) return false;
    uint8_t p[18];
    /* Only a zero status is idle; a ready report with no contacts is a
       release and must still pass the full checksum and ack path below. */
    if (read_reg(s_data,p,1) != ESP_OK || !p[0]) return false;
    if (read_reg(s_data,p,sizeof(p)) != ESP_OK || !p[0] || !checksum(p,8)) return false;
    unsigned count=p[2]&15;
    if (count>10) return false;
    bool down=(p[0]&0x80) && count;
    if (down && count==1 && !checksum(p+8,10)) return false;
    uint8_t ack[]={s_data>>24,s_data>>16,s_data>>8,s_data,0};
    if (i2c_master_transmit(s_dev,ack,sizeof(ack),30)!=ESP_OK) return false;
    if (down) {
        /* Vendor a47cf73e: raw 1060x2400 maps to native 568x1232. */
        unsigned nx=(uint32_t)le16(p+10)*LS_BOARD_LCD_H_RES/1060;
        unsigned ny=(uint32_t)le16(p+12)*LS_BOARD_LCD_V_RES/2400;
        s_last_x=nx<LS_BOARD_LCD_H_RES ? nx : LS_BOARD_LCD_H_RES-1;
        s_last_y=ny<LS_BOARD_LCD_V_RES ? ny : LS_BOARD_LCD_V_RES-1;
    }

    *x=s_last_x;
    *y=s_last_y;
    *pressed=down;
    return true;
}
#elif defined(LS_BOARD_PANEL_HI8561)
/* HI8561 touch (TFT SKU), adapted from LILYGO_L cpp_bus_driver
 * hi8561_touch.cpp. The firmware publishes a section table in ERAM; the
 * coordinate report's address is read from it, then polled. Read-only apart
 * from the backdoor enter/exit around the optional resolution read. */
#include "ls_i2c.h"
#include "ls_board_hw.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#define ERAM            0x20011000u
#define ERAM_SIZE       0x1000u
#define DSRAM_TABLE     (ERAM + 4)
#define ESRAM_COUNT     (DSRAM_TABLE + 25 * 8)
#define ESRAM_TABLE     (ESRAM_COUNT + 4)
#define MAX_CONTACTS    10
static i2c_master_dev_handle_t s_dev;
static uint32_t s_report;
static uint16_t s_res_x, s_res_y;
static uint16_t s_last_x, s_last_y;
static uint16_t le16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | (uint32_t)le16(p+2) << 16; }
static esp_err_t read_mem(uint32_t a, uint8_t *p, size_t n)
{
    const uint8_t cmd[] = {0xF3, a>>24, a>>16, a>>8, a, 0x03};
    return i2c_master_transmit_receive(s_dev, cmd, sizeof(cmd), p, n, 30);
}
static esp_err_t read_eram(uint32_t a, uint8_t *p, size_t n)
{
    if (a < ERAM || a + n > ERAM + ERAM_SIZE) return ESP_ERR_INVALID_ARG;
    return read_mem(a, p, n);
}
static esp_err_t section(uint32_t table, unsigned index, uint32_t *addr, uint32_t *len)
{
    uint8_t d[8];
    esp_err_t e = read_eram(table + index * 8, d, sizeof(d));
    if (e == ESP_OK) { *addr = le32(d); *len = le32(d+4); }
    return e;
}
static esp_err_t backdoor(bool on)
{
    static const uint8_t enter[] = {0xF2, 0xAA, 0xF0, 0x0F, 0x55, 0x68};
    static const uint8_t leave[] = {0xF2, 0xAA, 0x88, 0x00, 0x00, 0x00};
    return i2c_master_transmit(s_dev, on ? enter : leave, sizeof(enter), 30);
}
/* The coordinate range the firmware reports in, from its config section.
   Left at 0 (native pixels assumed) if any step fails. */
static void read_resolution(unsigned dsram_count)
{
    uint32_t addr, len;
    uint8_t cfg[6];
    if (dsram_count <= 1 || section(DSRAM_TABLE, 1, &addr, &len) != ESP_OK ||
        !addr || len < sizeof(cfg) || backdoor(true) != ESP_OK) return;
    esp_err_t e = read_mem(addr, cfg, sizeof(cfg));
    if (backdoor(false) != ESP_OK || e != ESP_OK) return;
    if (le16(cfg) && le16(cfg+2)) { s_res_x = le16(cfg); s_res_y = le16(cfg+2); }
}
esp_err_t ls_touch_init(void)
{
    if (s_report) return ESP_OK;
    /* Same die as the panel: reset the touch side again now the display is
       running, as the vendor's InitHi8561Touch does after InitScreen. */
    esp_err_t e = ls_board_hw_touch_reset();
    if (e != ESP_OK) return e;
    e = ls_i2c_probe(LS_I2C_PRIMARY, LS_BOARD_TOUCH_I2C_ADDR, 30);
    if (e != ESP_OK) return e;
    e = ls_i2c_device(LS_I2C_PRIMARY, LS_BOARD_TOUCH_I2C_ADDR, 400000, &s_dev);
    if (e != ESP_OK) return e;
    uint8_t d[4];
    bool ready = false;
    for (int i = 0; i < 25 && !ready; ++i) {
        e = read_eram(ERAM, d, 2);
        if (e != ESP_OK) goto fail;
        ready = le16(d) == 0xA55A;
        if (!ready) vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (!ready) { e = ESP_ERR_TIMEOUT; goto fail; }
    if ((e = read_eram(ERAM + 2, d, 4)) != ESP_OK) goto fail;
    const unsigned dsram_count = le16(d);
    if ((e = read_eram(ESRAM_COUNT, d, 4)) != ESP_OK) goto fail;
    const unsigned esram_count = le16(d);
    if (dsram_count <= 3 || dsram_count > 25 || esram_count <= 1 || esram_count > 10) {
        e = ESP_ERR_INVALID_RESPONSE; goto fail;
    }
    uint32_t addr, len;
    if ((e = section(ESRAM_TABLE, 1, &addr, &len)) != ESP_OK) goto fail;
    if (len < 8 || addr < ERAM || addr + 8 > ERAM + ERAM_SIZE) {
        e = ESP_ERR_INVALID_SIZE; goto fail;
    }
    s_report = addr;
    read_resolution(dsram_count);
    ESP_LOGI("hi8561", "touch ready at 0x%lx, range %ux%u", (unsigned long)s_report,
             s_res_x ? s_res_x : LS_BOARD_LCD_H_RES, s_res_y ? s_res_y : LS_BOARD_LCD_V_RES);
    return ESP_OK;
fail:
    i2c_master_bus_rm_device(s_dev); s_dev=NULL;
    return e;
}
static uint16_t scale(uint16_t v, uint16_t range, int native)
{
    uint32_t n = range ? (uint32_t)v * native / range : v;
    return n < (uint32_t)native ? n : native - 1;
}
bool ls_touch_read(uint16_t *x, uint16_t *y, bool *pressed)
{
    if (!s_report) return false;
    /* count, sequence, gesture, then the first contact: x, y big-endian,
       pressure. All-ones coordinates mark an edge contact, not a point. */
    uint8_t p[8];
    if (read_eram(s_report, p, sizeof(p)) != ESP_OK || p[0] > MAX_CONTACTS) return false;
    const uint16_t rx = (uint16_t)p[3] << 8 | p[4], ry = (uint16_t)p[5] << 8 | p[6];
    const bool down = p[0] && !(rx == 0xFFFF && ry == 0xFFFF);
    if (down) {
        s_last_x = scale(rx, s_res_x, LS_BOARD_LCD_H_RES);
        s_last_y = scale(ry, s_res_y, LS_BOARD_LCD_V_RES);
    }
    *x=s_last_x;
    *y=s_last_y;
    *pressed=down;
    return true;
}
#else
esp_err_t ls_touch_init(void) { return ESP_ERR_NOT_SUPPORTED; }
bool ls_touch_read(uint16_t *x, uint16_t *y, bool *pressed) { (void)x;(void)y;(void)pressed; return false; }
#endif
