#include "waveshare_p4_sd.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_pwr_ctrl.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include <string.h>

esp_err_t waveshare_p4_sd_mount(const char *base_path, int max_files)
{
    static sdmmc_card_t *card;
    static char mounted_path[ESP_VFS_PATH_MAX + 1];
    if (!base_path || base_path[0] != '/' || strlen(base_path) >= sizeof(mounted_path) || max_files < 1)
        return ESP_ERR_INVALID_ARG;
    if (card) return strcmp(mounted_path, base_path) == 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
    const sd_pwr_ctrl_ldo_config_t power_cfg = { .ldo_chan_id = 4 };
    sd_pwr_ctrl_handle_t power;
    esp_err_t result = sd_pwr_ctrl_new_on_chip_ldo(&power_cfg, &power);
    if (result != ESP_OK) return result;
    result = sd_pwr_ctrl_set_io_voltage(power, 3300);
    if (result != ESP_OK) {
        sd_pwr_ctrl_del_on_chip_ldo(power);
        return result;
    }
    // Let the switched SD supply settle before the host sends its first command.
    vTaskDelay(pdMS_TO_TICKS(200));

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    host.pwr_ctrl_handle = power;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    const esp_vfs_fat_sdmmc_mount_config_t fat = {
        .format_if_mount_failed = false,
        .max_files = max_files,
    };
    sdmmc_card_t *mounted;
    result = esp_vfs_fat_sdmmc_mount(base_path, &host, &slot, &fat, &mounted);
    if (result != ESP_OK) {
        ESP_LOGW("waveshare_p4_sd", "SD mount failed: %s", esp_err_to_name(result));
        sd_pwr_ctrl_del_on_chip_ldo(power);
    } else {
        card = mounted;
        strcpy(mounted_path, base_path);
        ESP_LOGI("waveshare_p4_sd", "microSD mounted at %s", mounted_path);
    }
    return result;
}

