/* headless_main.c - UART-console entry for the headless LakeShark build.
 *
 * Selected at build time by CONFIG_LAKESHARK_HEADLESS (see main/CMakeLists.txt).
 * Boots the same radio core as the GUI build (P25 / FM / ADS-B) but with no
 * display, Brookesia, or LVGL - just the backend and a serial console.
 *
 * Target board: ESP32-P4-NANO. A single press of the BOOT button (GPIO35)
 * cycles the active mode P25 -> ADS-B -> FM -> P25. app_switch_to() (reached
 * via lakeshark_select_*) tears the previous mode down and brings the next one
 * up, keeping the radio live across the switch.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "lakeshark_backend.h"

static const char *TAG = "headless";

/* BOOT button on the P4 (active-low, internal pull-up). Single press = cycle. */
#define BOOT_BTN_GPIO   GPIO_NUM_35
/* USB host VBUS load-switch enable on the NANO board (active-high). The 86-box
 * BSP this firmware links against does not drive it, so power it here or the
 * RTL-SDR never enumerates on the nano. */
#define USB_VBUS_GPIO   GPIO_NUM_46

typedef struct {
    const char *name;
    void      (*select)(void);
} hl_mode_t;

static const hl_mode_t s_modes[] = {
    { "P25",   lakeshark_select_p25  },
    { "ADS-B", lakeshark_select_adsb },
    { "FM",    lakeshark_select_fm   },
};
#define N_MODES ((int)(sizeof(s_modes) / sizeof(s_modes[0])))

static volatile int s_mode = 0;

static void select_mode(int idx)
{
    s_mode = idx;
    ESP_LOGI(TAG, ">>> mode: %s", s_modes[idx].name);
    s_modes[idx].select();
}

static void cycle_next(void)
{
    select_mode((s_mode + 1) % N_MODES);
}

/* Debounced single-press detector. Fires cycle_next() on each clean press
 * (high->low edge that holds stable). */
static void boot_btn_task(void *arg)
{
    (void)arg;
    int stable = 1, prev = 1, cnt = 0;
    for (;;) {
        int lvl = gpio_get_level(BOOT_BTN_GPIO);
        if (lvl == stable) {
            cnt = 0;
        } else if (++cnt >= 2) {            /* ~40 ms held -> accept new level */
            stable = lvl;
            cnt = 0;
            if (prev == 1 && stable == 0) { /* press */
                cycle_next();
            }
            prev = stable;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void gpio_init(void)
{
    gpio_config_t vbus = {
        .pin_bit_mask = 1ULL << USB_VBUS_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&vbus);
    gpio_set_level(USB_VBUS_GPIO, 1);       /* power the USB host port */

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BOOT_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    gpio_init();                                  /* VBUS high before USB host */

    ESP_ERROR_CHECK(bsp_spiffs_mount());
    ESP_ERROR_CHECK(bsp_extra_codec_init_speaker_only());  /* nano: no ES7210 mic */

    ESP_LOGI(TAG, "LakeShark headless boot - radio core, no display (NANO)");
    ESP_LOGI(TAG, "BOOT button (GPIO%d) cycles P25 -> ADS-B -> FM", BOOT_BTN_GPIO);

    /* Brings up USB host + RTL-SDR and registers the P25 / FM / ADS-B apps. */
    lakeshark_backend_start();

    /* Default to P25 and run (the GUI build boots parked; headless runs). */
    select_mode(0);
    lakeshark_radio_unpark();

    xTaskCreate(boot_btn_task, "boot_btn", 3072, NULL, 5, NULL);

    while (1) {
        ESP_LOGI(TAG, "alive: %s  free_int=%u free_psram=%u",
                 s_modes[s_mode].name,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
