#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_memory_utils.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"

#include "shell/ls_shell.hpp"
#include "p25_gui/AppP25.hpp"
#include "fm_gui/AppFM.hpp"
#include "adsb_gui/AppADSB.hpp"
#include "mesh/MeshController.hpp"
#include "file_browser/FileBrowser.hpp"

#include "lakeshark_backend.h"
#include "boot_splash.h"
#include "bsod.h"

extern "C" esp_lcd_panel_handle_t bsp_get_dsi_panel(void);

static const char *TAG = "main";

#define BOOT_BTN_GPIO  GPIO_NUM_35

static void boot_btn_poll_cb(lv_timer_t *)
{
    static int prev = 1, stable = 1, cnt = 0;
    int lvl = gpio_get_level(BOOT_BTN_GPIO);
    if (lvl == stable) { cnt = 0; }
    else if (++cnt >= 2) { stable = lvl; cnt = 0;
        if (prev == 1 && stable == 0) LsShell::instance().cycleNext();
        prev = stable;
    }
}

static void boot_btn_init(void)
{
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << BOOT_BTN_GPIO;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);
    lv_timer_create(boot_btn_poll_cb, 40, NULL);
}

extern "C" void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(bsp_spiffs_mount());
    ESP_LOGI(TAG, "SPIFFS mount successfully");

#if CONFIG_EXAMPLE_ENABLE_SD_CARD
    ESP_ERROR_CHECK(bsp_sdcard_mount());
    ESP_LOGI(TAG, "SD card mount successfully");
#endif

    ESP_ERROR_CHECK(bsp_extra_codec_init());

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = true,
        }};

    cfg.lvgl_port_cfg.task_affinity = 0;
    bsp_display_start_with_config(&cfg);

    bsod_init(bsp_get_dsi_panel());

    bsp_display_backlight_on();

    const char *recover_app = lakeshark_recovery_take_app();
    const bool  recovering  = (recover_app != NULL);

    if (!recovering) {
        bsp_display_lock(0);
        lakeshark_boot_splash_show();
        bsp_display_unlock();
        vTaskDelay(pdMS_TO_TICKS(2200));
    }

    bsp_display_lock(0);

    LsShell &shell = LsShell::instance();
    shell.begin();

    shell.registerApp(new AppP25());
    shell.registerApp(new AppFM());
    shell.registerApp(new AppADSB());
    shell.registerApp(new MeshController());
    shell.registerApp(new AppFileBrowser());

    bsp_display_unlock();

    lakeshark_backend_start();

    bsp_display_lock(0);
    if (!recovering) lakeshark_boot_splash_hide();
    shell.start(recover_app);
    boot_btn_init();
    bsp_display_unlock();
}
