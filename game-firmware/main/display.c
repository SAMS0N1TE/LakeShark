#include "display.h"

#ifndef LAKESHARK_PRESENT_CORE
#define LAKESHARK_PRESENT_CORE 0
#endif
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/ppa.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7701.h"
#include "esp_ldo_regulator.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/axi_icm_ll.h"
#include "hal/dw_gdma_ll.h"
#include "sdkconfig.h"
#include <stdatomic.h>
#include "waveshare43_panel.inc"

enum { LCD_WIDTH = 480, LCD_HEIGHT = 800, LCD_BL = GPIO_NUM_26, LCD_RST = GPIO_NUM_27 };
static const char *TAG = "game_display";
static esp_lcd_panel_handle_t panel;
static uint16_t *draw_pixels[2];
static void *scanout[2];
static SemaphoreHandle_t refresh;
static SemaphoreHandle_t pending, presenter_idle;
static ppa_client_handle_t scaler;
static unsigned front, draw_slot, queued_slot;
static bool awaiting_refresh;
static bool display_ready;
static esp_err_t presenter_result;
static atomic_uint scale_us;

static void presenter_task(void *unused);

static bool IRAM_ATTR on_refresh(esp_lcd_panel_handle_t lcd,
                                 esp_lcd_dpi_panel_event_data_t *event, void *context)
{
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(context, &wake);
    return wake == pdTRUE;
}

static esp_err_t panel_init(void)
{
    const ledc_timer_config_t backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_1,
        .duty_resolution = LEDC_TIMER_10_BIT, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t backlight_channel = {
        .gpio_num = LCD_BL, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_1, .flags.output_invert = true,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&backlight_timer), TAG, "backlight timer");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&backlight_channel), TAG, "active-low backlight");
    // A warm launch can arrive with the pad held off. Release it after PWM is set to zero.
    ESP_RETURN_ON_ERROR(gpio_hold_dis(LCD_BL), TAG, "release backlight hold");
    const esp_ldo_channel_config_t power_cfg = { .chan_id = 3, .voltage_mv = 2500 };
    esp_ldo_channel_handle_t power;
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&power_cfg, &power), TAG, "DSI power");
    const esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0, .num_data_lanes = 2, .lane_bit_rate_mbps = 500,
    };
    esp_lcd_dsi_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &bus), TAG, "DSI bus");
    const esp_lcd_dbi_io_config_t io_cfg = { .lcd_cmd_bits = 8, .lcd_param_bits = 8 };
    esp_lcd_panel_io_handle_t io;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(bus, &io_cfg, &io), TAG, "DSI control IO");

    const esp_lcd_dpi_panel_config_t dpi_cfg = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = CONFIG_LS_GAME_DPI_CLOCK_MHZ,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565, .num_fbs = 2,
        .video_timing = {
            .h_size = LCD_WIDTH, .v_size = LCD_HEIGHT,
            .hsync_back_porch = 42, .hsync_pulse_width = 12, .hsync_front_porch = 42,
            .vsync_back_porch = 2, .vsync_pulse_width = 8, .vsync_front_porch = 60,
        },
    };
    st7701_vendor_config_t vendor = {
        .init_cmds = panel_commands, .init_cmds_size = sizeof(panel_commands) / sizeof(panel_commands[0]),
        .flags.use_mipi_interface = 1,
        .mipi_config = { .dsi_bus = bus, .dpi_config = &dpi_cfg },
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16, .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7701(io, &panel_cfg, &panel), TAG, "ST7701");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "panel reset");
    return esp_lcd_panel_init(panel);
}

static esp_err_t presenter_init(void)
{
    // The DSI DMA reads the scanout buffers continuously through its memory port.
    // Give those reads priority over competing PSRAM traffic from PPA and map loads.
    axi_icm_ll_set_dw_gdma_qos_arbiter_prio(DW_GDMA_LL_MASTER_PORT_MEMORY, 0, 8);
    ESP_RETURN_ON_ERROR(panel_init(), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(panel, 2, &scanout[0], &scanout[1]),
                        TAG, "panel buffers");
    for (unsigned i = 0; i < 2; ++i) {
        draw_pixels[i] = heap_caps_aligned_calloc(CONFIG_CACHE_L2_CACHE_LINE_SIZE,
                                                GAME_WIDTH * GAME_HEIGHT,
                                                sizeof(*draw_pixels[i]),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!draw_pixels[i]) return ESP_ERR_NO_MEM;
    }
    const esp_lcd_dpi_panel_event_callbacks_t callbacks = { .on_refresh_done = on_refresh };
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_register_event_callbacks(panel, &callbacks, refresh),
                        TAG, "refresh callback");
    const ppa_client_config_t ppa_cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_64,
    };
    return ppa_register_client(&ppa_cfg, &scaler);
}

esp_err_t game_display_init(void)
{
    if (display_ready) return ESP_OK;
    refresh = xSemaphoreCreateBinary();
    pending = xSemaphoreCreateBinary();
    presenter_idle = xSemaphoreCreateBinary();
    if (!refresh || !pending || !presenter_idle) return ESP_ERR_NO_MEM;

    // On pre-v3 P4 the DMA EOF ISR chooses the next buffer before signaling refresh.
    // Initialize that interrupt on the presenter core so submission can't race its choice.
    if (xTaskCreatePinnedToCore(presenter_task, "present", 4608, NULL, 5, NULL,
                                LAKESHARK_PRESENT_CORE) != pdPASS)
        return ESP_ERR_NO_MEM;
    xSemaphoreTake(presenter_idle, portMAX_DELAY);
    ESP_RETURN_ON_ERROR(presenter_result, TAG, "presenter init");
    xSemaphoreGive(presenter_idle);
    display_ready = true;
    return ESP_OK;
}

esp_err_t game_display_brightness(unsigned percent)
{
    if (percent > 100) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 1023 * percent / 100),
                        TAG, "backlight duty");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0),
                        TAG, "apply backlight duty");
    return gpio_hold_dis(LCD_BL);
}

esp_err_t game_display_blank_for_restart(void)
{
    // The GPIO matrix inverts the channel output, so LEDC idle-low turns the backlight off.
    ESP_RETURN_ON_ERROR(ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0), TAG, "blank backlight");
    return gpio_hold_en(LCD_BL);
}

uint16_t *game_display_buffer(void)
{
    return draw_pixels[draw_slot];
}

static esp_err_t wait_for_scanout(void)
{
    if (!awaiting_refresh) return ESP_OK;
    if (xSemaphoreTake(refresh, pdMS_TO_TICKS(90)) != pdTRUE) return ESP_ERR_TIMEOUT;
    awaiting_refresh = false;
    return ESP_OK;
}

static esp_err_t present_slot(unsigned source)
{
    esp_err_t status = wait_for_scanout();
    if (status != ESP_OK) return status;
    unsigned back = front ^ 1;
    const ppa_srm_oper_config_t op = {
        .in = {
            .buffer = draw_pixels[source], .pic_w = GAME_WIDTH, .pic_h = GAME_HEIGHT,
            .block_w = GAME_WIDTH, .block_h = GAME_HEIGHT, .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = scanout[back], .buffer_size = LCD_WIDTH * LCD_HEIGHT * 2,
            .pic_w = LCD_WIDTH, .pic_h = LCD_HEIGHT, .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
        .scale_x = (float)LCD_HEIGHT / GAME_WIDTH,
        .scale_y = (float)LCD_WIDTH / GAME_HEIGHT, .mode = PPA_TRANS_MODE_BLOCKING,
    };
    int64_t start = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(ppa_do_scale_rotate_mirror(scaler, &op), TAG, "rotate and scale");
    atomic_store_explicit(&scale_us, esp_timer_get_time() - start, memory_order_relaxed);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_WIDTH, LCD_HEIGHT,
                                                scanout[back]), TAG, "queue scanout");
    front = back;
    /* Only release the old scanout after a refresh observed after submission. */
    xSemaphoreTake(refresh, 0);
    awaiting_refresh = true;
    return wait_for_scanout();
}

static void presenter_task(void *unused)
{
    const esp_err_t init = presenter_init();
    presenter_result = init;
    if (init == ESP_OK)
        ESP_LOGI(TAG, "LCD %d MHz, scanout and buffer submission on core %d",
                 CONFIG_LS_GAME_DPI_CLOCK_MHZ, xPortGetCoreID());
    xSemaphoreGive(presenter_idle);
    if (init != ESP_OK) {
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        xSemaphoreTake(pending, portMAX_DELAY);
        presenter_result = present_slot(queued_slot);
        xSemaphoreGive(presenter_idle);
    }
}

esp_err_t game_display_present(void)
{
    /* Only the previous submission can own the other source buffer.
       Once it has been displayed, hand the presenter the just-drawn buffer. */
    if (xSemaphoreTake(presenter_idle, pdMS_TO_TICKS(120)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t previous = presenter_result;
    queued_slot = draw_slot;
    draw_slot ^= 1;
    xSemaphoreGive(pending);
    return previous;
}

esp_err_t game_display_wait_visible(void)
{
    if (xSemaphoreTake(presenter_idle, pdMS_TO_TICKS(120)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t result = presenter_result;
    // The presenter has observed the flip. Wait for the new frame to finish scanning.
    if (result == ESP_OK && xSemaphoreTake(refresh, pdMS_TO_TICKS(90)) != pdTRUE)
        result = ESP_ERR_TIMEOUT;
    xSemaphoreGive(presenter_idle);
    return result;
}

unsigned game_display_scale_us(void)
{
    return atomic_load_explicit(&scale_us, memory_order_relaxed);
}

