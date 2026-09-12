/* SPDX-License-Identifier: GPL-3.0-or-later
 * RM69A10 command sequence adapted from LILYGO_L's GPL-3.0 rm69a10.h in
 * cpp_bus_driver (T-Display-P4 vendor reference). No vendor framework needed.
 * Native panel bring-up and the TUI's framebuffer. Color bars remain the
 * diagnostic fallback until the TUI takes the framebuffer.
 */
#include "ls_panel.h"
#include "ls_board.h"

#if defined(LS_BOARD_PANEL_RM69A10)
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
static char s_gap_task[configMAX_TASK_NAME_LEN];

/* the Flipper can start P25 while HOME stays visible. Count scan
 * gaps here, where every TUI screen is covered, instead of in the retired
 * LVGL shell. A late callback is evidence of a gap, not proof of its cause. */
#define PANEL_FRAME_US ((uint32_t)((uint64_t)(LS_BOARD_LCD_H_RES + \
    LS_BOARD_LCD_HSYNC + LS_BOARD_LCD_HBP + LS_BOARD_LCD_HFP) * \
    (LS_BOARD_LCD_V_RES + LS_BOARD_LCD_VSYNC + LS_BOARD_LCD_VBP + \
    LS_BOARD_LCD_VFP) / LS_BOARD_LCD_DPI_CLK_MHZ))

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
            const char *name = pcTaskGetName(NULL);
            unsigned i = 0;
            for (; i + 1 < sizeof(s_gap_task) && name[i]; ++i)
                s_gap_task[i] = name[i];
            s_gap_task[i] = 0;
            __atomic_store_n(&s_max_gap_us, gap, __ATOMIC_RELAXED);
        }
        if (gap > PANEL_FRAME_US * 3 / 2)
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
    if (!s_panel || !s_frames[s_back]) return;
    /* The pixels are already in the buffer being scanned; this writes the
       cache back so the scan sees them. */
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0,
                              LS_BOARD_LCD_H_RES, LS_BOARD_LCD_V_RES,
                              s_frames[s_back]);
    static uint32_t reported, last_report_ms;
    uint32_t late = __atomic_load_n(&s_late_frames, __ATOMIC_RELAXED);
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (late != reported && now_ms - last_report_ms >= 1000) {
        ESP_LOGW("rm69a10", "refresh gaps: total=%lu max=%lu us expected=%lu us task=%s",
                 (unsigned long)late,
                 (unsigned long)__atomic_load_n(&s_max_gap_us, __ATOMIC_RELAXED),
                 (unsigned long)PANEL_FRAME_US, s_gap_task);
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
    printf("panel: late=%lu max_gap=%lu us expected=%lu us task=%s\n",
           (unsigned long)__atomic_load_n(&s_late_frames, __ATOMIC_RELAXED),
           (unsigned long)__atomic_load_n(&s_max_gap_us, __ATOMIC_RELAXED),
           (unsigned long)PANEL_FRAME_US, s_gap_task);
}

esp_err_t ls_panel_test_start(void)
{
    if (s_panel) return ESP_OK;
    esp_err_t err;
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
    uint8_t id = 0;
    TRY(esp_lcd_panel_io_rx_param(s_io, 0xA1, &id, 1));
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

    esp_lcd_dpi_panel_config_t dpi = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = LS_BOARD_LCD_DPI_CLK_MHZ,
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
    TRY(esp_lcd_panel_init(s_panel));
    TRY(esp_lcd_dpi_panel_set_pattern(s_panel, MIPI_DSI_PATTERN_BAR_VERTICAL));
    uint8_t brightness = 96;
    TRY(esp_lcd_panel_io_tx_param(s_io, 0x51, &brightness, 1));
    ESP_LOGI("rm69a10", "ID 0x%02x; %dx%d DSI color bars enabled", id,
             LS_BOARD_LCD_H_RES, LS_BOARD_LCD_V_RES);
    return ESP_OK;
fail:
    if (s_panel) { esp_lcd_panel_del(s_panel); s_panel = NULL; }
    if (s_io) { esp_lcd_panel_io_del(s_io); s_io = NULL; }
    if (s_bus) { esp_lcd_del_dsi_bus(s_bus); s_bus = NULL; }
    if (s_ldo4) { esp_ldo_release_channel(s_ldo4); s_ldo4 = NULL; }
    if (s_ldo3) { esp_ldo_release_channel(s_ldo3); s_ldo3 = NULL; }
    ESP_LOGE("rm69a10", "panel bring-up failed: %s", esp_err_to_name(err));
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
        ESP_LOGE("rm69a10", "no framebuffers: %s", esp_err_to_name(err));
        return err;
    }
    const esp_lcd_dpi_panel_event_callbacks_t callbacks = {
        .on_refresh_done = refresh_done,
    };
    err = esp_lcd_dpi_panel_register_event_callbacks(s_panel, &callbacks, NULL);
    if (err != ESP_OK)
        ESP_LOGW("rm69a10", "refresh count unavailable: %s", esp_err_to_name(err));
    return esp_lcd_dpi_panel_set_pattern(s_panel, MIPI_DSI_PATTERN_NONE);
}

esp_err_t ls_panel_set_brightness(unsigned percent)
{
    if (!s_io) return ESP_ERR_INVALID_STATE;
    if (percent < 5) percent = 5;
    if (percent > 100) percent = 100;
    uint8_t value = (percent * 255 + 50) / 100;
    return esp_lcd_panel_io_tx_param(s_io, 0x51, &value, 1);
}
#else
esp_err_t ls_panel_test_start(void) { return ESP_OK; }
esp_err_t ls_panel_start(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ls_panel_set_brightness(unsigned percent) { (void)percent; return ESP_ERR_NOT_SUPPORTED; }
void ls_panel_diagnostics(void) {}
bool ls_panel_fb(ls_panel_fb_t *out) { (void)out; return false; }
void ls_panel_fb_present(void) {}
#endif
