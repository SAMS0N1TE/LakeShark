/* SPDX-License-Identifier: GPL-3.0-or-later
 * RM69A10 command sequence adapted from LILYGO_L's GPL-3.0 rm69a10.h in
 * cpp_bus_driver (T-Display-P4 vendor reference). No vendor framework needed.
 * HI8561 (TFT SKU) sequence adapted from the same library's GPL-3.0
 * hi8561.h; its backlight is the PT4103 on LS_BOARD_LCD_BL_GPIO.
 * Native panel bring-up and the TUI's framebuffer. Color bars remain the
 * diagnostic fallback until the TUI takes the framebuffer.
 */
#include "ls_panel.h"
#include "ls_board.h"

#if defined(LS_BOARD_PANEL_RM69A10) || defined(LS_BOARD_PANEL_HI8561)
#if defined(LS_BOARD_PANEL_HI8561)
#define PANEL_TAG "hi8561"
#include "driver/ledc.h"
#else
#define PANEL_TAG "rm69a10"
#endif
#include "ls_xl9535.h"
#include "ls_board_hw.h"
#include "esp_attr.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/mipi_dsi_ll.h"
#include "ls_panel_dsi_id.h"
#include "cell_performance.h"
#include <string.h>
#include <stdio.h>

static esp_lcd_dsi_bus_handle_t s_bus;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static esp_ldo_channel_handle_t s_ldo3, s_ldo4;
/* The DPI panel is created with two framebuffers and the TUI paints
   the second: presenting it makes it the scan target, and nothing moves it
   after that. Drawing straight into the buffer being scanned was measured
   against swapping whole frames in - 44.4 ms a flush to 6.1 ms in
   portrait - and the swap path, which only the LVGL flush ever used, went
   with LVGL (). */
static uint16_t *s_frames[2];
static const unsigned s_back=1;
static uint32_t s_refresh_count;
static uint32_t s_last_refresh_us, s_max_gap_us, s_late_frames;
static uint32_t s_max_gap_at_us;

/* the Flipper can start P25 while HOME stays visible. Count scan
 * gaps here, where every TUI screen is covered, instead of in the retired
 * LVGL shell. A late callback is evidence of a gap, not proof of its cause. */
#define PANEL_FRAME_PIXELS ((uint32_t)((uint64_t)(LS_BOARD_LCD_H_RES + \
    LS_BOARD_LCD_HSYNC + LS_BOARD_LCD_HBP + LS_BOARD_LCD_HFP) * \
    (LS_BOARD_LCD_V_RES + LS_BOARD_LCD_VSYNC + LS_BOARD_LCD_VBP + \
    LS_BOARD_LCD_VFP)))
static unsigned s_pixel_clock_mhz = LS_BOARD_LCD_DPI_CLK_MHZ;
static uint32_t s_frame_us = PANEL_FRAME_PIXELS / LS_BOARD_LCD_DPI_CLK_MHZ;

static bool IRAM_ATTR refresh_done(esp_lcd_panel_handle_t panel,
    esp_lcd_dpi_panel_event_data_t *event,void *ctx)
{
    (void)panel; (void)event; (void)ctx;
    uint32_t now = (uint32_t)esp_timer_get_time();
    uint32_t previous = s_last_refresh_us;
    s_last_refresh_us = now;
    if (previous) {
        uint32_t gap = now - previous;
        if (gap > __atomic_load_n(&s_max_gap_us, __ATOMIC_RELAXED)) {
            /* This ISR also runs with flash/cache disabled. Do not inspect
             * the interrupted task's name/TCB, which may live in PSRAM. */
            __atomic_store_n(&s_max_gap_at_us, now, __ATOMIC_RELAXED);
            __atomic_store_n(&s_max_gap_us, gap, __ATOMIC_RELAXED);
        }
        if (gap > s_frame_us * 3 / 2)
            __atomic_add_fetch(&s_late_frames, 1, __ATOMIC_RELAXED);
    }
    __atomic_add_fetch(&s_refresh_count,1,__ATOMIC_RELEASE);
    return false;
}

bool ls_panel_fb(ls_panel_fb_t *out)
{
    if (!out || !s_frames[s_back]) return false;
    out->pixels = s_frames[s_back];
    out->width  = LS_BOARD_LCD_H_RES;
    out->height = LS_BOARD_LCD_V_RES;
    return true;
}

void ls_panel_fb_present(void)
{
    ls_panel_fb_present_rows(0, LS_BOARD_LCD_V_RES);
}

void ls_panel_fb_present_rows(int y0, int y1)
{
    if (!s_panel || !s_frames[s_back]) return;
    if (y0 < 0) y0 = 0;
    if (y1 > LS_BOARD_LCD_V_RES) y1 = LS_BOARD_LCD_V_RES;
    if (y1 <= y0) return;
    /* The pixels are already in the buffer being scanned; this writes the
       cache back so the scan sees them. The DPI driver only uses the pointer
       to find which framebuffer it is in and writes back just rows y0..y1. */
    esp_lcd_panel_draw_bitmap(s_panel, 0, y0,
                              LS_BOARD_LCD_H_RES, y1,
                              s_frames[s_back]);
    static uint32_t reported, last_report_ms;
    uint32_t late = __atomic_load_n(&s_late_frames, __ATOMIC_RELAXED);
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (late != reported && now_ms - last_report_ms >= 1000) {
        ESP_LOGW(PANEL_TAG, "refresh gaps: total=%lu max=%lu us expected=%lu us at=%lu us",
                 (unsigned long)late,
                 (unsigned long)__atomic_load_n(&s_max_gap_us, __ATOMIC_RELAXED),
                 (unsigned long)s_frame_us,
                 (unsigned long)__atomic_load_n(&s_max_gap_at_us,__ATOMIC_RELAXED));
        reported = late;
        last_report_ms = now_ms;
    }
}

/* The refresh count is the one number that says the scan is running
   at all: a panel showing a frozen picture and a panel whose DMA has stopped
   look the same from the outside. */
void ls_panel_diagnostics(void)
{
    const uint32_t a = __atomic_load_n(&s_refresh_count, __ATOMIC_ACQUIRE);
    vTaskDelay(pdMS_TO_TICKS(1000));
    const uint32_t b = __atomic_load_n(&s_refresh_count, __ATOMIC_ACQUIRE);
    printf("panel: %lu refreshes, %lu in the last second, framebuffer %s\n",
           (unsigned long)b, (unsigned long)(b - a),
           s_frames[s_back] ? "ready" : "missing");
    printf("panel: late=%lu max_gap=%lu us expected=%lu us at=%lu us\n",
           (unsigned long)__atomic_load_n(&s_late_frames, __ATOMIC_RELAXED),
           (unsigned long)__atomic_load_n(&s_max_gap_us, __ATOMIC_RELAXED),
           (unsigned long)s_frame_us,
           (unsigned long)__atomic_load_n(&s_max_gap_at_us,__ATOMIC_RELAXED));
    printf("panel: pixel_clock=%u MHz performance=%u\n",s_pixel_clock_mhz,cell_performance_active());
}

#if defined(LS_BOARD_PANEL_HI8561)
/* Vendor hi8561.h kInitSequence, as { command, length, parameters... }.
   Page select is 0xDE; 0xCC 0x31 selects two lanes. Sleep-out and display-on
   follow in hi8561_init. */
static const uint8_t HI8561_INIT[] = {
    0xDF, 3, 0x90, 0x69, 0xF9,
    0xDE, 1, 0x00,
    0xBB, 7, 0x0F, 0x10, 0x43, 0x50, 0x32, 0x44, 0x44,
    0xBF, 2, 0x46, 0x32,
    0xC0, 4, 0x01, 0xAD, 0x01, 0xAD,
    0xBD, 2, 0x00, 0xB4,
    0xC6, 23,
    0x00, 0x7D, 0x00, 0xC8, 0x00, 0x17, 0x1A, 0x82, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01,
    0xC8, 3, 0x23, 0x48, 0x87,
    0xCC, 1, 0x31,
    0xBC, 3, 0x2E, 0x80, 0x84,
    0xC3, 25,
    0x3B, 0x01, 0x02, 0x05, 0x0C, 0x0C, 0x75, 0x0A, 0x79, 0x0A, 0x79, 0x02,
    0x6E, 0x02, 0x6E, 0x02, 0x6E, 0x0A, 0x0D, 0x0A, 0x0F, 0x0A, 0x0F, 0x0A,
    0x0F,
    0xC4, 24,
    0x01, 0x02, 0x05, 0x0C, 0x0C, 0x75, 0x0A, 0x79, 0x0A, 0x79, 0x02, 0x6E,
    0x02, 0x6E, 0x02, 0x6E, 0x0A, 0x0D, 0x0A, 0x0F, 0x0A, 0x0F, 0x0A, 0x0F,
    0xC5, 23,
    0x03, 0x05, 0x0C, 0x0C, 0x75, 0x0A, 0x79, 0x0A, 0x79, 0x02, 0x6E, 0x02,
    0x6E, 0x02, 0x6E, 0x0A, 0x0D, 0x0A, 0x0F, 0x0A, 0x0F, 0x0A, 0x0F,
    0xD7, 17,
    0x00, 0x0A, 0x63, 0x0A, 0x63, 0x0A, 0x63, 0x0A, 0x63, 0x0A, 0x63, 0x0A,
    0x63, 0x0A, 0x63, 0x0A, 0x63,
    0xCB, 43,
    0x7F, 0x78, 0x71, 0x64, 0x5A, 0x58, 0x4B, 0x51, 0x3A, 0x53, 0x51, 0x4F,
    0x6A, 0x54, 0x57, 0x46, 0x3F, 0x2F, 0x1B, 0x0F, 0x08, 0x7F, 0x78, 0x71,
    0x64, 0x5A, 0x58, 0x4B, 0x51, 0x3A, 0x53, 0x51, 0x4F, 0x6A, 0x54, 0x57,
    0x46, 0x3F, 0x2F, 0x1B, 0x0F, 0x08, 0x00,
    0xCE, 23,
    0x00, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C,
    0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C,
    0xCF, 45,
    0x00, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xD0, 29,
    0x00, 0x1F, 0x1F, 0x11, 0x1E, 0x1F, 0x0F, 0x0F, 0x0D, 0x0D, 0x0B, 0x0B,
    0x09, 0x09, 0x07, 0x07, 0x05, 0x05, 0x01, 0x1F, 0x1F, 0x1F, 0x1F, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0xD1, 29,
    0x00, 0x1F, 0x1F, 0x10, 0x1E, 0x1F, 0x0E, 0x0E, 0x0C, 0x0C, 0x0A, 0x0A,
    0x08, 0x08, 0x06, 0x06, 0x04, 0x04, 0x00, 0x1F, 0x1F, 0x1F, 0x1F, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0xD2, 29,
    0x00, 0x5F, 0x1F, 0x10, 0x1F, 0x1E, 0x08, 0x08, 0x4A, 0x0A, 0x0C, 0x0C,
    0x0E, 0x0E, 0x04, 0x04, 0x06, 0x06, 0x00, 0x1F, 0x1F, 0x1F, 0x1F, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0xD3, 29,
    0x00, 0x1F, 0x1F, 0x11, 0x1F, 0x1E, 0x09, 0x09, 0x0B, 0x0B, 0x0D, 0x0D,
    0x0F, 0x0F, 0x05, 0x05, 0x07, 0x07, 0x01, 0x1F, 0x1F, 0x1F, 0x1F, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0xD4, 87,
    0x00, 0x20, 0x0B, 0x00, 0x0D, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x03, 0x03, 0x00, 0x81, 0x04, 0xAE,
    0x04, 0xB0, 0x04, 0xB2, 0x04, 0xB4, 0x04, 0xB6, 0x04, 0xB8, 0x00, 0x00,
    0x00, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x00, 0x06, 0x44, 0x06, 0x46,
    0x03, 0x03, 0x00, 0x00, 0x07, 0x00, 0x06, 0x04, 0xA7, 0x04, 0xA8, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x20, 0x00,
    0xD5, 61,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE0, 0x00, 0x00,
    0x00, 0x07, 0x32, 0x5A, 0x00, 0x00, 0x3C, 0x00, 0x1E, 0x00, 0x1E, 0xB3,
    0x00, 0x0F, 0x06, 0x0C, 0x00, 0x71, 0x20, 0x04, 0x10, 0x04, 0x06, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x1F,
    0xFF, 0x00, 0x00, 0x00, 0x1F, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF,
    0xCD, 2, 0x00, 0x00,
    0xDE, 1, 0x01,
    0xB9, 4, 0x00, 0xFF, 0xFF, 0x04,
    0xC7, 3, 0x1F, 0x14, 0x0E,
    0xDE, 1, 0x02,
    0xE5, 24,
    0x00, 0x60, 0x60, 0x02, 0x18, 0x60, 0x18, 0x60, 0x09, 0x04, 0x00, 0xC5,
    0x01, 0x2C, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x04,
    0xE6, 3, 0x10, 0x10, 0x82,
    0xC4, 7, 0x00, 0x11, 0x07, 0x00, 0x11, 0x01, 0x08,
    0xC3, 2, 0x20, 0xFF,
    0xBD, 1, 0x1B,
    0xC6, 2, 0x4A, 0x00,
    0xCD, 4, 0x14, 0x64, 0x11, 0x40,
    0xC1, 10, 0x00, 0x40, 0x00, 0x02, 0x02, 0x02, 0x02, 0x7F, 0x00, 0x00,
    0xB3, 2, 0x00, 0xA8,
    0xBB, 11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x40, 0x43, 0x04,
    0xC2, 10, 0x02, 0x42, 0x50, 0x00, 0x02, 0xE4, 0x61, 0x73, 0xF9, 0x08,
    0xEC, 14,
    0x07, 0x07, 0x40, 0x00, 0x22, 0x02, 0x00, 0xFF, 0x08, 0x7C, 0x00, 0x00,
    0x00, 0x00,
    0xDE, 1, 0x03,
    0xD1, 5, 0x00, 0x00, 0x21, 0xFF, 0x00,
    0xDE, 1, 0x00,
};

static esp_err_t hi8561_init(uint8_t id[2])
{
    if (!ls_panel_read_dcs(0xDA, &id[0]) || !ls_panel_read_dcs(0xDB, &id[1]))
        return ESP_ERR_TIMEOUT;
    if (id[0] != 0x85 || id[1] != 0x61) return ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < sizeof(HI8561_INIT); ) {
        const uint8_t cmd = HI8561_INIT[i], n = HI8561_INIT[i + 1];
        if (i + 2 + n > sizeof(HI8561_INIT)) return ESP_ERR_INVALID_SIZE;
        esp_err_t err = esp_lcd_panel_io_tx_param(s_io, cmd, &HI8561_INIT[i + 2], n);
        if (err != ESP_OK) return err;
        i += 2 + n;
    }
    esp_err_t err = esp_lcd_panel_io_tx_param(s_io, 0x35, NULL, 0);   /* TE on */
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(30));
    err = esp_lcd_panel_io_tx_param(s_io, 0x11, NULL, 0);             /* sleep out */
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(120));
    return esp_lcd_panel_io_tx_param(s_io, 0x29, NULL, 0);            /* display on */
}

/* PT4103 EN takes PWM directly; its soft start caps that at 1 kHz. Timer and
   channel 2: ls_keypad owns 1 for the keyboard backlight. */
#define BL_TIMER   LEDC_TIMER_2
#define BL_CHANNEL LEDC_CHANNEL_2
#define BL_BITS    LEDC_TIMER_10_BIT
static bool s_bl_ready;

static esp_err_t backlight_init(void)
{
    if (s_bl_ready) return ESP_OK;
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = BL_BITS,
        .timer_num = BL_TIMER, .freq_hz = LS_BOARD_LCD_BL_PWM_MAX_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) return err;
    const ledc_channel_config_t channel = {
        .gpio_num = LS_BOARD_LCD_BL_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_CHANNEL, .timer_sel = BL_TIMER, .duty = 0,
    };
    err = ledc_channel_config(&channel);
    if (err == ESP_OK) s_bl_ready = true;
    return err;
}

static esp_err_t backlight_set(unsigned percent)
{
    if (!s_bl_ready) return ESP_ERR_INVALID_STATE;
    const uint32_t max = (1u << BL_BITS) - 1;
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL,
                                  (max * percent + 50) / 100);
    return err == ESP_OK ? ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL) : err;
}
#endif

esp_err_t ls_panel_test_start(void)
{
    if (s_panel) return ESP_OK;
    esp_err_t err;
#ifdef LS_BOARD_LCD_DPI_PERF_CLK_MHZ
    s_pixel_clock_mhz = cell_performance_active() ? LS_BOARD_LCD_DPI_PERF_CLK_MHZ : LS_BOARD_LCD_DPI_CLK_MHZ;
    s_frame_us = PANEL_FRAME_PIXELS / s_pixel_clock_mhz;
#endif
#define TRY(call) do { err = (call); if (err != ESP_OK) goto fail; } while (0)
    esp_ldo_channel_config_t ldo = { .chan_id = 3, .voltage_mv = 2500 };
    TRY(esp_ldo_acquire_channel(&ldo, &s_ldo3));
    ldo.chan_id = 4; ldo.voltage_mv = 3300;
    TRY(esp_ldo_acquire_channel(&ldo, &s_ldo4));
    /* Repeat after the display supplies settle, as in vendor InitPower ->
       InitScreen/InitGt9895. The console can repeat this with i2c reset-touch. */
    TRY(ls_board_hw_touch_reset());
    TRY(ls_xl9535_out(LS_BOARD_XL_SCREEN_RST, false));
    vTaskDelay(pdMS_TO_TICKS(10));
    TRY(ls_xl9535_set(LS_BOARD_XL_SCREEN_RST, true));
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_lcd_dsi_bus_config_t bus = {
        .bus_id = 0, .num_data_lanes = LS_BOARD_LCD_DSI_LANES,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = LS_BOARD_LCD_DSI_MBPS,
    };
    TRY(esp_lcd_new_dsi_bus(&bus, &s_bus));
    esp_lcd_dbi_io_config_t io = { .lcd_cmd_bits = 8, .lcd_param_bits = 8 };
    TRY(esp_lcd_new_panel_io_dbi(s_bus, &io, &s_io));
#if defined(LS_BOARD_PANEL_HI8561)
    uint8_t ids[2] = {0};
    TRY(hi8561_init(ids));
    /* Dark until the TUI's first present lights it. */
    TRY(backlight_init());
    const unsigned id = (unsigned)ids[0] << 8 | ids[1];
#else
    uint8_t id = 0;
    if (!ls_panel_read_id(&id)) { err = ESP_ERR_TIMEOUT; goto fail; }
    if (id != 0x01) { err = ESP_ERR_NOT_FOUND; goto fail; }

    const uint8_t unlock[][2] = { {0xFE, 0xFD}, {0x80, 0xFC}, {0xFE, 0x00} };
    for (unsigned i = 0; i < sizeof(unlock) / sizeof(unlock[0]); ++i)
        TRY(esp_lcd_panel_io_tx_param(s_io, unlock[i][0], &unlock[i][1], 1));
    uint16_t xmax = LS_BOARD_LCD_H_RES - 1, ymax = LS_BOARD_LCD_V_RES - 1;
    uint8_t col[] = {0, 0, xmax >> 8, xmax & 255};
    uint8_t row[] = {0, 0, ymax >> 8, ymax & 255};
    uint8_t partial[] = {0, 3, (xmax - 3) >> 8, (xmax - 3) & 255};
    TRY(esp_lcd_panel_io_tx_param(s_io, 0x2A, col, sizeof(col)));
    TRY(esp_lcd_panel_io_tx_param(s_io, 0x2B, row, sizeof(row)));
    TRY(esp_lcd_panel_io_tx_param(s_io, 0x31, partial, sizeof(partial)));
    TRY(esp_lcd_panel_io_tx_param(s_io, 0x30, row, sizeof(row)));
    uint8_t zero = 0;
    TRY(esp_lcd_panel_io_tx_param(s_io, 0x12, &zero, 1));
    TRY(esp_lcd_panel_io_tx_param(s_io, 0x35, &zero, 1));
    TRY(esp_lcd_panel_io_tx_param(s_io, 0x51, &zero, 1));
    TRY(esp_lcd_panel_io_tx_param(s_io, 0x11, NULL, 0));
    vTaskDelay(pdMS_TO_TICKS(120));
    TRY(esp_lcd_panel_io_tx_param(s_io, 0x29, NULL, 0));
#endif

    esp_lcd_dpi_panel_config_t dpi = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = s_pixel_clock_mhz,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 2,
        .video_timing = {
            .h_size = LS_BOARD_LCD_H_RES, .v_size = LS_BOARD_LCD_V_RES,
            .hsync_pulse_width = LS_BOARD_LCD_HSYNC,
            .hsync_back_porch = LS_BOARD_LCD_HBP,
            .hsync_front_porch = LS_BOARD_LCD_HFP,
            .vsync_pulse_width = LS_BOARD_LCD_VSYNC,
            .vsync_back_porch = LS_BOARD_LCD_VBP,
            .vsync_front_porch = LS_BOARD_LCD_VFP,
        },
    };
    /* IDF owns both PSRAM framebuffers; only a completed frame is scanned. */
    TRY(esp_lcd_new_panel_dpi(s_bus, &dpi, &s_panel));
    /* Scans the zeroed framebuffer from here on, so the glass is black until
       the first frame. No DSI test pattern: enabling one stops the bridge
       under a DMA transfer already in flight, and switching it off again
       could leave the scan stopped after one frame. */
    TRY(esp_lcd_panel_init(s_panel));
    ESP_LOGI(PANEL_TAG, "ID 0x%02x; %dx%d scanning", (unsigned)id,
             LS_BOARD_LCD_H_RES, LS_BOARD_LCD_V_RES);
    return ESP_OK;
fail:
    if (s_panel) { esp_lcd_panel_del(s_panel); s_panel = NULL; }
    if (s_io) { esp_lcd_panel_io_del(s_io); s_io = NULL; }
    if (s_bus) { esp_lcd_del_dsi_bus(s_bus); s_bus = NULL; }
    if (s_ldo4) { esp_ldo_release_channel(s_ldo4); s_ldo4 = NULL; }
    if (s_ldo3) { esp_ldo_release_channel(s_ldo3); s_ldo3 = NULL; }
    ESP_LOGE(PANEL_TAG, "panel bring-up failed: %s", esp_err_to_name(err));
    return err;
#undef TRY
}

esp_err_t ls_panel_start(void)
{
    if (s_frames[s_back]) return ESP_OK;
    esp_err_t err = ls_panel_test_start();
    if (err != ESP_OK) return err;
    err = esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2,
            (void **)&s_frames[0], (void **)&s_frames[1]);
    if (err != ESP_OK) {
        s_frames[0] = s_frames[1] = NULL;
        ESP_LOGE(PANEL_TAG, "no framebuffers: %s", esp_err_to_name(err));
        return err;
    }
    const esp_lcd_dpi_panel_event_callbacks_t callbacks = {
        .on_refresh_done = refresh_done,
    };
    err = esp_lcd_dpi_panel_register_event_callbacks(s_panel, &callbacks, NULL);
    if (err != ESP_OK)
        ESP_LOGW(PANEL_TAG, "refresh count unavailable: %s", esp_err_to_name(err));
    return ESP_OK;
}

/* The last level written to the glass, and whether it has been put out for
   good (a restart is coming). */
static unsigned s_level;
static bool s_blanked;

esp_err_t ls_panel_set_brightness(unsigned percent)
{
    if (!s_io) return ESP_ERR_INVALID_STATE;
    if (s_blanked) return ESP_OK;
    /* 0 is dark, not the dimmest lit level: the boot keeps the glass dark
       until the first frame is up. */
    if (percent && percent < 5) percent = 5;
    if (percent > 100) percent = 100;
    s_level = percent;
#if defined(LS_BOARD_PANEL_HI8561)
    return backlight_set(percent);
#else
    uint8_t value = (percent * 255 + 50) / 100;
    return esp_lcd_panel_io_tx_param(s_io, 0x51, &value, 1);
#endif
}

/* Fade to black and switch the panel off, for good. The glass is dark and
   stays dark through the reset that follows, instead of showing whatever
   the panel shows when the video stops. Nothing lights it again until the
   panel is brought up afresh. */
esp_err_t ls_panel_blank(unsigned fade_ms)
{
    if (!s_io) return ESP_ERR_INVALID_STATE;
    if (s_blanked) return ESP_OK;
    const unsigned steps = fade_ms >= 40 ? 8 : 1;
    const unsigned from = s_level ? s_level : 5;
    for (unsigned i = 1; i < steps; i++) {
        ls_panel_set_brightness(from - from * i / steps);
        vTaskDelay(pdMS_TO_TICKS(fade_ms / steps));
    }
#if defined(LS_BOARD_PANEL_HI8561)
    esp_err_t e = backlight_set(0);
#else
    uint8_t zero = 0;
    esp_err_t e = esp_lcd_panel_io_tx_param(s_io, 0x51, &zero, 1);
#endif
    s_blanked = true;
    s_level = 0;
    if (e == ESP_OK) e = esp_lcd_panel_io_tx_param(s_io, 0x28, NULL, 0);   /* display off */
    return e;
}
#else
esp_err_t ls_panel_test_start(void) { return ESP_OK; }
esp_err_t ls_panel_start(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ls_panel_set_brightness(unsigned percent) { (void)percent; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ls_panel_blank(unsigned fade_ms) { (void)fade_ms; return ESP_ERR_NOT_SUPPORTED; }
void ls_panel_diagnostics(void) {}
bool ls_panel_fb(ls_panel_fb_t *out) { (void)out; return false; }
void ls_panel_fb_present(void) {}
void ls_panel_fb_present_rows(int y0, int y1) { (void)y0; (void)y1; }
#endif
