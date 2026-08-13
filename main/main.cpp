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
#include "esp_system.h"
#include "settings.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"

#include "shell/ls_shell.hpp"
/*LS-602*/
#include "shell/ls_hub.h"
/*LS-606*/
#include "sdr_ui/sdr_ui.h"
/*LS-604*/
#include "home/AppHome.hpp"
#include "p25_gui/AppP25.hpp"
#include "fm_gui/AppFM.hpp"
#include "adsb_gui/AppADSB.hpp"
#include "file_browser/FileBrowser.hpp"
#include "settings/AppSettings.hpp"

#include "lakeshark_backend.h"
#include "ls_board.h"
/*LS-017*/
#include "esp_hosted.h"
#include "display_ctl.h"
#include "ls_ctl.h"
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

/*LS-017*/
static void c6_probe(void)
{
    int e = esp_hosted_connect_to_slave();
    if (e != 0) {
        ESP_LOGW(TAG, "C6 co-processor link FAILED (%d) - no BLE on this build", e);
        /*LS-603*/
        ls_hub_set_c6(0);
        return;
    }

    esp_hosted_coprocessor_fwver_t v = {};
    if (esp_hosted_get_coprocessor_fwversion(&v) != 0) {
        ESP_LOGW(TAG, "C6 link up but the slave will not report a version - "
                      "that is the pre-2.5.2 factory image, it must be reflashed");
        /*LS-603*/
        ls_hub_set_c6(0);
        return;
    }

    /*LS-603*/
    ls_hub_set_c6(1);

    ESP_LOGI(TAG, "C6 esp_hosted: host %d.%d.%d, co-processor %lu.%lu.%lu",
             ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1,
             ESP_HOSTED_VERSION_PATCH_1,
             (unsigned long)v.major1, (unsigned long)v.minor1,
             (unsigned long)v.patch1);

    if ((uint32_t)ESP_HOSTED_VERSION_MAJOR_1 != v.major1 ||
        (uint32_t)ESP_HOSTED_VERSION_MINOR_1 != v.minor1) {
        ESP_LOGE(TAG, "C6 MAJOR.MINOR MISMATCH - RPC will time out and BLE will "
                      "not start. Reflash the C6 from c6_firmware/ (LS-013).");
    }
}

/*LS-015*/
static void vbus_init(void)
{
#if LS_BOARD_HAS_VBUS_CTRL
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << LS_BOARD_VBUS_EN_GPIO;
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);
    gpio_set_level((gpio_num_t)LS_BOARD_VBUS_EN_GPIO, 1);
    ESP_LOGI(TAG, "USB host VBUS switch on GPIO%d - enabled",
             (int)LS_BOARD_VBUS_EN_GPIO);
#else
    ESP_LOGI(TAG, "no USB host VBUS switch on this board - port is hard-powered");
#endif
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
    ESP_LOGW(TAG, "boot: reset_reason=%d", (int)esp_reset_reason());

    /*LS-015*/
    vbus_init();
    /*LS-017*/
    c6_probe();

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
    /*LS-016*/
    {
        esp_err_t sd = bsp_sdcard_mount();
        /*LS-603*/
        ls_hub_set_sd(sd == ESP_OK);
        if (sd == ESP_OK) {
            ESP_LOGI(TAG, "SD card mounted at %s", BSP_SD_MOUNT_POINT);
        } else {
            ESP_LOGW(TAG, "no SD card (%s) - running without it",
                     esp_err_to_name(sd));
        }
    }
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

    /*LS-604*/
    shell.registerApp(new AppHome());
    shell.registerApp(new AppP25());
    shell.registerApp(new AppFM());
    shell.registerApp(new AppADSB());
    shell.registerApp(new LsSettings());
    shell.registerApp(new AppFileBrowser(), false);

    bsp_display_unlock();

    lakeshark_backend_start();
    display_ctl_init();
    ls_ctl_start_repl();

    if (!recovering) lakeshark_boot_sound();

    bsp_display_lock(0);
    /*LS-606*/
    sdr_theme_set((sdr_theme_t)settings_get_theme());
    if (!recovering) lakeshark_boot_splash_hide();
    shell.start(recover_app);
    boot_btn_init();
    bsp_display_unlock();
}
