#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_console.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "lakeshark_backend.h"
#include "audio_out.h"
#include "tone.h"

static const char *TAG = "headless";

#define BOOT_BTN_GPIO   GPIO_NUM_35
#define USB_VBUS_GPIO   GPIO_NUM_46
#define PA_CTRL_GPIO    GPIO_NUM_53
#define DEFAULT_VOLUME  85

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

static const char *s_fm_modes[] = { "listen", "scan", "pocsag", "wfm" };
#define FM_IDX 2

static volatile int s_mode = 0;

static void pa_on(void) { gpio_set_level(PA_CTRL_GPIO, 1); }

static uint32_t cur_freq_hz(void)
{
    switch (s_mode) {
    case 0:  return lakeshark_p25_get_freq();
    case 2:  return lakeshark_fm_get_freq();
    default: return 1090000000UL;
    }
}

static void select_mode(int idx)
{
    s_mode = idx;
    ESP_LOGI(TAG, ">>> mode: %s", s_modes[idx].name);
    s_modes[idx].select();
    pa_on();
}

static void cycle_next(void)
{
    select_mode((s_mode + 1) % N_MODES);
}

static void boot_btn_task(void *arg)
{
    (void)arg;
    int stable = 1, prev = 1, cnt = 0;
    for (;;) {
        int lvl = gpio_get_level(BOOT_BTN_GPIO);
        if (lvl == stable) {
            cnt = 0;
        } else if (++cnt >= 2) {
            stable = lvl;
            cnt = 0;
            if (prev == 1 && stable == 0) {
                cycle_next();
            }
            prev = stable;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

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
    gpio_set_level(PA_CTRL_GPIO, 1);

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BOOT_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);
}

static int cmd_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("mode=%s  freq=%.4f MHz  vol=%d  gain=%.1f dB  mute=%d  fmmode=%s  "
           "feed=%s  free_int=%u  free_psram=%u\n",
           s_modes[s_mode].name, cur_freq_hz() / 1e6,
           audio_volume_get(), lakeshark_radio_get_gain_tenths() / 10.0,
           audio_is_muted(), s_fm_modes[lakeshark_fm_get_mode() & 3],
           lakeshark_cartotui_enabled() ? "on" : "off",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return 0;
}

static int cmd_feed(int argc, char **argv)
{
    if (argc >= 2) {
        if      (!strcmp(argv[1], "on"))  lakeshark_cartotui_set_enabled(true);
        else if (!strcmp(argv[1], "off")) lakeshark_cartotui_set_enabled(false);
        else { printf("usage: feed on|off\n"); return 0; }
    }
    printf("feed=%s\n", lakeshark_cartotui_enabled() ? "on" : "off");
    return 0;
}

static int cmd_fm(int argc, char **argv)
{
    if (argc < 2) {
        printf("fm submode=%s (listen|scan|pocsag|wfm)\n",
               s_fm_modes[lakeshark_fm_get_mode() & 3]);
        return 0;
    }
    int m = -1;
    for (int i = 0; i < 4; i++) {
        if (!strcmp(argv[1], s_fm_modes[i])) { m = i; break; }
    }
    if (m < 0 && !strcmp(argv[1], "nbfm")) m = 0;
    if (m < 0) { printf("usage: fm listen|scan|pocsag|wfm\n"); return 0; }
    if (s_mode != FM_IDX) select_mode(FM_IDX);
    lakeshark_fm_set_mode(m);
    pa_on();
    printf("mode=FM submode=%s\n", s_fm_modes[m]);
    return 0;
}

static int cmd_mode(int argc, char **argv)
{
    if (argc < 2) { printf("usage: mode p25|adsb|fm|next\n"); return 0; }
    if      (!strcmp(argv[1], "next")) cycle_next();
    else if (!strcmp(argv[1], "p25"))  select_mode(0);
    else if (!strcmp(argv[1], "adsb")) select_mode(1);
    else if (!strcmp(argv[1], "fm"))   select_mode(2);
    else { printf("unknown mode '%s' (p25|adsb|fm|next)\n", argv[1]); return 0; }
    printf("mode=%s\n", s_modes[s_mode].name);
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
    if (argc < 2) { printf("freq=%.4f MHz\n", cur_freq_hz() / 1e6); return 0; }
    uint32_t hz = (uint32_t)(atof(argv[1]) * 1e6 + 0.5);
    if      (s_mode == 0) lakeshark_p25_set_freq(hz);
    else if (s_mode == 2) lakeshark_fm_set_freq(hz);
    else { printf("ADS-B is fixed at 1090 MHz\n"); return 0; }
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
        if      (s_mode == 0) lakeshark_p25_agc();
        else if (s_mode == 1) lakeshark_adsb_agc();
        else                  lakeshark_fm_agc();
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
        { .command = "status", .help = "Show mode, freq, volume, gain, mute, heap",
          .func = &cmd_status },
        { .command = "mode",   .help = "Switch mode", .hint = "p25|adsb|fm|next",
          .func = &cmd_mode },
        { .command = "fm",     .help = "FM sub-mode (hops into FM)",
          .hint = "listen|scan|pocsag|wfm", .func = &cmd_fm },
        { .command = "vol",    .help = "Volume 0-100 (or +n / -n)", .hint = "<n|+n|-n>",
          .func = &cmd_vol },
        { .command = "freq",   .help = "Tune the current mode", .hint = "<MHz>",
          .func = &cmd_freq },
        { .command = "gain",   .help = "RF gain in dB, or 'auto'", .hint = "<dB|auto>",
          .func = &cmd_gain },
        { .command = "feed",   .help = "ADS-B JSON feed to console (CartoTUI)",
          .hint = "on|off", .func = &cmd_feed },
        { .command = "mute",   .help = "Toggle audio mute", .func = &cmd_mute },
    };
    esp_console_register_help_command();
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
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

    ESP_LOGI(TAG, "LakeShark headless boot - radio core, no display (NANO)");
    ESP_LOGI(TAG, "BOOT button (GPIO%d) cycles P25 -> ADS-B -> FM", BOOT_BTN_GPIO);

    lakeshark_backend_start();

    select_mode(0);
    lakeshark_radio_unpark();
    audio_volume_set(DEFAULT_VOLUME);

    vTaskDelay(pdMS_TO_TICKS(700));
    pa_on();
    audio_out_ensure_unmuted();
    snd_boot();

    xTaskCreate(boot_btn_task, "boot_btn", 3072, NULL, 5, NULL);

    esp_log_level_set("P25TEL",  ESP_LOG_ERROR);
    esp_log_level_set("P25DIAG", ESP_LOG_ERROR);
    esp_log_level_set("ADSB",    ESP_LOG_ERROR);

    console_start();
    ESP_LOGI(TAG, "console ready - type 'help' for commands");
}
