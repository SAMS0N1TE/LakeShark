#include "gui_link.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_console.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "esp_hosted.h"

#include "flipper_link.h"
#include "ble_link.h"
#include "lakeshark_backend.h"
#include "audio_out.h"
#include "ls_board.h"

#include "shell/ls_shell.hpp"
#include "shell/ls_app.hpp"

/*LS-019*/

static const char *TAG = "gui_link";

#define C6_EN_GPIO ((gpio_num_t)LS_BOARD_C6_EN_GPIO)

enum { GM_P25 = 0, GM_ADSB, GM_FM, GM_REC };

static const char *const MODE_NAME[] = { "P25", "ADS-B", "FM", "REC" };

static volatile int  s_pending   = -1;
/*LS-019*/
static volatile int  s_cur_mode  = GM_P25;
static char          s_c6_fw[16] = "?";

static bool mode_index_by_name(const char *n, int *out)
{
    if (!n) return false;
    if (!strcasecmp(n, "p25"))  { *out = GM_P25;  return true; }
    if (!strcasecmp(n, "adsb")) { *out = GM_ADSB; return true; }
    if (!strcasecmp(n, "ads-b")){ *out = GM_ADSB; return true; }
    if (!strcasecmp(n, "fm"))   { *out = GM_FM;   return true; }
    if (!strcasecmp(n, "rec"))  { *out = GM_REC;  return true; }
    return false;
}

/*LS-020*/
static int radio_app_index(const char *n)
{
    if (!n) return -1;
    for (int i = 0; i <= GM_REC; i++)
        if (!strcasecmp(n, MODE_NAME[i])) return i;
    return -1;
}

/*LS-019*/
static void gui_apply_cb(lv_timer_t *)
{
    LsApp *cur = LsShell::instance().current();
    int shown = radio_app_index(cur ? cur->name() : NULL);

    if (shown >= 0) s_cur_mode = shown;

    int want = s_pending;
    if (want < 0) return;
    s_pending = -1;

    if (!LsShell::instance().launchByName(MODE_NAME[want])) {
        ESP_LOGW(TAG, "no GUI app named '%s' - mode unchanged", MODE_NAME[want]);
        return;
    }
    s_cur_mode = want;
}

static void gl_select_mode_by_name(const char *name)
{
    int m;
    if (!mode_index_by_name(name, &m)) {
        ESP_LOGW(TAG, "unknown mode name '%s' - staying on %s",
                 name ? name : "(null)", gui_link_mode_name());
        return;
    }
    s_pending = m;
}

static const char *gl_current_mode_name(void)
{
    int m = s_cur_mode;
    if (m < 0 || m > GM_REC) m = GM_P25;
    return MODE_NAME[m];
}

static void gl_play_test_sound(void) { lakeshark_boot_sound(); }

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

static void gl_reboot(void)
{
    static bool armed = false;
    if (armed) return;
    armed = true;
    xTaskCreateStatic(reboot_task, "fl_reboot", REBOOT_STACK_WORDS, NULL, 6,
                      s_reboot_stack, &s_reboot_tcb);
}

static void c6_drive(int level)
{
    gpio_config_t out = {};
    out.pin_bit_mask = 1ULL << (int)C6_EN_GPIO;
    out.mode         = GPIO_MODE_OUTPUT;
    out.pull_up_en   = GPIO_PULLUP_DISABLE;
    out.pull_down_en = GPIO_PULLDOWN_DISABLE;
    out.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&out);
    gpio_set_level(C6_EN_GPIO, level);
}

static int c6_read_en(void)
{
    return gpio_get_level(C6_EN_GPIO);
}

/*LS-301*/
typedef enum { DEFER_NONE = 0, DEFER_SDR_RESET, DEFER_C6_RESET } defer_job_t;

#define DEFER_STACK_WORDS (3072 / sizeof(StackType_t))
static StackType_t   s_defer_stack[DEFER_STACK_WORDS];
static StaticTask_t  s_defer_tcb;
static QueueHandle_t s_defer_q = NULL;

static void defer_task(void *arg)
{
    (void)arg;
    defer_job_t job;
    for (;;) {
        if (xQueueReceive(s_defer_q, &job, portMAX_DELAY) != pdTRUE) continue;

        vTaskDelay(pdMS_TO_TICKS(150));

        switch (job) {
        case DEFER_SDR_RESET:
            ESP_LOGW(TAG, "SDR reset requested by the head");
            lakeshark_radio_park();
            vTaskDelay(pdMS_TO_TICKS(200));
            lakeshark_radio_unpark();
            break;
        case DEFER_C6_RESET:
            ESP_LOGW(TAG, "C6 reset requested by the head - the BLE link will "
                          "drop until the co-processor is back");
            c6_drive(0);
            vTaskDelay(pdMS_TO_TICKS(200));
            c6_drive(1);
            vTaskDelay(pdMS_TO_TICKS(50));
            break;
        default:
            break;
        }
    }
}

static void defer_start(void)
{
    if (s_defer_q) return;
    s_defer_q = xQueueCreate(4, sizeof(defer_job_t));
    if (!s_defer_q) {
        ESP_LOGE(TAG, "deferred job queue alloc failed");
        return;
    }
    xTaskCreateStatic(defer_task, "gl_defer", DEFER_STACK_WORDS, NULL, 4,
                      s_defer_stack, &s_defer_tcb);
}

static void defer_post(defer_job_t job)
{
    if (!s_defer_q) return;
    xQueueSend(s_defer_q, &job, 0);
}

static void gl_sdr_reset(void)  { defer_post(DEFER_SDR_RESET); }
static void gl_c6_reset(void)   { defer_post(DEFER_C6_RESET); }
static int  gl_c6_up(void)      { return esp_hosted_connect_to_slave(); }
static void gl_sdr_recover(void){ lakeshark_radio_recover(); }

/*LS-904*/
static void gl_sdr_power_cycle(void)
{
    ESP_LOGW(TAG, "%s has no VBUS switch on the USB host port - cannot power "
                  "cycle the dongle in software. Replug it, or run "
                  "'SDR recover'.", LS_BOARD_NAME);
}

static void gl_ble_enable(bool on)
{
    if (on) ble_link_start();
    else    ble_link_stop();
}

static uint32_t gl_uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000LL);
}

static void gl_heap_stats(uint32_t *internal, uint32_t *dma, uint32_t *psram)
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

static void gl_sys_info(char *out, size_t len)
{
    snprintf(out, len,
             "up=%lus rst=%s idf=%s int=%u dma=%u psram=%u mode=%s rtl=%d c6=%d "
             "c6fw=%s hostfw=%d.%d.%d board=%s",
             (unsigned long)gl_uptime_s(), reset_reason_name(),
             esp_get_idf_version(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             gl_current_mode_name(),
             lakeshark_radio_device_ready() ? 1 : 0,
             c6_read_en(), s_c6_fw,
             ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1,
             ESP_HOSTED_VERSION_PATCH_1, LS_BOARD_NAME);
}

static const struct { const char *name; esp_log_level_t lvl; } LOG_LEVELS[] = {
    { "none",    ESP_LOG_NONE    },
    { "error",   ESP_LOG_ERROR   },
    { "warn",    ESP_LOG_WARN    },
    { "info",    ESP_LOG_INFO    },
    { "debug",   ESP_LOG_DEBUG   },
    { "verbose", ESP_LOG_VERBOSE },
};
#define N_LOG_LEVELS ((int)(sizeof(LOG_LEVELS) / sizeof(LOG_LEVELS[0])))

static bool gl_set_log_level(const char *tag, const char *level)
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

static const flipper_link_host_t s_link_host = {
    .select_mode_by_name = gl_select_mode_by_name,
    .current_mode_name   = gl_current_mode_name,
    .play_test_sound     = gl_play_test_sound,

    .reboot              = gl_reboot,
    .c6_reset            = gl_c6_reset,
    .c6_up               = gl_c6_up,
    .sdr_reset           = gl_sdr_reset,
    .sdr_recover         = gl_sdr_recover,
    .sdr_power_cycle     = gl_sdr_power_cycle,
    .ble_enable          = gl_ble_enable,
    .sys_info            = gl_sys_info,
    .heap_stats          = gl_heap_stats,
    .uptime_s            = gl_uptime_s,
    .set_log_level       = gl_set_log_level,
};

const char *gui_link_mode_name(void) { return gl_current_mode_name(); }

/*LS-019*/
static int cmd_ble(int argc, char **argv)
{
    if (argc < 2) {
        char peer[32] = "", addr[24] = "";
        uint32_t rx = 0, tx = 0, drops = 0;
        ble_link_peer(peer, sizeof(peer), addr, sizeof(addr));
        ble_link_stats(&rx, &tx, &drops);
        printf("ble: %s peer=\"%s\" %s rx=%lu tx=%lu drops=%lu tel=%dHz\n",
               ble_link_state_name(), peer, addr,
               (unsigned long)rx, (unsigned long)tx, (unsigned long)drops,
               ble_link_tel_hz());
        if (ble_link_passkey_pending())
            printf("*** PAIRING: the head is showing a 6-digit code - "
                   "enter it with:  ble pin <code>\n");
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
        if (e == ESP_ERR_INVALID_STATE) printf("ble: nothing is waiting for a passkey right now\n");
        else if (e == ESP_ERR_INVALID_ARG) printf("ble: the passkey is 6 digits (0-999999)\n");
        else printf("ble pin: %s\n", esp_err_to_name(e));
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

/*LS-019*/
static int cmd_link(int argc, char **argv)
{
    if (argc < 2) {
        flipper_link_cfg_t cfg;
        uint32_t rx = 0, tx = 0, bad = 0;
        flipper_link_get_cfg(&cfg);
        flipper_link_stats(&rx, &tx, &bad);
        printf("link: %s rx=GPIO%d tx=GPIO%d @%lu rx_lines=%lu tx_lines=%lu bad=%lu "
               "verbose=%d mode=%s\n",
               flipper_link_running() ? "on" : "off",
               cfg.rx_gpio, cfg.tx_gpio, (unsigned long)cfg.baud,
               (unsigned long)rx, (unsigned long)tx, (unsigned long)bad,
               flipper_link_verbose() ? 1 : 0, gl_current_mode_name());
        printf("usage: link <on|off|verbose <0|1>|probe>\n");
        return 0;
    }

    if (!strcmp(argv[1], "on")) {
        flipper_link_cfg_t cfg = FLIPPER_LINK_CFG_DEFAULT();
        cfg.rx_gpio = LS_BOARD_LINK_RX_GPIO;
        cfg.tx_gpio = LS_BOARD_LINK_TX_GPIO;
        printf("link start: %s\n", esp_err_to_name(flipper_link_start(&cfg, &s_link_host)));
    } else if (!strcmp(argv[1], "off")) {
        flipper_link_stop();
        printf("link off\n");
    } else if (!strcmp(argv[1], "verbose") && argc >= 3) {
        flipper_link_set_verbose(atoi(argv[2]) != 0);
        printf("link verbose=%s\n", argv[2]);
    } else if (!strcmp(argv[1], "probe")) {
        printf("link probe: rx idle level %d\n", flipper_link_probe_rx());
    } else {
        printf("usage: link <on|off|verbose <0|1>|probe>\n");
    }
    return 0;
}

/*LS-021*/
static int cmd_fl(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: fl <line>   - run a head command locally and print the reply\n"
               "       fl MODE FM        fl STAT        fl REC ARM\n");
        return 0;
    }

    char line[160];
    int  n = 0;
    for (int i = 1; i < argc && n < (int)sizeof(line) - 1; i++)
        n += snprintf(line + n, sizeof(line) - n, "%s%s", i > 1 ? " " : "", argv[i]);

    char reply[512];
    reply[0] = '\0';
    flipper_link_inject(line, reply, sizeof(reply));
    printf("%s\n", reply[0] ? reply : "(no reply)");
    return 0;
}

static void gui_link_register_commands(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "fl", .help = "Inject a Flipper head command locally",
          .hint = "<line>", .func = &cmd_fl, .argtable = NULL },
        { .command = "ble", .help = "BLE control head link (P4 is central)",
          .hint = "<on|off|rescan|pin <code>|name <s>|tel <hz>|verbose <0|1>>",
          .func = &cmd_ble, .argtable = NULL },
        { .command = "link", .help = "Flipper serial head control",
          .hint = "<on|off|verbose <0|1>|probe>",
          .func = &cmd_link, .argtable = NULL },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        esp_console_cmd_register(&cmds[i]);
}

/*LS-305*/
static void c6_version_probe(void)
{
    esp_hosted_coprocessor_fwver_t v = {};
    if (esp_hosted_get_coprocessor_fwversion(&v) != 0) {
        ESP_LOGW(TAG, "C6 firmware version query failed");
        return;
    }
    snprintf(s_c6_fw, sizeof(s_c6_fw), "%lu.%lu.%lu",
             (unsigned long)v.major1, (unsigned long)v.minor1,
             (unsigned long)v.patch1);

    bool skew = ((uint32_t)ESP_HOSTED_VERSION_MAJOR_1 != v.major1 ||
                 (uint32_t)ESP_HOSTED_VERSION_MINOR_1 != v.minor1);
    ESP_LOGW(TAG, "C6 esp_hosted: host %d.%d.%d, co-processor %s%s",
             ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1,
             ESP_HOSTED_VERSION_PATCH_1, s_c6_fw,
             skew ? "  <-- MISMATCH, expect RPC/HCI timeouts" : "");
}

void gui_link_start(void)
{
    LsApp *cur = LsShell::instance().current();
    int shown = radio_app_index(cur ? cur->name() : NULL);
    if (shown >= 0) s_cur_mode = shown;

    defer_start();
    gui_link_register_commands();

    bsp_display_lock(0);
    lv_timer_create(gui_apply_cb, 200, NULL);
    bsp_display_unlock();

    /*LS-110*/
    int e = esp_hosted_connect_to_slave();
    if (e != 0) {
        ESP_LOGE(TAG, "C6 co-processor link FAILED (%d) - not starting BLE. "
                      "Do NOT retry past this; it boot-loops (LS-110).", e);
    } else {
        c6_version_probe();
        esp_err_t be = ble_link_start();
        ESP_LOGW(TAG, "BLE control head link: %s", esp_err_to_name(be));
    }

    ESP_LOGW(TAG, "heap after C6/BLE: internal=%u DMA=%u largest-DMA=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    /*LS-003*/
    flipper_link_cfg_t cfg = FLIPPER_LINK_CFG_DEFAULT();
    cfg.rx_gpio = LS_BOARD_LINK_RX_GPIO;
    cfg.tx_gpio = LS_BOARD_LINK_TX_GPIO;
    esp_err_t lerr = flipper_link_start(&cfg, &s_link_host);
    ESP_LOGW(TAG, "flipper link (rx=GPIO%d tx=GPIO%d @%lu): %s",
             cfg.rx_gpio, cfg.tx_gpio, (unsigned long)cfg.baud,
             esp_err_to_name(lerr));

    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    ble_link_allow_telemetry(true);
}
