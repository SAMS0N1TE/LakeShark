#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_console.h"
#include "ls_ctl.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "lakeshark_backend.h"
#include "audio_out.h"
#include "tone.h"

static const char *TAG = "headless";

#define USB_VBUS_GPIO   GPIO_NUM_46
#define PA_CTRL_GPIO    GPIO_NUM_53
#define DEFAULT_VOLUME  55

static void pa_enable(int on) { gpio_set_level(PA_CTRL_GPIO, on ? 1 : 0); }

static void gpio_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << USB_VBUS_GPIO) | (1ULL << PA_CTRL_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out);
    gpio_set_level(USB_VBUS_GPIO, 1);
    gpio_set_level(PA_CTRL_GPIO, 0);
}

static int cmd_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("P25  freq=%.4f MHz  vol=%d  gain=%.1f dB  mute=%d  "
           "free_int=%u  free_psram=%u\n",
           lakeshark_p25_get_freq() / 1e6,
           audio_volume_get(), lakeshark_radio_get_gain_tenths() / 10.0,
           audio_is_muted(),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return 0;
}

static int cmd_vol(int argc, char **argv)
{
    if (argc < 2) { printf("vol=%d\n", audio_volume_get()); return 0; }
    if (argv[1][0] == '+' || argv[1][0] == '-') audio_volume_delta(atoi(argv[1]));
    else                                         audio_volume_set(atoi(argv[1]));
    printf("vol=%d\n", audio_volume_get());
    return 0;
}

static int cmd_freq(int argc, char **argv)
{
    if (argc < 2) { printf("freq=%.4f MHz\n", lakeshark_p25_get_freq() / 1e6); return 0; }
    uint32_t hz = (uint32_t)(atof(argv[1]) * 1e6 + 0.5);
    lakeshark_p25_set_freq(hz);
    printf("freq=%.4f MHz\n", hz / 1e6);
    return 0;
}

static int cmd_gain(int argc, char **argv)
{
    if (argc < 2) {
        printf("gain=%.1f dB\n", lakeshark_radio_get_gain_tenths() / 10.0);
        return 0;
    }
    if (!strcmp(argv[1], "auto")) {
        lakeshark_p25_agc();
        printf("gain=auto\n");
        return 0;
    }
    lakeshark_radio_set_gain((int)(atof(argv[1]) * 10 + 0.5));
    printf("gain=%.1f dB\n", lakeshark_radio_get_gain_tenths() / 10.0);
    return 0;
}

static int cmd_mute(int argc, char **argv)
{
    (void)argc; (void)argv;
    audio_toggle_mute();
    printf("mute=%d\n", audio_is_muted());
    return 0;
}

static void console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "lakeshark>";
    repl_cfg.max_cmdline_length = 128;

    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl));

    const esp_console_cmd_t cmds[] = {
        { .command = "status", .help = "Show freq, volume, gain, mute, heap",
          .func = &cmd_status },
        { .command = "vol",    .help = "Volume 0-100 (or +n / -n)", .hint = "<n|+n|-n>",
          .func = &cmd_vol },
        { .command = "freq",   .help = "Tune P25 receiver", .hint = "<MHz>",
          .func = &cmd_freq },
        { .command = "gain",   .help = "RF gain in dB, or 'auto'", .hint = "<dB|auto>",
          .func = &cmd_gain },
        { .command = "mute",   .help = "Toggle audio mute", .func = &cmd_mute },
    };
    esp_console_register_help_command();
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
    ls_ctl_register_commands();
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    gpio_init();

    ESP_ERROR_CHECK(bsp_spiffs_mount());
    ESP_ERROR_CHECK(bsp_extra_codec_init_speaker_only());
    pa_enable(0);

    ESP_LOGI(TAG, "LakeShark headless P25 - ESP32-P4-WIFI6, no display");
    ESP_LOGI(TAG, "tuned to Franklin PD %.4f MHz", 154785000UL / 1e6);

    lakeshark_backend_start();
    audio_volume_set(DEFAULT_VOLUME);

    vTaskDelay(pdMS_TO_TICKS(150));
    pa_enable(1);
    vTaskDelay(pdMS_TO_TICKS(90));
    audio_out_ensure_unmuted();
    snd_boot();
    vTaskDelay(pdMS_TO_TICKS(500));

    lakeshark_select_p25();
    lakeshark_radio_unpark();

    esp_log_level_set("P25TEL",  ESP_LOG_ERROR);
    esp_log_level_set("P25DIAG", ESP_LOG_ERROR);

    console_start();
    ESP_LOGI(TAG, "console ready - type 'help' for commands");
}
