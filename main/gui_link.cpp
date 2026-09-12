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
#include "esp_lvgl_port_disp_stats.h"  /**/
#include "link_ctl.h"  /**/
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "esp_hosted.h"

#include "flipper_link.h"
#include "ble_link.h"
#include "ls_sweep.h"   /**/
#include "adsb_demo.h"    /**/
/**/
#include "ls_wifi.h"
#include "lakeshark_backend.h"
#include "radio_health.h"
#include "audio_out.h"
#include "ls_board.h"
#include "screenshot.h"   /**/
/**/
extern "C" {
#include "ls_version.h"
}

#include "shell/ls_shell.hpp"
#include "shell/ls_app.hpp"

/* Synthetic aircraft, so the whole chain below the decoder can be
   exercised without waiting for a plane. See adsb_demo.h. */
static int cmd_adsbdemo(int argc, char **argv)
{
    if (argc >= 2) adsb_demo_set(atoi(argv[1]));
    int n = adsb_demo_count();
    if (n) {
        printf("%d SYNTHETIC aircraft - generated, not received.\n"
               "   `adsbdemo 0` turns them off.\n", n);
    } else {
        printf("demo aircraft off.  `adsbdemo <1-8>` creates some.\n");
    }
    return 0;
}

/* Print the telemetry line the head actually receives.

   headless_main.c has had this since ; the LCD build never did, so the
   only way to see a frame on the board people actually use was to read it out
   of the Flipper's log over a BLE link that might itself be the thing under
   suspicion. A whole day of wire-format debugging went that way. */
static int cmd_tel(int argc, char **argv)
{
    int n = (argc >= 2) ? atoi(argv[1]) : 1;
    if (n < 1)  n = 1;
    if (n > 60) n = 60;
    char buf[512];
    for (int i = 0; i < n; i++) {
        int len = flipper_link_snapshot(buf, sizeof(buf));
        printf("%s", buf);
        printf("  [%d bytes]\n", len);
        if (i + 1 < n) vTaskDelay(pdMS_TO_TICKS(500));
    }
    return 0;
}

/* `sweep` - the Phase 0 primitive, as a console command. */

static bool sweep_parse_hz(const char *s, uint64_t *out, bool bare_is_mhz)
{
    if (!s || !*s) return false;
    char   *end = NULL;
    double  v   = strtod(s, &end);
    if (end == s || v <= 0) return false;
    double mult = 1.0;
    if (*end == 'k' || *end == 'K') mult = 1e3;
    else if (*end == 'm' || *end == 'M') mult = 1e6;
    else if (*end == 'g' || *end == 'G') mult = 1e9;
    else if (bare_is_mhz && v < 10000.0) mult = 1e6;
    *out = (uint64_t)(v * mult + 0.5);
    return true;
}

static void sweep_progress(uint32_t tune, uint32_t n, void *)
{
    if (n >= 8 && (tune % (n / 8)) == 0) { printf("."); fflush(stdout); }
}

static int cmd_sweep(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: sweep <start> <stop> [bin_hz] [gain_tenths]\n"
               "       sweep 88M 108M          broadcast FM, 25 kHz bins\n"
               "       sweep 433M 435M 5k      ISM, 5 kHz bins\n");
        return 0;
    }

    uint64_t start = 0, stop = 0;
    if (!sweep_parse_hz(argv[1], &start, true) ||
        !sweep_parse_hz(argv[2], &stop, true)) {
        printf("sweep: could not read the frequency range\n");
        return 1;
    }
    uint32_t bin_hz = 25000;
    if (argc >= 4) { uint64_t b = 0; if (sweep_parse_hz(argv[3], &b, false)) bin_hz = (uint32_t)b; }
    int gain = (argc >= 5) ? atoi(argv[4]) : 0;
    bool fast = (argc >= 6) && atoi(argv[5]) != 0;

    ls_sweep_plan_t plan;
    if (!ls_sweep_plan(start, stop, bin_hz, 2400000u, &plan)) {
        printf("sweep: cannot plan that - check stop > start, and that the bin\n"
               "       is not wider than one tune's strip\n");
        return 1;
    }

    int8_t *dbfs = (int8_t *)heap_caps_malloc(plan.n_bins, MALLOC_CAP_SPIRAM);
    if (!dbfs) dbfs = (int8_t *)malloc(plan.n_bins);
    if (!dbfs) { printf("sweep: out of memory for %u bins\n", (unsigned)plan.n_bins); return 1; }

    printf("sweep %.4f-%.4f MHz  %u bins @ %u Hz  %u tunes\n",
           start / 1e6, stop / 1e6, (unsigned)plan.n_bins,
           (unsigned)plan.bin_hz, (unsigned)plan.n_tunes);

    int64_t t0 = esp_timer_get_time();
    ls_radio_err_t e = ls_sweep_run(&plan, gain, 0, fast, dbfs, sweep_progress, NULL);
    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    printf("\n");

    if (e != LS_RADIO_OK) {
        printf("sweep: %s%s\n", ls_radio_err_name(e),
               e == LS_RADIO_ERR_BUSY
                   ? " - an app owns the receiver; go to HOME first" : "");
        free(dbfs);
        return 1;
    }

    /* Peak, floor and the strongest bins. A finder's answer is "what is here
       and how loud", not a picture. */
    int      peak_i = -1;
    long     sum    = 0;
    uint32_t n_real = 0;
    for (uint32_t i = 0; i < plan.n_bins; i++) {
        if (dbfs[i] == LS_SWEEP_NO_DATA) continue;
        if (peak_i < 0 || dbfs[i] > dbfs[peak_i]) peak_i = (int)i;
        sum += dbfs[i];
        n_real++;
    }
    if (!n_real) { printf("sweep: nothing measured\n"); free(dbfs); return 1; }

    int floor_db = (int)(sum / (long)n_real);
    printf("%u bins in %lld ms   floor ~%d dBFS   peak %d dBFS @ %.4f MHz   gaps %u\n",
           (unsigned)n_real, (long long)ms, floor_db, dbfs[peak_i],
           ls_sweep_out_hz(&plan, (uint32_t)peak_i) / 1e6,
           (unsigned)ls_sweep_gaps(&plan, dbfs));

    const int thresh = floor_db + 12;
    printf("  signals >= %d dBFS:\n", thresh);
    uint32_t i = 0, listed = 0;
    while (i < plan.n_bins && listed < 24) {
        if (dbfs[i] == LS_SWEEP_NO_DATA || dbfs[i] < thresh) { i++; continue; }
        uint32_t a = i, best = i;
        while (i < plan.n_bins && dbfs[i] != LS_SWEEP_NO_DATA && dbfs[i] >= thresh) {
            if (dbfs[i] > dbfs[best]) best = i;
            i++;
        }
        printf("    %10.4f MHz  %4d dBFS  %6.1f kHz wide\n",
               ls_sweep_out_hz(&plan, best) / 1e6, dbfs[best],
               (double)(i - a) * plan.bin_hz / 1e3);
        listed++;
    }
    if (!listed) printf("    (none)\n");

    free(dbfs);
    return 0;
}

/**/

static const char *TAG = "gui_link";

#define C6_EN_GPIO ((gpio_num_t)LS_BOARD_C6_EN_GPIO)

enum { GM_P25 = 0, GM_ADSB, GM_FM, GM_REC };

static const char *const MODE_NAME[] = { "P25", "ADS-B", "FM", "REC" };

static volatile int  s_pending   = -1;
/**/
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

/**/
static int radio_app_index(const char *n)
{
    if (!n) return -1;
    for (int i = 0; i <= GM_REC; i++)
        if (!strcasecmp(n, MODE_NAME[i])) return i;
    return -1;
}

/**/
/* Announce each late display frame the moment it happens, so the
   console log shows what else was running at that timestamp. Four theories
   about the blue frame were wrong; this replaces the fifth guess with a
   timestamp that can be lined up against everything else in the log. */
static void disp_late_cb(lv_timer_t *)
{
    uint32_t gap = lvgl_port_disp_stats_take_late();
    if (!gap) return;
    const char *who = lvgl_port_disp_stats_late_task();   /**/
    ESP_LOGW(TAG, "display late frame: gap=%lu.%03lu ms interrupted=%s",
             (unsigned long)(gap / 1000), (unsigned long)(gap % 1000),
             who ? who : "?");
}

static void gui_apply_cb(lv_timer_t *)
{
    /* report only a completed visible mode, but keep accepting the
     * newest console request during loading or a failed-stop retry. */
    LsApp *cur = LsShell::instance().current();
    int shown = radio_app_index(cur ? cur->name() : NULL);

    if (shown >= 0 && !LsShell::instance().transitioning()) s_cur_mode = shown;

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

/**/
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
        /* radio_health_tick enumerates endpoints and takes the health
           and endpoint locks. Running that inside the 200 ms LVGL timer made
           unrelated radio recovery/lock contention stall touch and display
           handling. This task and its internal static stack already exist for
           those same recovery actions, so its queue timeout supplies the
           watchdog cadence without another dynamic task or another stack. */
        if (xQueueReceive(s_defer_q, &job, pdMS_TO_TICKS(200)) != pdTRUE) {
            radio_health_tick();
            continue;
        }

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
static void gl_sdr_recover(void)
{
    lakeshark_radio_recover(LS_RADIO_ENDPOINT_RTL_USB);
}

/**/

static bool gl_sdr_power_cycle(void)
{
    ESP_LOGW(TAG, "%s has no VBUS switch on the USB host port - cannot power "
                  "cycle the dongle in software. Replug it, or run "
                  "'SDR recover'.", LS_BOARD_NAME);
    return false;
}

static void gl_ble_enable(bool on)
{
    if (on) ble_link_start();
    else    ble_link_stop();
}

/* The same two calls, reachable from the Settings screen. OFF is the
   only way to stop a head that refuses pairing from retrying forever, and a
   handheld has no serial console to type `ble off` into. */
static bool gl_ble_is_on(void)        { return ble_link_state() != BLE_LINK_OFF; }
static bool gl_ble_is_connected(void) { return ble_link_is_connected(); }

static const ls_link_ctl_t GL_LINK_CTL = {
    .ble_enable       = gl_ble_enable,
    .ble_is_on        = gl_ble_is_on,
    .ble_is_connected = gl_ble_is_connected,
    /**/
    .reboot           = gl_reboot,
    .sdr_reset        = gl_sdr_reset,
};

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
    /**/
    ls_version_info_t vi;
    ls_version_get(&vi);
    snprintf(out, len,
             "up=%lus rst=%s ver=%s dirty=%d idf=%s int=%u dma=%u psram=%u "
             "mode=%s rtl=%d c6=%d c6fw=%s hostfw=%d.%d.%d board=%s",
             (unsigned long)gl_uptime_s(), reset_reason_name(),
             vi.version, ls_version_is_dirty(vi.version) ? 1 : 0,
             esp_get_idf_version(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             gl_current_mode_name(),
             lakeshark_iq_receiver_ready() ? 1 : 0,
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

/**/
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
        /**/
        if (ble_link_stock_head_seen() && rx == 0)
            printf("*** a Flipper is on the air advertising its own BLE "
                   "profile, not ours - open the LakeShark app on it\n");
        printf("usage: ble <on|off|rescan|pin <code>|name <substr>|tel <hz>|"
               "verbose <0|1>|forget>\n");
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
    /**/
    } else if (!strcmp(argv[1], "forget")) {
        esp_err_t e = ble_link_forget_bonds();
        printf("ble forget: %s\n", esp_err_to_name(e));
    } else {
        printf("usage: ble <on|off|rescan|pin <code>|name <substr>|tel <hz>|"
               "verbose <0|1>|forget>\n");
    }
    return 0;
}

/**/
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

/**/
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

/**/ /**/
/* WiFi console. */

static int cmd_wifi(int argc, char **argv)
{
    char st[192];
    if (argc < 2 || !strcmp(argv[1], "status")) {
        ls_wifi_status(st, sizeof(st));
        printf("%s\n", st);
        return 0;
    }
    if (!strcmp(argv[1], "on") || !strcmp(argv[1], "start")) {
        esp_err_t e = ls_wifi_start();
        if (e != ESP_OK) { printf("wifi failed: %s\n", esp_err_to_name(e)); return 0; }
        ls_wifi_status(st, sizeof(st));
        printf("%s\n", st);
        return 0;
    }
    if (!strcmp(argv[1], "off") || !strcmp(argv[1], "stop")) {
        esp_err_t e = ls_wifi_stop();
        if (e != ESP_OK) { printf("wifi stop failed: %s\n", esp_err_to_name(e)); return 0; }
        printf("wifi off - BLE head restarted\n");
        return 0;
    }
    if (!strcmp(argv[1], "scan")) {
        auto *rows = static_cast<ls_wifi_scan_ap_t *>(heap_caps_malloc(
            16 * sizeof(ls_wifi_scan_ap_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!rows) { printf("scan unavailable: low memory\n"); return 0; }
        int n = ls_wifi_sta_scan(rows, 16);
        if (n < 0) { heap_caps_free(rows); printf("scan failed\n"); return 0; }
        if (n == 0) { heap_caps_free(rows); printf("no networks in range\n"); return 0; }
        for (int i = 0; i < n; i++) {
            printf("%3d dBm  %s  %s\n", rows[i].rssi,
                   rows[i].secure ? "wpa" : "open", rows[i].ssid);
        }
        heap_caps_free(rows);
        return 0;
    }
    if (!strcmp(argv[1], "join")) {
        if (argc < 3) { printf("usage: wifi join <ssid> [passphrase]\n"); return 0; }
        const char *ssid = argv[2];
        const char *pass = (argc >= 4) ? argv[3] : "";
        esp_err_t e = ls_wifi_sta_join(ssid, pass);
        /* Wipe the passphrase argv slot so nothing further in this
           command's lifetime can leak it. Console lines already live in a
           parser buffer we do not own; this at least zeroes our copy. */
        if (argc >= 4) memset(argv[3], 0, strlen(argv[3]));
        if (e == ESP_ERR_INVALID_ARG) {
            printf("bad ssid or passphrase (ssid 1..32 chars; pass empty or 8..63)\n");
            return 0;
        }
        if (e != ESP_OK) {
            printf("join failed: %s\n", esp_err_to_name(e));
            return 0;
        }
        printf("joining - `wifi status` for progress\n");
        return 0;
    }
    if (!strcmp(argv[1], "leave")) {
        esp_err_t e = ls_wifi_sta_leave();
        if (e != ESP_OK) { printf("leave failed: %s\n", esp_err_to_name(e)); return 0; }
        printf("sta off\n");
        return 0;
    }
    if (!strcmp(argv[1], "forget")) {
        esp_err_t e = ls_wifi_sta_forget();
        if (e != ESP_OK) { printf("forget failed: %s\n", esp_err_to_name(e)); return 0; }
        printf("stored credentials erased\n");
        return 0;
    }
    printf("usage: wifi [on|off|status|scan|join <ssid> [pass]|leave|forget]\n");
    return 0;
}

/**/
static int cmd_shot(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "list")) {
        char buf[512] = "";
        int n = screenshot_list(buf, sizeof(buf));
        if (n <= 0) printf("no screenshots\n");
        else        printf("%d screenshot(s):\n%s", n, buf);
        return 0;
    }
    char path[96] = "";
    screenshot_result_t result = screenshot_save_from_task(
        argc >= 2 ? argv[1] : "screen", path, sizeof(path));
    if (result == SCREENSHOT_OK)
        printf("wrote %s  (fetch it with 'wifi on')\n", path);
    else
        printf("screenshot failed: %s\n", screenshot_result_message(result));
    return 0;
}

/* The panel intermittently shows a whole-screen blue frame. That is a
   MIPI-DSI DPI underrun - the framebuffer DMA is a continuous PSRAM read (the
   pixel clock is already down at 20 MHz for that reason, ) - and the
   driver reports no underrun event. `disp` prints the refresh cadence instead:
   a frame that underran shows up as an interval longer than one frame period.
   Compare late/max between builds; do not call the blue flash fixed on the
   strength of not having seen one. */
static int cmd_disp(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "reset")) {
        uint32_t threshold = (argc >= 3) ? (uint32_t)strtoul(argv[2], NULL, 10) : 0;
        lvgl_port_disp_stats_reset(threshold);
        printf("disp counters cleared\n");
        return 0;
    }
    lvgl_port_disp_stats_t s;
    lvgl_port_disp_stats_get(&s);
    printf("disp frames=%lu late=%lu max_gap=%lu.%03lu ms last_gap=%lu.%03lu ms "
           "threshold=%lu.%03lu ms uptime=%lu s\n",
           (unsigned long)s.frames, (unsigned long)s.late,
           (unsigned long)(s.max_gap_us / 1000), (unsigned long)(s.max_gap_us % 1000),
           (unsigned long)(s.last_gap_us / 1000), (unsigned long)(s.last_gap_us % 1000),
           (unsigned long)(s.threshold_us / 1000), (unsigned long)(s.threshold_us % 1000),
           (unsigned long)(s.uptime_ms / 1000));
    if (!s.frames)
        printf("no refresh events counted - the panel may not be running\n");
    return 0;
}

static int cmd_mem(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("internal free=%u largest=%u   dma free=%u largest=%u   psram free=%u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    UBaseType_t n = uxTaskGetNumberOfTasks();
    /* The snapshot itself goes to PSRAM - taking internal RAM to measure
       internal RAM would change the number being measured. */
    TaskStatus_t *st = (TaskStatus_t *)heap_caps_malloc(
        n * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!st) { printf("task snapshot unavailable\n"); return 0; }

    UBaseType_t got = uxTaskGetSystemState(st, n, NULL);
    printf("%-18s %6s %6s\n", "task", "unused", "prio");
    for (UBaseType_t i = 0; i < got; i++) {
        printf("%-18s %6u %6u\n",
               st[i].pcTaskName ? st[i].pcTaskName : "?",
               (unsigned)(st[i].usStackHighWaterMark * sizeof(StackType_t)),
               (unsigned)st[i].uxCurrentPriority);
    }
    heap_caps_free(st);
    return 0;
}

static void gui_link_register_commands(void)
{

    const esp_console_cmd_t cmds[] = {
        /**/ /**/
        { .command = "wifi",
          .help = "SoftAP+HTTP (stops BLE) plus station mode (does not)",
          .hint = "on|off|status|scan|join <ssid> [pass]|leave|forget",
          .func = &cmd_wifi, .argtable = NULL },
        { .command = "fl", .help = "Inject a Flipper head command locally",
          .hint = "<line>", .func = &cmd_fl, .argtable = NULL },
        { .command = "ble", .help = "BLE control head link (P4 is central)",
          .hint = "<on|off|rescan|pin <code>|name <s>|tel <hz>|verbose <0|1>|forget>",
          .func = &cmd_ble, .argtable = NULL },
        { .command = "link", .help = "Flipper serial head control",
          .hint = "<on|off|verbose <0|1>|probe>",
          .func = &cmd_link, .argtable = NULL },
        /**/
        { .command = "shot", .help = "Screenshot the panel to a .bmp beside the REC captures",
          .hint = "[name|list]", .func = &cmd_shot, .argtable = NULL },
        /**/
        { .command = "disp", .help = "Panel refresh cadence; long gaps are DPI underruns (blue frames)",
          .hint = "[reset [threshold_us]]", .func = &cmd_disp, .argtable = NULL },
        /**/
        { .command = "mem", .help = "Heap by capability and per-task stack headroom",
          .hint = "", .func = &cmd_mem, .argtable = NULL },
        /**/
        { .command = "adsbdemo", .help = "Synthetic aircraft for testing the map and the link",
          .hint = "<0-8>", .func = &cmd_adsbdemo, .argtable = NULL },
        /**/
        { .command = "tel", .help = "Print raw telemetry frames exactly as the head receives them",
          .hint = "[count]", .func = &cmd_tel, .argtable = NULL },
        /**/
        { .command = "sweep", .help = "Wideband sweep: power vs frequency across a range",
          .hint = "<start> <stop> [bin_hz] [gain_tenths] [fast]", .func = &cmd_sweep, .argtable = NULL },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        esp_console_cmd_register(&cmds[i]);
}

/**/
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

    radio_health_hooks_t health = {};
    health.request_recovery = lakeshark_radio_recover;
    radio_health_init(&health);

    LsApp *cur = LsShell::instance().current();
    int shown = radio_app_index(cur ? cur->name() : NULL);
    if (shown >= 0) s_cur_mode = shown;

    defer_start();
    gui_link_register_commands();

    bsp_display_lock(0);
    lv_timer_create(gui_apply_cb, 200, NULL);
    /**/
    lv_timer_create(disp_late_cb, 100, NULL);
    bsp_display_unlock();

    /**/
    int e = esp_hosted_connect_to_slave();
    if (e != 0) {
        ESP_LOGE(TAG, "C6 co-processor link FAILED (%d) - not starting BLE. "
                      "Do NOT retry past this; it boot-loops.", e);
    } else {
        c6_version_probe();
        /**/
        ls_link_ctl_register(&GL_LINK_CTL);
        esp_err_t be = ble_link_start();
        ESP_LOGW(TAG, "BLE control head link: %s", esp_err_to_name(be));
        /* Rejoin the last WiFi network if one is stored. Does not
           stop BLE. Silent when no credentials are saved. */
        esp_err_t we = ls_wifi_sta_autojoin();
        if (we == ESP_OK) {
            ESP_LOGW(TAG, "WiFi station: rejoining saved network");
        } else if (we != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "WiFi station autojoin: %s", esp_err_to_name(we));
        }
    }

    ESP_LOGW(TAG, "heap after C6/BLE: internal=%u DMA=%u largest-DMA=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    /**/
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
