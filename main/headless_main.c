/* headless_main.c - UART-console entry for the headless LakeShark build.
 *
 * Selected at build time by CONFIG_LAKESHARK_HEADLESS (see main/CMakeLists.txt).
 * Boots the same radio core as the GUI build (P25 / FM / ADS-B) but with no
 * display, Brookesia, or LVGL - just the backend and a serial console. The
 * rich TUI front end (ported from the tui-legacy branch) layers on top of this.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "lakeshark_backend.h"

static const char *TAG = "headless";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(bsp_spiffs_mount());
    ESP_ERROR_CHECK(bsp_extra_codec_init());     /* radio audio output */

    ESP_LOGI(TAG, "LakeShark headless boot - radio core, no display");

    /* Brings up USB host + RTL-SDR and registers the P25 / FM / ADS-B apps. */
    lakeshark_backend_start();

    /* Default to P25 and start the radio (GUI boots parked; headless runs). */
    lakeshark_select_p25();
    lakeshark_radio_unpark();

    /* Minimal console heartbeat. The full TUI (tui-legacy ui/tui.c) replaces
     * this loop in the next step. */
    while (1) {
        ESP_LOGI(TAG, "alive: P25 %.4f MHz  free_int=%u free_psram=%u",
                 lakeshark_p25_get_freq() / 1e6,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
