#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_console.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "ls_ctl.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"

#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "lakeshark_backend.h"
#include "audio_out.h"
#include "tone.h"
#include "flipper_link.h"
#include "esp_hosted.h"
#include "ble_link.h"
#include "settings.h"
#include "esp_libusb.h"

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

static const struct { const char *name; esp_log_level_t lvl; } LOG_LEVELS[] = {
    { "none",    ESP_LOG_NONE    },
    { "error",   ESP_LOG_ERROR   },
    { "warn",    ESP_LOG_WARN    },
    { "info",    ESP_LOG_INFO    },
    { "debug",   ESP_LOG_DEBUG   },
    { "verbose", ESP_LOG_VERBOSE },
};
#define N_LOG_LEVELS ((int)(sizeof(LOG_LEVELS) / sizeof(LOG_LEVELS[0])))

static const char *LOG_QUIET_TAGS[] = { "P25TEL", "P25DIAG", "ADSB", "NimBLE" };

#define NVS_NS   "lakeshark"
#define NVS_VOL  "vol"

static int settings_load_volume(void)
{
    nvs_handle_t h;
    int32_t v = DEFAULT_VOLUME;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, NVS_VOL, &v) != ESP_OK) v = DEFAULT_VOLUME;
        nvs_close(h);
    }
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    return (int)v;
}

static void settings_save_volume(int v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, NVS_VOL, (int32_t)v);
    nvs_commit(h);
    nvs_close(h);
}

#define SETTINGS_STACK_WORDS (3072 / sizeof(StackType_t))
static StackType_t  s_settings_stack[SETTINGS_STACK_WORDS];
static StaticTask_t s_settings_tcb;

#define SDR_STALL_RECOVER_S 20
#define SDR_STALL_MAX_TRIES 3

#define SDR_STALL_RETRY_S   20

#define SDR_ABSENT_POWER_S  15

#define SDR_PWR_MAGIC       0x53445057u
#define SDR_PWR_MAX         2
static RTC_NOINIT_ATTR uint32_t s_sdr_pwr_magic;
static RTC_NOINIT_ATTR uint32_t s_sdr_pwr_count;

static void hl_sdr_power_cycle(void);

static bool sdr_auto_power_cycle(const char *why)
{
    if (s_sdr_pwr_magic != SDR_PWR_MAGIC) {
        s_sdr_pwr_magic = SDR_PWR_MAGIC;
        s_sdr_pwr_count = 0;
    }
    if (s_sdr_pwr_count >= SDR_PWR_MAX) {
        ESP_LOGE(TAG, "SDR %s and %lu power cycles did not fix it - stopping. "
                      "Replug the dongle, then 'SDR power'.",
                 why, (unsigned long)s_sdr_pwr_count);
        return false;
    }
    s_sdr_pwr_count++;
    ESP_LOGW(TAG, "SDR %s - power cycling the dongle (attempt %lu of %d)",
             why, (unsigned long)s_sdr_pwr_count, SDR_PWR_MAX);
    hl_sdr_power_cycle();
    return true;
}

static void sdr_health_ok(void)
{
    s_sdr_pwr_magic = SDR_PWR_MAGIC;
    s_sdr_pwr_count = 0;
}

static void settings_task(void *arg)
{
    (void)arg;
    int last_seen  = audio_volume_get();
    int last_saved = last_seen;
    int stable_ms  = 0;
    int stall_next = SDR_STALL_RECOVER_S;
    int stall_tries = 0;
    bool stall_told = false;
    int absent_ms  = 0;
    bool absent_told = false;
    int healthy_ms = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(250));

        {
            int stall = flipper_link_sdr_stall_s();
            bool present = lakeshark_radio_device_ready();

            if (!present) {
                absent_ms += 250;
                healthy_ms = 0;
                if (!absent_told && absent_ms >= SDR_ABSENT_POWER_S * 1000) {
                    absent_ms = 0;
                    if (!sdr_auto_power_cycle("not enumerated")) absent_told = true;
                }
            } else {
                absent_told = false;
                absent_ms = 0;
            }

            if (present && stall == 0) {
                stall_next  = SDR_STALL_RECOVER_S;
                stall_tries = 0;
                stall_told  = false;

                healthy_ms += 250;
                if (healthy_ms >= 10000) {
                    healthy_ms = 0;
                    sdr_health_ok();
                }
            } else if (present && stall >= stall_next) {
                healthy_ms = 0;
                if (stall_tries < SDR_STALL_MAX_TRIES) {
                    stall_tries++;
                    stall_next = stall + SDR_STALL_RETRY_S;
                    ESP_LOGW(TAG, "SDR silent for %ds - attempting in-place USB recovery",
                             stall);
                    lakeshark_radio_recover();
                } else if (!stall_told) {
                    stall_told = true;

                    if (!sdr_auto_power_cycle("silent after three in-place recoveries")) {
                        ESP_LOGE(TAG, "SDR has delivered nothing for %ds and cannot be "
                                      "recovered automatically.", stall);
                    }
                }
            }
        }

        int v = audio_volume_get();

        if (v != last_seen) {
            last_seen = v;
            stable_ms = 0;
            continue;
        }
        if (v == last_saved) continue;

        stable_ms += 250;
        if (stable_ms >= 1500) {
            settings_save_volume(v);
            last_saved = v;
            ESP_LOGI(TAG, "volume %d saved", v);
        }
    }
}

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

static void hl_select_mode_by_name(const char *name)
{
    if      (!strcasecmp(name, "p25"))  select_mode(0);
    else if (!strcasecmp(name, "adsb")) select_mode(1);
    else if (!strcasecmp(name, "fm"))   select_mode(2);
}

static const char *hl_current_mode_name(void) { return s_modes[s_mode].name; }

static void hl_play_test_sound(void) { snd_boot(); }

#define REBOOT_STACK_WORDS (2048 / sizeof(StackType_t))
static StackType_t  s_reboot_stack[REBOOT_STACK_WORDS];
static StaticTask_t s_reboot_tcb;

static void reboot_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(400));
    ESP_LOGW(TAG, "reboot requested by the control head");
    esp_restart();
}

static void hl_reboot(void)
{
    static bool armed = false;
    if (armed) return;
    armed = true;
    xTaskCreateStatic(reboot_task, "fl_reboot", REBOOT_STACK_WORDS, NULL, 6,
                      s_reboot_stack, &s_reboot_tcb);
}

static void c6_drive(int level);
static int  c6_read_en(void);

static void hl_c6_reset(void)
{
    c6_drive(0);
    vTaskDelay(pdMS_TO_TICKS(200));
    c6_drive(1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static int hl_c6_up(void) { return esp_hosted_connect_to_slave(); }

static void hl_sdr_recover(void) { lakeshark_radio_recover(); }

static bool hl_set_log_level(const char *tag, const char *level)
{
    if (!tag || !level) return false;
    for (int i = 0; i < N_LOG_LEVELS; i++) {
        if (!strcasecmp(level, LOG_LEVELS[i].name)) {
            esp_log_level_set(tag, LOG_LEVELS[i].lvl);
            ESP_LOGW(TAG, "log level %s = %s (set from the head)", tag, level);
            return true;
        }
    }
    return false;
}

static void hl_sdr_reset(void)
{

    lakeshark_radio_park();
    vTaskDelay(pdMS_TO_TICKS(200));
    lakeshark_radio_unpark();
}

#define SDRPWR_STACK_WORDS (3072 / sizeof(StackType_t))
static StackType_t  s_sdrpwr_stack[SDRPWR_STACK_WORDS];
static StaticTask_t s_sdrpwr_tcb;
static volatile bool s_sdrpwr_busy = false;

static void sdr_power_task(void *arg)
{
    (void)arg;

    ESP_LOGW(TAG, "SDR power cycle: dropping VBUS (GPIO%d)", USB_VBUS_GPIO);
    lakeshark_radio_park();
    vTaskDelay(pdMS_TO_TICKS(300));

    gpio_set_level(USB_VBUS_GPIO, 0);

    vTaskDelay(pdMS_TO_TICKS(1200));
    gpio_set_level(USB_VBUS_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(800));

    ESP_LOGW(TAG, "SDR power cycle: VBUS restored, restarting to re-enumerate");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static void hl_sdr_power_cycle(void)
{
    if (s_sdrpwr_busy) return;
    s_sdrpwr_busy = true;
    xTaskCreateStatic(sdr_power_task, "sdr_pwr", SDRPWR_STACK_WORDS, NULL, 5,
                      s_sdrpwr_stack, &s_sdrpwr_tcb);
}

static void hl_ble_enable(bool on)
{
    if (on) ble_link_start();
    else    ble_link_stop();
}

static uint32_t hl_uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000LL);
}

static void hl_heap_stats(uint32_t *internal, uint32_t *dma, uint32_t *psram)
{
    if (internal) *internal = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (dma)      *dma      = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DMA);
    if (psram)    *psram    = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

static const char *reset_reason_name(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "poweron";
    case ESP_RST_EXT:      return "ext";
    case ESP_RST_SW:       return "sw";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT:      return "wdt";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO:     return "sdio";
    default:               return "unknown";
    }
}

static void hl_sys_info(char *out, size_t len)
{
    uint32_t up = hl_uptime_s();
    snprintf(out, len,
             "up=%lus rst=%s idf=%s int=%u dma=%u psram=%u mode=%s rtl=%d c6=%d",
             (unsigned long)up, reset_reason_name(), esp_get_idf_version(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             s_modes[s_mode].name,
             lakeshark_radio_device_ready() ? 1 : 0,
             c6_read_en());
}

static const flipper_link_host_t s_link_host = {
    .select_mode_by_name = hl_select_mode_by_name,
    .current_mode_name   = hl_current_mode_name,
    .play_test_sound     = hl_play_test_sound,

    .reboot              = hl_reboot,
    .c6_reset            = hl_c6_reset,
    .c6_up               = hl_c6_up,
    .sdr_reset           = hl_sdr_reset,
    .sdr_recover         = hl_sdr_recover,
    .sdr_power_cycle     = hl_sdr_power_cycle,
    .ble_enable          = hl_ble_enable,
    .sys_info            = hl_sys_info,
    .heap_stats          = hl_heap_stats,
    .uptime_s            = hl_uptime_s,
    .set_log_level       = hl_set_log_level,
};

static int cmd_link(int argc, char **argv)
{
    flipper_link_cfg_t cfg;
    flipper_link_get_cfg(&cfg);

    if (argc < 2) {
        uint32_t rx, tx, bad;
        flipper_link_stats(&rx, &tx, &bad);
        printf("link=%s uart=%d rx_gpio=%d tx_gpio=%d baud=%lu tel=%dHz "
               "verbose=%d  rx_lines=%lu tx_lines=%lu bad=%lu\n",
               flipper_link_running() ? "on" : "off",
               cfg.uart_num, cfg.rx_gpio, cfg.tx_gpio,
               (unsigned long)cfg.baud, cfg.telemetry_hz,
               flipper_link_verbose(),
               (unsigned long)rx, (unsigned long)tx, (unsigned long)bad);
        return 0;
    }

    if (!strcmp(argv[1], "on")) {
        if (flipper_link_running()) { printf("already on\n"); return 0; }
        esp_err_t e = flipper_link_start(&cfg, &s_link_host);
        printf("link start: %s\n", esp_err_to_name(e));
    } else if (!strcmp(argv[1], "off")) {
        flipper_link_stop();
        printf("link off (GPIO%d restored high)\n", cfg.tx_gpio);
    } else if (!strcmp(argv[1], "verbose") && argc >= 3) {
        flipper_link_set_verbose(atoi(argv[2]) != 0);
        printf("verbose=%d\n", flipper_link_verbose());
    } else if (!strcmp(argv[1], "baud") && argc >= 3) {
        cfg.baud = (uint32_t)strtoul(argv[2], NULL, 10);
        printf("reconfigure: %s\n", esp_err_to_name(flipper_link_reconfigure(&cfg)));
    } else if (!strcmp(argv[1], "tel") && argc >= 3) {
        cfg.telemetry_hz = atoi(argv[2]);
        printf("reconfigure: %s\n", esp_err_to_name(flipper_link_reconfigure(&cfg)));
    } else if (!strcmp(argv[1], "scan")) {
        flipper_link_scan_rx(argc >= 3 ? atoi(argv[2]) : 1200);
    } else if (!strcmp(argv[1], "probe")) {
        flipper_link_probe_rx();
    } else if (!strcmp(argv[1], "pins") && argc >= 4) {
        cfg.rx_gpio = atoi(argv[2]);
        cfg.tx_gpio = atoi(argv[3]);
        printf("reconfigure: %s\n", esp_err_to_name(flipper_link_reconfigure(&cfg)));
    } else {
        printf("usage: link [on|off|verbose <0|1>|baud <n>|tel <hz>|pins <rx> <tx>"
               "|scan [ms]|probe]\n"
               "  probe = pull-down continuity test: finds the head's TX wire\n"
               "          even if it landed on the wrong header pin.\n");
    }
    return 0;
}

static int cmd_beep(int argc, char **argv)
{
    (void)argc; (void)argv;
    pa_on();
    audio_out_ensure_unmuted();
    snd_boot();
    printf("boot chime played (vol=%d mute=%d pa=GPIO%d ring_avail=%lu)\n",
           audio_volume_get(), audio_is_muted(), PA_CTRL_GPIO,
           (unsigned long)audio_out_ring_avail());
    return 0;
}

#define C6_EN_GPIO GPIO_NUM_54

static int c6_read_en(void)
{
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << C6_EN_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&in);
    return gpio_get_level(C6_EN_GPIO);
}

static void c6_drive(int level)
{
    gpio_config_t out = {
        .pin_bit_mask = 1ULL << C6_EN_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&out);
    gpio_set_level(C6_EN_GPIO, level);
}

static void c6_release(void)
{
    (void)c6_read_en();
}

static int cmd_c6(int argc, char **argv)
{
    if (argc < 2) {
        printf("c6 en=%d (GPIO%d, undriven read)  1=C6 running, 0=in reset\n",
               c6_read_en(), C6_EN_GPIO);
        printf("usage: c6 <0|1|reset|release>\n"
               "  0/1      drive EN low/high and hold it\n"
               "  reset    pulse EN low 200ms then high, rebooting the C6\n"
               "  release  stop driving; let the board pull-up hold it\n");
        return 0;
    }

    if (!strcmp(argv[1], "up")) {

        printf("c6: connecting to co-processor...\n");
        int e = esp_hosted_connect_to_slave();
        printf("c6: esp_hosted_connect_to_slave -> %d\n", e);
    } else if (!strcmp(argv[1], "reset")) {
        c6_drive(0);
        vTaskDelay(pdMS_TO_TICKS(200));
        c6_drive(1);
        vTaskDelay(pdMS_TO_TICKS(50));
        printf("c6: EN pulsed low->high (C6 rebooting)\n");
    } else if (!strcmp(argv[1], "release")) {
        c6_release();
        vTaskDelay(pdMS_TO_TICKS(20));
        printf("c6: EN released, undriven read = %d\n", c6_read_en());
    } else {
        int lvl = atoi(argv[1]) ? 1 : 0;
        c6_drive(lvl);
        vTaskDelay(pdMS_TO_TICKS(20));
        printf("c6: EN driven %d\n", lvl);
    }
    return 0;
}

static int cmd_ble(int argc, char **argv)
{
    if (argc < 2) {
        char name[32], addr[20], filt[24];
        uint32_t rx, tx, drops;
        ble_link_peer(name, sizeof(name), addr, sizeof(addr));
        ble_link_get_name_filter(filt, sizeof(filt));
        ble_link_stats(&rx, &tx, &drops);
        printf("ble=%s filter=\"%s\" peer=%s [%s] tel=%dHz  rx_lines=%lu "
               "tx_frames=%lu drops=%lu\n",
               ble_link_state_name(), filt,
               name[0] ? name : "-", addr[0] ? addr : "-",
               ble_link_tel_hz(),
               (unsigned long)rx, (unsigned long)tx, (unsigned long)drops);
        if (ble_link_passkey_pending()) {
            printf("*** PAIRING: the head is showing a 6-digit code - "
                   "enter it with:  ble pin <code>\n");
        }
        printf("usage: ble <on|off|rescan|pin <code>|name <substr>|tel <hz>|"
               "verbose <0|1>>\n");
        return 0;
    }

    if (!strcmp(argv[1], "on")) {
        printf("ble start: %s\n", esp_err_to_name(ble_link_start()));
    } else if (!strcmp(argv[1], "off")) {
        ble_link_stop();
        printf("ble off\n");
    } else if (!strcmp(argv[1], "rescan")) {
        ble_link_rescan();
        printf("ble rescanning\n");
    } else if (!strcmp(argv[1], "pin") && argc >= 3) {
        esp_err_t e = ble_link_submit_passkey((uint32_t)strtoul(argv[2], NULL, 10));
        if (e == ESP_ERR_INVALID_STATE) {
            printf("ble: nothing is waiting for a passkey right now\n");
        } else if (e == ESP_ERR_INVALID_ARG) {
            printf("ble: the passkey is 6 digits (0-999999)\n");
        } else {
            printf("ble pin: %s\n", esp_err_to_name(e));
        }
    } else if (!strcmp(argv[1], "name") && argc >= 3) {
        ble_link_set_name_filter(argv[2]);
        printf("ble filter=\"%s\" (rescan to apply)\n", argv[2]);
    } else if (!strcmp(argv[1], "tel") && argc >= 3) {
        ble_link_set_tel_hz(atoi(argv[2]));
        printf("ble tel=%dHz\n", ble_link_tel_hz());
    } else if (!strcmp(argv[1], "verbose") && argc >= 3) {
        ble_link_set_verbose(atoi(argv[2]) != 0);
        printf("ble verbose=%s\n", argv[2]);
    } else {
        printf("usage: ble <on|off|rescan|pin <code>|name <substr>|tel <hz>|"
               "verbose <0|1>>\n");
    }
    return 0;
}

static int cmd_moto(int argc, char **argv)
{
    const char *which = (argc >= 2) ? argv[1] : "all";
    pa_on();
    audio_out_ensure_unmuted();

    if      (!strcmp(which, "on"))    snd_moto_power_on();
    else if (!strcmp(which, "alert")) snd_moto_alert();
    else if (!strcmp(which, "bonk"))  snd_moto_bonk();
    else if (!strcmp(which, "all"))   snd_moto_full();
    else { printf("usage: moto [on|alert|bonk|all]\n"); return 0; }

    printf("played %s (vol=%d, unmute if you hear nothing)\n", which, audio_volume_get());
    return 0;
}

static int cmd_log(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: log <tag|*> <none|error|warn|info|debug|verbose>\n"
               "       log <level>            same as: log * <level>\n\n"
               "Turned down by this firmware (they are hot paths):\n");
        for (size_t i = 0; i < sizeof(LOG_QUIET_TAGS) / sizeof(LOG_QUIET_TAGS[0]); i++) {
            printf("  %s\n", LOG_QUIET_TAGS[i]);
        }
        printf("\nExamples:\n"
               "  log NimBLE debug   see every GATT write again\n"
               "  log * warn         quiet everything while timing something\n"
               "  log ble_link info  just this module\n"
               "Levels are runtime filters only - nothing is compiled out.\n");
        return 0;
    }

    const char *tag = argv[1];
    const char *lvl_s = (argc >= 3) ? argv[2] : NULL;

    if (!lvl_s) {
        lvl_s = argv[1];
        tag = "*";
    }

    for (int i = 0; i < N_LOG_LEVELS; i++) {
        if (!strcasecmp(lvl_s, LOG_LEVELS[i].name)) {
            esp_log_level_set(tag, LOG_LEVELS[i].lvl);
            printf("log %s = %s\n", tag, LOG_LEVELS[i].name);
            return 0;
        }
    }
    printf("unknown level '%s' (none|error|warn|info|debug|verbose)\n", lvl_s);
    return 0;
}

static int cmd_heap(int argc, char **argv)
{
    (void)argc; (void)argv;

    multi_heap_info_t hi;

    heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL);
    printf("internal : free=%u largest=%u min_ever=%u\n",
           (unsigned)hi.total_free_bytes,
           (unsigned)hi.largest_free_block,
           (unsigned)hi.minimum_free_bytes);

    heap_caps_get_info(&hi, MALLOC_CAP_DMA);
    printf("dma      : free=%u largest=%u min_ever=%u\n",
           (unsigned)hi.total_free_bytes,
           (unsigned)hi.largest_free_block,
           (unsigned)hi.minimum_free_bytes);

    heap_caps_get_info(&hi, MALLOC_CAP_SPIRAM);
    printf("psram    : free=%u largest=%u min_ever=%u\n",
           (unsigned)hi.total_free_bytes,
           (unsigned)hi.largest_free_block,
           (unsigned)hi.minimum_free_bytes);

    printf("usb iq   : %d transfer slots in flight, %llu B dropped, ring avail=%u\n",
           esp_libusb_stream_slots(),
           (unsigned long long)esp_libusb_stream_dropped(),
           (unsigned)esp_libusb_stream_avail());

    uint32_t done = 0, dropped = 0, commits = 0;
    settings_write_stats(&done, &dropped, &commits);
    printf("nvs      : %lu writes in %lu commits, %lu dropped\n",
           (unsigned long)done, (unsigned long)commits, (unsigned long)dropped);

    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *ts = calloc(n, sizeof(TaskStatus_t));
    if (ts) {
        n = uxTaskGetSystemState(ts, n, NULL);
        printf("tasks    : %-16s %-8s %s\n", "name", "stack", "free-hi-water");
        for (UBaseType_t i = 0; i < n; i++) {
            bool ext = esp_ptr_external_ram(ts[i].pxStackBase);
            printf("           %-16s %-8s %u%s\n",
                   ts[i].pcTaskName, ext ? "PSRAM" : "internal",
                   (unsigned)ts[i].usStackHighWaterMark,
                   ext ? "   <- must not write flash" : "");
        }
        free(ts);
    }
    return 0;
}

static int cmd_tel(int argc, char **argv)
{
    int n = (argc >= 2) ? atoi(argv[1]) : 1;
    if (n < 1)  n = 1;
    if (n > 60) n = 60;
    char buf[384];
    for (int i = 0; i < n; i++) {
        flipper_link_snapshot(buf, sizeof(buf));
        printf("%s", buf);
        if (i + 1 < n) vTaskDelay(pdMS_TO_TICKS(500));
    }
    return 0;
}

static int cmd_fl(int argc, char **argv)
{
    char line[192] = { 0 };
    for (int i = 1; i < argc; i++) {
        if (i > 1) strlcat(line, " ", sizeof(line));
        strlcat(line, argv[i], sizeof(line));
    }
    if (!line[0]) { printf("usage: fl <protocol line>   e.g. fl FREQ 851.0125\n"); return 0; }

    char reply[256] = { 0 };
    flipper_link_inject(line, reply, sizeof(reply));
    printf("%s", reply[0] ? reply : "(no reply)\n");
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
        { .command = "beep",   .help = "Play the boot chime - proves the speaker path",
          .func = &cmd_beep },
        { .command = "ble",    .help = "BLE control head link (P4 is central)",
          .hint = "<on|off|rescan|pin <code>|name <s>|tel <hz>|verbose <0|1>>",
          .func = &cmd_ble },
        { .command = "c6",     .help = "ESP32-C6 enable line (GPIO54)",
          .hint = "<0|1|reset|invreset>", .func = &cmd_c6 },
        { .command = "link",   .help = "Flipper serial head control",
          .hint = "[on|off|verbose <0|1>|baud <n>|tel <hz>|pins <rx> <tx>]",
          .func = &cmd_link },
        { .command = "moto",   .help = "Motorola-style radio alerts on the speaker",
          .hint = "[on|alert|bonk|all]", .func = &cmd_moto },
        { .command = "heap",   .help = "Internal/DMA/PSRAM free, USB IQ slots, NVS write stats",
          .func = &cmd_heap },
        { .command = "log",    .help = "Runtime log level per tag (hot tags are quiet by default)",
          .hint = "<tag|*> <none|error|warn|info|debug|verbose>", .func = &cmd_log },
        { .command = "tel",    .help = "Print the flipper-link telemetry frame N times (0.5s apart)",
          .hint = "[n]", .func = &cmd_tel },
        { .command = "fl",     .help = "Run one flipper-link protocol line locally",
          .hint = "<PING|FREQ|VOL|GAIN|DEMOD|...>", .func = &cmd_fl },
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

    ESP_LOGI(TAG, "LakeShark headless boot - radio core, no display (NANO)");
    ESP_LOGI(TAG, "BOOT button (GPIO%d) cycles P25 -> ADS-B -> FM", BOOT_BTN_GPIO);

    ESP_LOGW(TAG, "heap before C6/BLE: internal=%u DMA=%u largest-DMA=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    {
        int e = esp_hosted_connect_to_slave();
        ESP_LOGI(TAG, "ESP-Hosted co-processor link: %s (%d)",
                 e == 0 ? "up" : "FAILED", e);

        if (e == 0) {
            esp_err_t be = ble_link_start();
            ESP_LOGI(TAG, "BLE control head link: %s", esp_err_to_name(be));
        }
    }
    ESP_LOGW(TAG, "heap after C6/BLE: internal=%u DMA=%u largest-DMA=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    lakeshark_backend_start();

    lakeshark_set_usb_autoreboot(false);

    {
        const char *resume = lakeshark_recovery_take_app();
        if (resume && *resume) {
            ESP_LOGW(TAG, "rebooted to recover the USB dongle - resuming '%s'", resume);
            hl_select_mode_by_name(!strcasecmp(resume, "ADS-B") ? "adsb" : resume);
        } else {
            select_mode(0);
        }
    }
    lakeshark_radio_unpark();
    audio_volume_set(settings_load_volume());

    vTaskDelay(pdMS_TO_TICKS(700));
    pa_on();
    audio_out_ensure_unmuted();
    snd_boot();

    xTaskCreate(boot_btn_task, "boot_btn", 3072, NULL, 5, NULL);
    xTaskCreateStatic(settings_task, "settings", SETTINGS_STACK_WORDS, NULL, 2,
                      s_settings_stack, &s_settings_tcb);

    vTaskDelay(pdMS_TO_TICKS(1500));
    flipper_link_cfg_t link_cfg = FLIPPER_LINK_CFG_DEFAULT();
    esp_err_t lerr = flipper_link_start(&link_cfg, &s_link_host);
    ESP_LOGI(TAG, "flipper link (rx=GPIO%d tx=GPIO%d @%lu): %s",
             link_cfg.rx_gpio, link_cfg.tx_gpio,
             (unsigned long)link_cfg.baud, esp_err_to_name(lerr));

    esp_log_level_set("P25TEL",  ESP_LOG_ERROR);
    esp_log_level_set("P25DIAG", ESP_LOG_ERROR);
    esp_log_level_set("ADSB",    ESP_LOG_ERROR);

    esp_log_level_set("NimBLE",  ESP_LOG_WARN);

    ESP_LOGW(TAG, "opening BLE telemetry gate: internal=%u DMA=%u largest-DMA=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    ble_link_allow_telemetry(true);

    console_start();
    ESP_LOGI(TAG, "console ready - type 'help' for commands");
}
