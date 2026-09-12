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
#else
esp_err_t ls_touch_init(void) { return ESP_ERR_NOT_SUPPORTED; }
bool ls_touch_read(uint16_t *x, uint16_t *y, bool *pressed) { (void)x;(void)y;(void)pressed; return false; }
#endif
