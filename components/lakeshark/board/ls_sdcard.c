/* See ls_sdcard.h for why the vendored BSP cannot mount this board's
   card. This is the board-native mount, shaped like ls_i2c.c. */
#include "ls_sdcard.h"

#include <string.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "soc/soc_caps.h"

#include "ls_board.h"

static const char *TAG = "ls_sdcard";

#if defined(LS_BOARD_SDIO_CLK_GPIO)

static sdmmc_card_t *s_card;
static char          s_name[24];

#if SOC_SDMMC_PSRAM_DMA_CAPABLE && CONFIG_SPIRAM
static esp_err_t sdcard_dma_info(int slot, esp_dma_mem_info_t *info)
{
    esp_err_t err = sdmmc_host_get_dma_info(slot, info);
    if (err == ESP_OK) {
        /* Keep SD transfer buffers out of the internal USB/BLE DMA heap. */
        info->extra_heap_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    }
    return err;
}
#endif

esp_err_t ls_sdcard_mount(void)
{
    if (s_card) return ESP_OK;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    /* The other slot on this board is the ESP32-C6 link, and the
       shared default points at it - which is the whole reason this file
       exists. */
    /* SLOT 0, WHICH IS THE ONE THIS CARD IS PHYSICALLY WIRED TO. */

    host.slot = SDMMC_HOST_SLOT_0;
#if SOC_SDMMC_PSRAM_DMA_CAPABLE && CONFIG_SPIRAM
    host.get_dma_info = sdcard_dma_info;
#endif

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = LS_BOARD_SDIO_CLK_GPIO;
    slot.cmd = LS_BOARD_SDIO_CMD_GPIO;
    slot.d0  = LS_BOARD_SDIO_D0_GPIO;
#if defined(LS_BOARD_SDIO_D1_GPIO)
    slot.d1  = LS_BOARD_SDIO_D1_GPIO;
    slot.d2  = LS_BOARD_SDIO_D2_GPIO;
    slot.d3  = LS_BOARD_SDIO_D3_GPIO;
    slot.width = 4;
#else
    slot.width = 1;
#endif
    /* The board has the pull-ups; asking the pad for its own on a bus this
       fast fights them. */
    slot.flags = 0;

    const esp_vfs_fat_mount_config_t mount = {
        /* NEVER true, and this is the line to read twice. */

        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    const esp_err_t err = esp_vfs_fat_sdmmc_mount(LS_SDCARD_MOUNT, &host,
                                                  &slot, &mount, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        /* Told apart, because they need different things done about them. */
        if (err == ESP_FAIL)
            ESP_LOGE(TAG, "card found but no readable filesystem on it - "
                          "NOT formatting; mount it elsewhere and look");
        else
            ESP_LOGE(TAG, "no card on SDIO1 (clk %d cmd %d d0 %d): %s",
                     LS_BOARD_SDIO_CLK_GPIO, LS_BOARD_SDIO_CMD_GPIO,
                     LS_BOARD_SDIO_D0_GPIO, esp_err_to_name(err));
        return err;
    }

    snprintf(s_name, sizeof(s_name), "%s", s_card->cid.name);
    ESP_LOGI(TAG, "mounted %s at %s, %llu MB, %d bit",
             s_name, LS_SDCARD_MOUNT,
             ((uint64_t)s_card->csd.capacity) * s_card->csd.sector_size
                 / (1024 * 1024),
             slot.width);
    return ESP_OK;
}

void ls_sdcard_unmount(void)
{
    if (!s_card) return;
    esp_vfs_fat_sdcard_unmount(LS_SDCARD_MOUNT, s_card);
    s_card = NULL;
    s_name[0] = 0;
}

bool ls_sdcard_mounted(void) { return s_card != NULL; }

const char *ls_sdcard_name(void) { return s_card ? s_name : NULL; }

bool ls_sdcard_size(uint64_t *total, uint64_t *free_bytes)
{
    if (total) *total = 0;
    if (free_bytes) *free_bytes = 0;
    if (!s_card) return false;

    uint64_t t = 0, f = 0;
    if (esp_vfs_fat_info(LS_SDCARD_MOUNT, &t, &f) != ESP_OK) return false;
    if (total) *total = t;
    if (free_bytes) *free_bytes = f;
    return true;
}

void ls_sdcard_diagnostics(void)
{
    if (!s_card) {
        const esp_err_t err = ls_sdcard_mount();
        if (err != ESP_OK) {
            printf("sd: not mounted (%s)\n", esp_err_to_name(err));
            printf("sd: SDIO1 clk %d cmd %d d0-d3 %d %d %d %d\n",
                   LS_BOARD_SDIO_CLK_GPIO, LS_BOARD_SDIO_CMD_GPIO,
                   LS_BOARD_SDIO_D0_GPIO, LS_BOARD_SDIO_D1_GPIO,
                   LS_BOARD_SDIO_D2_GPIO, LS_BOARD_SDIO_D3_GPIO);
            return;
        }
    }

    uint64_t total = 0, freeb = 0;
    ls_sdcard_size(&total, &freeb);
    printf("sd: %s at %s\n", s_name, LS_SDCARD_MOUNT);
    printf("sd: %llu MB free of %llu MB\n",
           freeb / (1024 * 1024), total / (1024 * 1024));
    sdmmc_card_print_info(stdout, s_card);
}

#else  /* the board declares no SDIO pins */

esp_err_t ls_sdcard_mount(void) { return ESP_ERR_NOT_SUPPORTED; }
void ls_sdcard_unmount(void) { }
bool ls_sdcard_mounted(void) { return false; }
const char *ls_sdcard_name(void) { return NULL; }
bool ls_sdcard_size(uint64_t *total, uint64_t *free_bytes)
{
    if (total) *total = 0;
    if (free_bytes) *free_bytes = 0;
    return false;
}
void ls_sdcard_diagnostics(void)
{
    printf("sd: this board declares no SDIO pins\n");
}

#endif
