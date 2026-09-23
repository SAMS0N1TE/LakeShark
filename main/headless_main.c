#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_console.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "ls_ctl.h"
#include "cell_performance.h"
/**/
#include "ls_crash.h"
#include "ls_nvs_safe.h"
/**/
#include "ls_safe_mode.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"

#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "lakeshark_backend.h"
#include "fm_state.h"
#include "fm_mode_label.h"
#include "audio_out.h"
#include "tone.h"
#include "flipper_link.h"
#include "esp_hosted.h"
#include "ble_link.h"
#include "ls_wifi.h"
#if CONFIG_LS_BLE_HEAD
#include "ble_hci_rx_guard.h"
#endif
#include "settings.h"
/**/
#include "radio_health.h"
#include "rtlsdr_dev.h"
/* rtlsdr_stream_* live here; esp_libusb.h is private to the radio component. */
#include "rtl-sdr.h"
/**/
#include "rec_state.h"
#include "rec_watch.h"
#include "p25_p2_runtime.h"
#include "p25_p2_bench.h"
#include "p25_state.h"
#include "scan_engine.h"
#include "app_registry.h"
/**/
#include "rec_space.h"
#include "ls_board.h"
#include "ls_vitals.h"
#include "ls_sdcard.h"
#include "ls_keypad.h"
#include "ls_gauge.h"
#include "ls_board_hw.h"
#include "ls_lora.h"
#include "ls_hosted_board.h"
#include "ls_rtc.h"
#include "ls_gps.h"
#include "ls_track_log.h"
#include "ls_mesh.h"
#include "ls_audio_hw.h"
#include "ls_haptic.h"
#include "tui/ls_notify.h"
#include "ls_panel.h"
#include "compact_ui.h"
#include "ls_i2c.h"
#include "ls_imu.h"
#include <math.h>
/**/
#include "ls_version.h"
#include "ls_cpu_busy.h"

#include "tui/ls_tui_screen.h"

static const char *TAG = "headless";

/**/
#define BOOT_BTN_GPIO   ((gpio_num_t)LS_BOARD_BOOT_BTN_GPIO)
#define USB_VBUS_GPIO   ((gpio_num_t)LS_BOARD_VBUS_EN_GPIO)
#define PA_CTRL_GPIO    ((gpio_num_t)LS_BOARD_PA_EN_GPIO)

/* Not every board drives its PA from a GPIO.  On the T-Display-P4
   audio power is an XL9535 expander output, so the variant pins
   LS_BOARD_PA_EN_GPIO to -1 - and the old unconditional code built a pin
   mask with (1ULL << -1), which is undefined behaviour, then drove header
   pin 53.  With the keyboard expansion fitted that pin is the nRF24L01+'s
   CE line.  Gate on the capability, not on the presence of a macro. */
#if LS_HAS_AUDIO && defined(LS_BOARD_PA_EN_GPIO) && (LS_BOARD_PA_EN_GPIO >= 0)
#define LS_PA_CTRL 1
#else
#define LS_PA_CTRL 0
#endif

/**/
#ifdef CONFIG_LS_DEFAULT_VOLUME
#define DEFAULT_VOLUME  CONFIG_LS_DEFAULT_VOLUME
#else
#define DEFAULT_VOLUME  50
#endif

typedef struct {
    const char *name;
    void      (*select)(void);
} hl_mode_t;

static const hl_mode_t s_modes[] = {
    { "P25",   lakeshark_select_p25  },
    { "ADS-B", lakeshark_select_adsb },
    { "FM",    lakeshark_select_fm   },
    /**/
    { "REC",   lakeshark_select_rec  },
};
#define N_MODES ((int)(sizeof(s_modes) / sizeof(s_modes[0])))

#define FM_IDX 2

static volatile int s_mode = 0;

#if LS_PA_CTRL
static void pa_on(void) { gpio_set_level(PA_CTRL_GPIO, 1); }
#else
static void pa_on(void) {   }
#endif

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
/**/
#define NVS_MODE "mode"
/**/
#define NVS_MODENM "modenm"

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

/**/
static int settings_load_mode(void)
{
    nvs_handle_t h;
    int idx = 0;

    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        /**/
        char   name[16];
        size_t len = sizeof(name);
        if (nvs_get_str(h, NVS_MODENM, name, &len) == ESP_OK) {
            for (int i = 0; i < N_MODES; i++) {
                if (!strcasecmp(name, s_modes[i].name)) { idx = i; break; }
            }
        } else {
            int32_t m = 0;
            if (nvs_get_i32(h, NVS_MODE, &m) == ESP_OK && m >= 0 && m < N_MODES) {
                idx = (int)m;
            }
        }
        nvs_close(h);
    }
    return idx;
}

static void settings_save_mode(int m)
{
    if (m < 0 || m >= N_MODES) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    /**/
    nvs_set_str(h, NVS_MODENM, s_modes[m].name);
    nvs_commit(h);
    nvs_close(h);
}

#define SETTINGS_STACK_WORDS (3072 / sizeof(StackType_t))
static RTC_NOINIT_ATTR StackType_t s_settings_stack[SETTINGS_STACK_WORDS];
static StaticTask_t s_settings_tcb;

#define BOOT_BTN_STACK_BYTES 3072
static RTC_NOINIT_ATTR StackType_t
    s_boot_btn_stack[BOOT_BTN_STACK_BYTES / sizeof(StackType_t)];
static StaticTask_t s_boot_btn_tcb;

/**/ /**/
#define SDR_ABSENT_POWER_S  30

#define SDR_PWR_MAGIC       0x53445057u
#define SDR_PWR_MAX         2
static RTC_NOINIT_ATTR uint32_t s_sdr_pwr_magic;
static RTC_NOINIT_ATTR uint32_t s_sdr_pwr_count;

/* The two ways a power cycle can be refused, declared where the
   caller can see them. See hl_sdr_power_cycle_try. */
/* A board with no VBUS switch now resets the USB root port instead,
   so "no switch" is not a refusal anywhere any more. What such a board can
   still refuse is a reset with nothing enumerated for it to act on. */
typedef enum {
    SDRPWR_STARTED = 0,
    SDRPWR_NOTHING_ENUMERATED, /* no device on the port for a reset */
    SDRPWR_BUSY,               /* a cycle is already in flight */
} sdrpwr_result_t;
static sdrpwr_result_t hl_sdr_power_cycle_try(void);
static bool hl_sdr_power_cycle_running(void);
#define SDR_CYCLE_NOUN (LS_HAS_VBUS_CTRL ? "power cycle" : "root-port reset")

static bool hl_sdr_power_cycle(void);
/**/
static bool hl_sdr_power_cycle_now(void);

/**/
static bool sdr_auto_power_cycle(const char *why)
{
    if (s_sdr_pwr_magic != SDR_PWR_MAGIC) {
        s_sdr_pwr_magic = SDR_PWR_MAGIC;
        s_sdr_pwr_count = 0;
    }
    if (s_sdr_pwr_count >= SDR_PWR_MAX) {
        ESP_LOGE(TAG, "SDR %s and %lu %ss did not fix it - stopping. "
                      "Replug the dongle, then 'rtl reset'.",
                 why, (unsigned long)s_sdr_pwr_count, SDR_CYCLE_NOUN);
        return false;
    }
    /**/
    /* Each refusal named as itself. A board with no VBUS switch and a
       board already cycling want different things from whoever is reading. */
    const sdrpwr_result_t r = hl_sdr_power_cycle_try();
    if (r == SDRPWR_NOTHING_ENUMERATED) {
        ESP_LOGE(TAG, "SDR %s, and nothing is enumerated on the USB host port "
                      "for a %s to act on - replug the dongle. Not counted "
                      "against the budget.", why, SDR_CYCLE_NOUN);
        return false;
    }
    if (r == SDRPWR_BUSY) {
        ESP_LOGE(TAG, "SDR %s, and a %s is already running - not "
                      "counting it against the budget.", why, SDR_CYCLE_NOUN);
        return false;
    }
    s_sdr_pwr_count++;
    ESP_LOGW(TAG, "SDR %s - %s (attempt %lu of %d)", why,
             LS_HAS_VBUS_CTRL ? "power cycling the dongle"
                              : "resetting the USB root port",
             (unsigned long)s_sdr_pwr_count, SDR_PWR_MAX);
    return true;
}

static void sdr_health_ok(void)
{
    s_sdr_pwr_magic = SDR_PWR_MAGIC;
    s_sdr_pwr_count = 0;
}

/**/
static bool hl_health_power_cycle(const char *endpoint_id)
{
    if (!endpoint_id || strcmp(endpoint_id, LS_RADIO_ENDPOINT_RTL_USB) != 0)
        return false;
    return sdr_auto_power_cycle("the pipe stayed silent through every in-place recovery");
}

static void hl_health_recover(const char *endpoint_id)
{
    lakeshark_radio_recover(endpoint_id);
}

static void settings_task(void *arg)
{
    (void)arg;
    int last_seen  = audio_volume_get();
    int last_saved = last_seen;
    int stable_ms  = 0;
    int absent_ms  = 0;
    bool absent_told = false;
    int healthy_ms = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(250));

        /* settings_task already has the watchdog cadence.  Sharing
           it preserves internal RAM for cache-safe radio task stacks. */
        radio_health_tick();

        /**/
        {
            const char *usb_endpoint = LS_RADIO_ENDPOINT_RTL_USB;
            bool present = lakeshark_radio_endpoint_ready(usb_endpoint);
            if (!present && lakeshark_radio_endpoint_ready(LS_RADIO_ENDPOINT_HACKRF_USB)) {
                usb_endpoint = LS_RADIO_ENDPOINT_HACKRF_USB;
                present = true;
            }

            if (!present) {
                absent_ms += 250;
                healthy_ms = 0;
                if (!absent_told && absent_ms >= SDR_ABSENT_POWER_S * 1000) {
                    absent_ms = 0;
                    /* A root-port reset acts on a device that is
                       enumerated and not answering. It cannot bring back one
                       that is absent, and on a port holding some other device
                       it would reset that instead, so a board without a VBUS
                       switch only says so here. */
                    if (!LS_HAS_VBUS_CTRL) {
                        ESP_LOGW(TAG, "SDR not enumerated for %d s - replug the "
                                      "dongle", SDR_ABSENT_POWER_S);
                        absent_told = true;
                    } else if (!sdr_auto_power_cycle("not enumerated")) {
                        absent_told = true;
                    }
                }
            } else {
                absent_told = false;
                absent_ms   = 0;

                /* Healthy means samples moving. "ok" alone is also
                   what an idle dongle with a dead control pipe reports, and
                   refilling the budget on that would hand a wedged dongle
                   two more resets every time it sat idle for ten seconds. */
                radio_health_snapshot_t health;
                if (radio_health_get(usb_endpoint, &health) &&
                    health.state == RH_OK && health.bytes_per_second > 0) {
                    healthy_ms += 250;
                    if (healthy_ms >= 10000) {
                        healthy_ms = 0;
                        sdr_health_ok();
                    }
                } else {
                    healthy_ms = 0;
                }
            }
        }

        /**/
        {
            static int mode_seen  = -1;
            static int mode_saved = -1;
            static int mode_ms    = 0;
            int m = s_mode;

            if (mode_saved < 0) mode_saved = settings_load_mode();
            if (m != mode_seen) {
                mode_seen = m;
                mode_ms   = 0;
            } else if (m != mode_saved) {
                mode_ms += 250;
                if (mode_ms >= 2000) {
                    settings_save_mode(m);
                    mode_saved = m;
                    ESP_LOGI(TAG, "mode %s saved", s_modes[m].name);
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
    case 3:  return rec_get_freq();
    default: return 1090000000UL;
    }
}

static int cur_gain_tenths(void)
{
    if (s_mode == 3) {
        rec_status_t rec;
        rec_get_status(&rec);
        return rec.gain_tenths;
    }
    return lakeshark_radio_get_gain_tenths();
}

static void select_mode(int idx)
{
    if(cell_performance_active())return;
    s_mode = idx;
#if LS_HAS_COMPACT_UI
    if (compact_ui_select_mode(s_modes[idx].name)) return;
#endif
    ESP_LOGI(TAG, ">>> mode: %s", s_modes[idx].name);
    s_modes[idx].select();
    pa_on();
}

static void cycle_next(void)
{
    select_mode((s_mode + 1) % N_MODES);
}

#if LS_HAS_COMPACT_UI
static void compact_mode_changed(const char *mode)
{
    for (int i=0;i<N_MODES;++i) if (!strcmp(mode,s_modes[i].name)) { s_mode=i; break; }
}
#endif

/**/
static bool hl_mode_index_by_name(const char *name, int *out)
{
    if (!name || !*name || !out) return false;
    if      (!strcasecmp(name, "p25"))   *out = 0;
    else if (!strcasecmp(name, "adsb") ||
             !strcasecmp(name, "ads-b")) *out = 1;
    else if (!strcasecmp(name, "fm"))    *out = 2;
    else if (!strcasecmp(name, "rec"))   *out = 3;
    else return false;
    return true;
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
                if(cell_performance_active())cell_performance_reboot(false);
                cycle_next();
            }
            prev = stable;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void gpio_init(void)
{
    /**/
    /* Build the mask from the pins this board actually has.  Both
       entries are now optional: VBUS was already, and PA became so on the
       first board whose PA enable is not a GPIO. */
    uint64_t out_mask = 0;
#if LS_PA_CTRL
    out_mask |= (1ULL << PA_CTRL_GPIO);
#endif
#if LS_HAS_VBUS_CTRL
    out_mask |= (1ULL << LS_BOARD_VBUS_EN_GPIO);
#endif

    if (out_mask) {
        gpio_config_t out = {
            .pin_bit_mask = out_mask,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&out);
    }
#if LS_HAS_VBUS_CTRL
    gpio_set_level(USB_VBUS_GPIO, 1);
#endif
#if LS_PA_CTRL
    gpio_set_level(PA_CTRL_GPIO, 1);
#endif

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BOOT_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);
}

static int cmd_p2(int argc,char **argv)
{
    if(argc > 1 && !strcmp(argv[1],"test")) return p25_p2_bench_command(argc,argv);
    if(argc==2 && !strcmp(argv[1],"off"))p25_p2_enable(false);
    else if(argc==2 && !strcmp(argv[1],"on")){scan_engine_stop();p25_p2_enable(true);}
    else if(argc==6 && !strcmp(argv[1],"config")) {
        char *end[4];
        unsigned long w=strtoul(argv[2],&end[0],16),s=strtoul(argv[3],&end[1],16),n=strtoul(argv[4],&end[2],16),slot=strtoul(argv[5],&end[3],10);
        if(*end[0]||*end[1]||*end[2]||*end[3]||w>0xfffff||s>0xfff||n>0xfff||slot<1||slot>2||!p25_p2_config(w,s,n,slot-1)) {
            printf("Invalid WACN/SYS/NAC or slot (1-2)\n");return 1;
        }
    } else if(argc==3 && !strcmp(argv[1],"follow") &&
              (!strcmp(argv[2],"on") || !strcmp(argv[2],"off"))) {
        p25_set_phase2_follow(!strcmp(argv[2],"on"));
    } else if(argc>1 && strcmp(argv[1],"status")) {
        printf("p2 on|off|status|follow on|off|config <WACN hex> <SYS hex> <NAC hex> <slot 1-2>\n");return 1;
    }
    char text[256];p25_p2_describe(text,sizeof(text));printf("p2: %s\n",text);
    p25_p2_follow_describe(text,sizeof(text));printf("p2: %s\n",text);return 0;
}

static int cmd_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    const char *reported_mode = s_modes[s_mode].name;
    uint64_t reported_hz = cur_freq_hz();
    int reported_gain = cur_gain_tenths();
    if (scan_engine_active() && scan_engine_mixed()) {
        const app_t *active = app_current();
        ls_iq_control_status_t rx;
        scan_engine_receiver_status(&rx);
        if (active && active->name) reported_mode = active->name;
        reported_hz = rx.effective_center_known ? rx.effective_center_hz : 0;
        if (rx.effective_gain_known) reported_gain = rx.effective_gain_tenths_db;
    }
    printf("mode=%s  freq=%.4f MHz  vol=%d  gain=%.1f dB  mute=%d  fmmode=%s  "
           "feed=%s  free_int=%u  free_psram=%u  screen_lock=%s  scan=%s  gps_filter=%s\n",
           reported_mode, reported_hz / 1e6,
           audio_volume_get(), reported_gain / 10.0,
               audio_is_muted(), fm_mode_label(lakeshark_fm_get_mode()),
           lakeshark_cartotui_enabled() ? "on" : "off",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), ls_tui_locked() ? "on" : "off",
           scan_engine_active() ? "on" : "off", scan_engine_location() ? "on" : "off");
    /**/
    {
        char rh[160];
        radio_health_report(LS_RADIO_ENDPOINT_RTL_USB, rh, sizeof(rh));
        printf("%s\n", rh);
    }
    return 0;
}

/**/
static int cmd_rtl(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "detach")) {
        printf("simulating a dongle detach - the device object will be torn down.\n"
               "A VBUS cycle, a root-port reset ('rtl reset') or a replug brings it back.\n");
        /* Park and free BEFORE marking the device gone, which is what 's rtlsdr_dev_teardown() exists to do. */

        rtlsdr_dev_teardown();
        return 0;
    }

    /* The operator's way to the recovery the health watchdog takes on
       its own: a VBUS cycle where the board has a switch, a root-port reset
       where it does not. Not counted against the automatic budget, like SDR
       power from the head. Waits for the worker, so what is printed is the
       outcome rather than a promise of one. */
    if (argc >= 2 && !strcmp(argv[1], "reset")) {
        const sdrpwr_result_t r = hl_sdr_power_cycle_try();
        if (r == SDRPWR_BUSY) {
            printf("a %s is already running\n", SDR_CYCLE_NOUN);
            return 0;
        }
        if (r == SDRPWR_NOTHING_ENUMERATED) {
            printf("nothing is enumerated on the USB host port for a %s to act "
                   "on - replug the dongle\n", SDR_CYCLE_NOUN);
            return 0;
        }
        printf("%s started - waiting for it to finish\n", SDR_CYCLE_NOUN);
        for (int i = 0; i < 300 && hl_sdr_power_cycle_running(); i++)
            vTaskDelay(pdMS_TO_TICKS(100));
    }

    char rh[160];
    radio_health_report(LS_RADIO_ENDPOINT_RTL_USB, rh, sizeof(rh));
    printf("%s\n", rh);
    if (argc < 2) printf("usage: rtl [reset|detach]\n");
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
        printf("fm submode=%s (listen|scan|pocsag|wfm|am|acars|flex)\n",
               fm_mode_command_name((fm_mode_t)lakeshark_fm_get_mode()));
        return 0;
    }
    fm_mode_t mode;
    if (!fm_mode_parse(argv[1], &mode)) {
        printf("usage: fm listen|scan|pocsag|wfm|am|acars|flex\n");
        return 0;
    }
    if (s_mode != FM_IDX) select_mode(FM_IDX);
    /* Explicit NFM is analogue-only. Do not inherit mixed P25 demodulation
       from a previous channel-scanner session. */
    if (mode == FM_MODE_LISTEN) scan_engine_set_mixed(false);
    lakeshark_fm_set_mode((int)mode);
    pa_on();
    printf("mode=FM submode=%s\n", fm_mode_command_name(mode));
    return 0;
}

static int cmd_mode(int argc, char **argv)
{
    if (argc < 2) { printf("usage: mode p25|adsb|fm|rec|next\n"); return 0; }
    int idx;
    if (!strcmp(argv[1], "next")) cycle_next();
    /**/
    else if (hl_mode_index_by_name(argv[1], &idx)) select_mode(idx);
    else { printf("unknown mode '%s' (p25|adsb|fm|rec|next)\n", argv[1]); return 0; }
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
    if (s_mode == 3) {
        double mhz = atof(argv[1]);
        if (rec_watch_enabled()) { printf("Stop WATCH before changing frequency\n"); return 0; }
        bool valid = mhz >= 1 && mhz <= 2000;
        if (rec_watch_source() == REC_SOURCE_CC1101)
            valid = (mhz >= 300 && mhz <= 348) || (mhz >= 387 && mhz <= 464) || (mhz >= 779 && mhz <= 928);
        if (!valid) { printf("Frequency outside recorder source range\n"); return 0; }
        rec_set_freq((uint32_t)(mhz * 1e6 + 0.5));
        printf("freq=%.4f MHz\n", rec_get_freq() / 1e6);
        return 0;
    }
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
        printf("gain=%.1f dB\n", cur_gain_tenths() / 10.0);
        return 0;
    }
    if (s_mode == 3) {
        if (rec_watch_source() == REC_SOURCE_CC1101) printf("CC1101 uses its OOK AGC profile\n");
        else if (!strcmp(argv[1], "auto")) printf("RTL recorder uses manual gain\n");
        else {
            rec_set_gain((int)(atof(argv[1]) * 10 + 0.5));
            printf("gain=%.1f dB\n", cur_gain_tenths() / 10.0);
        }
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
    int idx;
    /**/
    if (!hl_mode_index_by_name(name, &idx)) {
        ESP_LOGW(TAG, "unknown mode name '%s' - staying on %s",
                 name ? name : "(null)", s_modes[s_mode].name);
        return;
    }
    select_mode(idx);
#if LS_HAS_COMPACT_UI
    compact_ui_show_mode(s_modes[idx].name);
#endif
}

static const char *hl_current_mode_name(void) { return s_modes[s_mode].name; }
static void hl_show_fm_mode(int mode)
{
#if LS_HAS_COMPACT_UI
    compact_ui_show_mode(mode == FM_MODE_POCSAG ? "POCSAG" : "FM");
#else
    (void)mode;
#endif
}

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

/**/
typedef enum { DEFER_NONE = 0, DEFER_SDR_RESET, DEFER_C6_RESET,
               DEFER_RADIO_WANT } defer_job_t;

/* What the visible TUI screen needs, and what is running.
   Written by ls_tui_radio_want below, read by defer_task; the pair is
   what keeps a screen change from stopping and restarting a receiver
   that was already the right one. */
static volatile int s_radio_want    = -1;   /* index into s_modes, -1 park */
static volatile int s_radio_applied = -2;   /* -2: nothing answered yet    */

#define DEFER_STACK_WORDS (3072 / sizeof(StackType_t))
static StackType_t   s_defer_stack[DEFER_STACK_WORDS];
static StaticTask_t  s_defer_tcb;
static QueueHandle_t s_defer_q = NULL;
static atomic_bool s_console_retry;
static void console_retry_start(void);

/* The receiver the visible TUI screen asked for. */

static int radio_effective_want(void)
{
    int rec=-1;
    (void)hl_mode_index_by_name("REC",&rec);
    return rec_watch_receiver_want_source(s_radio_want,rec,rec_watch_enabled(),rec_watch_source());
}

static void radio_reconcile(void)
{
    const int want = radio_effective_want();
    if (want == s_radio_applied) return;
    s_radio_applied = want;
    if (want < 0) {
        ESP_LOGI(TAG, "no visible screen needs a receiver - parking");
        lakeshark_radio_park();
    } else {
        /* The switch alone, with no unpark after it. */

        ESP_LOGI(TAG, "screen wants %s - starting it", s_modes[want].name);
        select_mode(want);
    }
}

static void defer_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* A timeout, so a lost post is not a lost session. Four
           times a second costs two volatile comparisons on a task that is
           otherwise asleep. */
        defer_job_t job = DEFER_NONE;
        if (xQueueReceive(s_defer_q, &job, pdMS_TO_TICKS(250)) != pdTRUE)
            job = DEFER_NONE;

        if (job != DEFER_NONE) vTaskDelay(pdMS_TO_TICKS(150));

        switch (job) {
        case DEFER_SDR_RESET:
            ESP_LOGW(TAG, "SDR reset requested by the head");
            lakeshark_radio_park();
            vTaskDelay(pdMS_TO_TICKS(200));
            lakeshark_radio_unpark();
            break;
        /* Nothing to do here: the reconcile below is what answers it, and it
           runs whether or not this message ever arrived. */
        case DEFER_RADIO_WANT:
            /* A console mode command can start RX while the UI's cached
               request still says parked. An explicit stop must park the
               actual receiver again, not be discarded as a duplicate. */
            if (s_radio_want < 0) s_radio_applied = -2;
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

        radio_reconcile();
        if (atomic_exchange(&s_console_retry, false)) {
            /* app_main's 8 KB internal stack is still live at the first
               attempt. Let it return and idle reclaim it before retrying.
               This existing internal-stack worker performs setup only;
               the REPL keeps its own internal stack for NVS commands. */
            vTaskDelay(pdMS_TO_TICKS(1000));
            console_retry_start();
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
    xTaskCreateStatic(defer_task, "hl_defer", DEFER_STACK_WORDS, NULL, 4,
                      s_defer_stack, &s_defer_tcb);
}

static void defer_post(defer_job_t job)
{
    if (!s_defer_q) return;

    if (xQueueSend(s_defer_q, &job, 0) != pdTRUE)
        ESP_LOGW(TAG, "deferred job %d dropped: the queue is full", (int)job);
}

static void hl_c6_reset(void) { defer_post(DEFER_C6_RESET); }

static int hl_c6_up(void) { return esp_hosted_connect_to_slave(); }

static void hl_sdr_recover(void)
{
    lakeshark_radio_recover(LS_RADIO_ENDPOINT_RTL_USB);
}

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

/**/
static void hl_sdr_reset(void) { defer_post(DEFER_SDR_RESET); }

/* What the visible TUI screen needs, and what is actually running. */

void ls_tui_radio_want(const char *mode_name)
{
    int want = -1;
    if (mode_name) {
        int idx = 0;
        if (hl_mode_index_by_name(mode_name, &idx)) {
            want = idx;
        } else {
            /* A screen naming a mode the firmware does not have is a bug in
               the screen, and silently parking would hide it. */
            ESP_LOGW(TAG, "a screen asked for receiver '%s', which is not a "
                          "mode this build has", mode_name);
        }
    }
    if (want == s_radio_want && want >= 0) return;
    s_radio_want = want;
    defer_post(DEFER_RADIO_WANT);
}

/* Which receiver mode is claimed, or NULL when the radio is parked. */

const char *ls_tui_radio_claimed(void)
{
    const int want = radio_effective_want();
    if (want < 0 || want >= N_MODES) return NULL;
    return s_modes[want].name;
}

#define SDRPWR_STACK_WORDS (3072 / sizeof(StackType_t))
static StackType_t  s_sdrpwr_stack[SDRPWR_STACK_WORDS];
static StaticTask_t s_sdrpwr_tcb;

/* Who owns the worker slot. The port worker shares this static
   stack, and a static stack may be reused only once the task on it has
   stopped touching it - An earlier fault found rtl_pump restarting on a stack that was
   still running. So the port worker parks itself suspended when it is done
   (FINISHED) instead of deleting itself, and the next caller that wins the
   compare-exchange deletes it from outside before creating the next one.
   The VBUS worker is unchanged: it goes straight back to IDLE. */
enum { SDRPWR_IDLE = 0, SDRPWR_RUNNING, SDRPWR_FINISHED };
static int s_sdrpwr_state = SDRPWR_IDLE;
static TaskHandle_t s_sdrpwr_task;

static bool hl_sdr_power_cycle_running(void)
{
    return __atomic_load_n(&s_sdrpwr_state, __ATOMIC_ACQUIRE) == SDRPWR_RUNNING;
}

/* Called only by the caller that moved FINISHED to RUNNING. */
static bool sdrpwr_reclaim_finished_worker(void)
{
    for (int i = 0; i < 40 && eTaskGetState(s_sdrpwr_task) != eSuspended; i++)
        vTaskDelay(pdMS_TO_TICKS(5));
    if (eTaskGetState(s_sdrpwr_task) != eSuspended) return false;
    vTaskDelete(s_sdrpwr_task);
    s_sdrpwr_task = NULL;
    return true;
}

static void sdr_power_task(void *arg)
{
    (void)arg;

    ESP_LOGW(TAG, "SDR power cycle: dropping VBUS (GPIO%d)", (int)USB_VBUS_GPIO);
    lakeshark_radio_park();
    vTaskDelay(pdMS_TO_TICKS(300));

    gpio_set_level(USB_VBUS_GPIO, 0);

    vTaskDelay(pdMS_TO_TICKS(1200));
    gpio_set_level(USB_VBUS_GPIO, 1);

    /**/
    ESP_LOGW(TAG, "SDR power cycle: VBUS restored - waiting for the dongle to "
                  "re-enumerate on its own");
    for (int i = 0; i < 80; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (lakeshark_radio_endpoint_ready(LS_RADIO_ENDPOINT_RTL_USB)) {
            ESP_LOGW(TAG, "SDR power cycle: re-enumerated after %d ms, no restart needed",
                     (i + 1) * 100);
            __atomic_store_n(&s_sdrpwr_state, SDRPWR_IDLE, __ATOMIC_RELEASE);
            vTaskDelete(NULL);
            return;
        }
    }

    ESP_LOGE(TAG, "SDR power cycle: nothing re-enumerated in 8 s - restarting");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

/* Where a root-port reset cannot bring the dongle back. A reboot is
   what revived it on 2026-09-11, but it is taken only with the operator's
   USB auto-reboot setting on (off by default). The automatic path
   counts each reset against the RTC budget, which survives the restart, so
   a dongle that is still wedged afterwards cannot turn this into a loop. */
static void sdr_port_give_up(const char *what)
{
    if (lakeshark_usb_autoreboot()) {
        ESP_LOGE(TAG, "SDR reset: %s - restarting (USB auto-reboot is on)", what);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
    ESP_LOGE(TAG, "SDR reset: %s. USB auto-reboot is off, so the board stays "
                  "up - replug the dongle or reboot to bring it back.", what);
}

/* The worker for a board with no VBUS switch. rtl_adapter_port_reset
   does the detach-safe teardown and the root-port cycle; this waits for the dongle
   to come back and says what happened. Pinned to core 1 below the USB lib
   task (priority 13 there), because usb_port_cycle.c relies on port events
   being handled before it acts on the port. The boot that revived the wedged
   dongle spent 2.6 s on "Root port reset failed" before the R820T answered,
   hence a longer wait than the VBUS path's 8 s. */
#define SDR_PORT_WAIT_MS 15000

static void sdr_port_task(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "SDR reset: releasing the dongle, then power cycling the USB "
                  "root port (VBUS stays up: this is a disconnect and a bus reset)");
    const usb_port_cycle_result_t r = rtl_adapter_port_reset();
    ESP_LOGW(TAG, "SDR reset: root-port cycle %s", usb_port_cycle_result_name(r));

    if (r == USB_PORT_CYCLE_STILL_OFF) {
        sdr_port_give_up("the USB host port did not power back on");
    } else if (r == USB_PORT_CYCLE_DONE || r == USB_PORT_CYCLE_NOT_RELEASED) {
        int waited_ms = 0;
        while (waited_ms < SDR_PORT_WAIT_MS &&
               !lakeshark_radio_endpoint_ready(LS_RADIO_ENDPOINT_RTL_USB)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            waited_ms += 100;
        }
        if (lakeshark_radio_endpoint_ready(LS_RADIO_ENDPOINT_RTL_USB))
            ESP_LOGW(TAG, "SDR reset: re-enumerated after %d ms, no restart needed",
                     waited_ms);
        else
            sdr_port_give_up("nothing re-enumerated");
    }
    /* NOTHING and OFF_FAILED left the port alone with the dongle released,
       which is where 'rtl detach' leaves it: a replug brings it back. */
    ESP_LOGI(TAG, "SDR reset: worker done, %u bytes of stack to spare",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    /* Parked, not deleted: see SDRPWR_FINISHED. The next caller reclaims it. */
    __atomic_store_n(&s_sdrpwr_state, SDRPWR_FINISHED, __ATOMIC_RELEASE);
    vTaskSuspend(NULL);
}

/**/
/**/
/* WHY it did not start, not just that it did not. */

static sdrpwr_result_t hl_sdr_power_cycle_try(void)
{
    /* Claimed with a compare-exchange, not a test then a set: the
       health watchdog, the console and the head can all ask at once, and two
       workers on one static stack is the worst thing this function could do.
       A finished port worker is still parked on that stack; the caller that
       claims the slot from FINISHED deletes it before reusing the stack. */
    int expected = SDRPWR_IDLE;
    if (!__atomic_compare_exchange_n(&s_sdrpwr_state, &expected,
                                     SDRPWR_RUNNING, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        if (expected != SDRPWR_FINISHED ||
            !__atomic_compare_exchange_n(&s_sdrpwr_state, &expected,
                                         SDRPWR_RUNNING, false,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return SDRPWR_BUSY;
        if (!sdrpwr_reclaim_finished_worker()) {
            __atomic_store_n(&s_sdrpwr_state, SDRPWR_FINISHED, __ATOMIC_RELEASE);
            return SDRPWR_BUSY;
        }
    }
    if (!LS_HAS_VBUS_CTRL) {
        if (!rtl_adapter_port_reset_possible()) {
            ESP_LOGW(TAG, "nothing is enumerated on the USB host port - a "
                          "root-port reset has nothing to act on. Replug the "
                          "dongle.");
            __atomic_store_n(&s_sdrpwr_state, SDRPWR_IDLE, __ATOMIC_RELEASE);
            return SDRPWR_NOTHING_ENUMERATED;
        }
        s_sdrpwr_task = xTaskCreateStaticPinnedToCore(
            sdr_port_task, "sdr_port", SDRPWR_STACK_WORDS, NULL, 5,
            s_sdrpwr_stack, &s_sdrpwr_tcb, 1);
        return SDRPWR_STARTED;
    }
    xTaskCreateStatic(sdr_power_task, "sdr_pwr", SDRPWR_STACK_WORDS, NULL, 5,
                      s_sdrpwr_stack, &s_sdrpwr_tcb);
    return SDRPWR_STARTED;
}

static bool hl_sdr_power_cycle_now(void)
{
    return hl_sdr_power_cycle_try() == SDRPWR_STARTED;
}

static bool hl_sdr_power_cycle(void)
{
    return hl_sdr_power_cycle_now();
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

/**/
static char s_c6_fw[16] = "?";

static void hl_sys_info(char *out, size_t len)
{
    uint32_t up = hl_uptime_s();
    /**/
    /* Firmware version: PROJECT_VER as IDF derived it from git describe
       --dirty.  The head reads this off the SYS reply so a screenshot on
       the Flipper side is enough to tie an observation back to a commit. */
    ls_version_info_t vi;
    ls_version_get(&vi);
    snprintf(out, len,
             "up=%lus rst=%s ver=%s dirty=%d idf=%s int=%u dma=%u psram=%u "
             "mode=%s rtl=%d c6=%d c6fw=%s hostfw=%d.%d.%d board=%s",
             (unsigned long)up, reset_reason_name(),
             vi.version, ls_version_is_dirty(vi.version) ? 1 : 0,
             esp_get_idf_version(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             s_modes[s_mode].name,
              lakeshark_radio_endpoint_ready(LS_RADIO_ENDPOINT_RTL_USB) ? 1 : 0,
             c6_read_en(), s_c6_fw,
             ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1,
             ESP_HOSTED_VERSION_PATCH_1, LS_BOARD_NAME);
}

static const flipper_link_host_t s_link_host = {
    .select_mode_by_name = hl_select_mode_by_name,
    .show_fm_mode = hl_show_fm_mode,
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
#if !LS_HAS_LINK_UART
        /* Saying "link=off uart=2 rx_gpio=33" on a board that has no link
           pins reads as a link that is merely switched off, and sent me
           looking for a cable that cannot exist. This board's variant header
           leaves LS_BOARD_LINK_*_GPIO undefined, so say that instead. */
        printf("link=unsupported on this board - no link UART pins defined\n");
        (void)cfg; (void)rx; (void)tx; (void)bad;
        return 0;
#endif
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
        if(ls_keypad_present()) {printf("link: keyboard radios own the expansion pins\n");return 1;}
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

/**/
static const char *rec_phase_name(rec_phase_t p)
{
    switch (p) {
    case REC_IDLE:      return "idle";
    case REC_ARMED:     return "armed";
    case REC_CAPTURING: return "capturing";
    case REC_DONE:      return "done";
    }
    return "?";
}

static void rec_emit_console(const char *line, void *ctx)
{
    (void)ctx;
    printf("%s\n", line);
}

static int cmd_rec(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "watch")) {
        rec_watch_status_t *watch=heap_caps_malloc(sizeof(*watch),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!watch) {printf("watch: snapshot allocation failed\n");return 1;}
        rec_watch_snapshot(watch);
        printf("watch: ready=%d enabled=%d patterns=%d captures=%lu skipped=%lu\n",
            watch->ready,watch->enabled,watch->count,(unsigned long)watch->received,(unsigned long)watch->dropped);
        printf("watch: %s; alerts=%d queued=%lu refused=%lu limited=%lu\n",watch->storage,
            watch->alerts,(unsigned long)watch->alert_sent,(unsigned long)watch->alert_failed,(unsigned long)watch->alert_suppressed);
        heap_caps_free(watch);return 0;
    }
    if (argc < 2 || !strcmp(argv[1], "status")) {
        rec_status_t s;
        rec_get_status(&s);
        printf("rec %s  %.4f MHz  gain=%.1f dB  edges=%d span=%lu us  captures=%lu\n",
               rec_phase_name(s.phase), s.freq_hz / 1e6, s.gain_tenths / 10.0,
               s.edges, (unsigned long)s.span_us, (unsigned long)s.captures);
        printf("    mag now=%d floor=%d thresh=%d   last=%s\n",
               s.mag_now, s.mag_floor, s.mag_thresh,
               s.last_file[0] ? s.last_file : "-");
        /**/
        {
            char bw[16];
            if (s.bw_hz) snprintf(bw, sizeof(bw), "%.0f kHz", s.bw_hz / 1000.0);
            else         snprintf(bw, sizeof(bw), "auto");
            printf("    bw=%s minp=%lu us maxspan=%lu ms minedges=%d\n",
                   bw, (unsigned long)s.min_pulse_us,
                   (unsigned long)(s.max_span_us / 1000u), s.min_edges);
        }
        /**/
        printf("    ended=%s  mark %lu-%lu us  ~%lu baud\n",
               rec_end_reason_name(s.end_reason),
               (unsigned long)s.min_mark_us, (unsigned long)s.max_mark_us,
               (unsigned long)s.baud_est);
        /**/
        /* Free-space line so the operator can see the volume filling
           up from the console without a separate command.  UINT64_MAX
           means the probe failed (statvfs not implemented on the
           mount, or a permissions error) - report it as "?" rather
           than as zero so it does not read like a full disk. */
        if (s.bytes_free == UINT64_MAX) {
            printf("    dir=%s   free=?\n", rec_dir());
        } else {
            char human[16];
            rec_space_format_free(human, sizeof(human), s.bytes_free);
            printf("    dir=%s   free=%s (%llu B)\n",
                   rec_dir(), human, (unsigned long long)s.bytes_free);
        }
        if (app_current_index() < 0 || strcmp(s_modes[s_mode].name, "REC") != 0) {
            printf("    note: not in REC mode - run 'mode rec' first\n");
        }
        printf("usage: rec <freq MHz|gain <dB>|thresh <n>|gap <ms>|bw <kHz>|minp <us>|"
               "maxspan <ms>|minedges <n>|arm|stop|save <name>|list|cat|rm>\n");
        return 0;
    }

    if (!strcmp(argv[1], "arm")) {
        /**/ /**/
        if (!rec_active()) select_mode(3);
        rec_arm_request();
        printf("armed at %.4f MHz - transmit now\n", rec_get_freq() / 1e6);
    } else if (!strcmp(argv[1], "stop")) {
        rec_disarm();
        printf("disarmed\n");
    } else if (!strcmp(argv[1], "freq") && argc >= 3) {
        uint32_t hz = (uint32_t)(atof(argv[2]) * 1e6 + 0.5);
        rec_set_freq(hz);
        printf("rec freq=%.4f MHz\n", rec_get_freq() / 1e6);
    } else if (!strcmp(argv[1], "gap") && argc >= 3) {
        rec_set_gap_ms(atoi(argv[2]));
        printf("rec gap=%d ms (silence that ends one transmission)\n", rec_get_gap_ms());
    /**/
    } else if (!strcmp(argv[1], "bw") && argc >= 3) {
        rec_set_bw((uint32_t)(atof(argv[2]) * 1000.0 + 0.5));
        if (rec_get_bw()) printf("rec bw=%.0f kHz\n", rec_get_bw() / 1000.0);
        else              printf("rec bw=auto\n");
    } else if (!strcmp(argv[1], "minp") && argc >= 3) {
        rec_set_min_pulse((uint32_t)atoi(argv[2]));
        printf("rec minp=%lu us (shorter transitions merge into the previous edge)\n",
               (unsigned long)rec_get_min_pulse());
    } else if (!strcmp(argv[1], "maxspan") && argc >= 3) {
        rec_set_max_span((uint32_t)atoi(argv[2]) * 1000u);
        printf("rec maxspan=%lu ms\n", (unsigned long)(rec_get_max_span() / 1000u));
    } else if (!strcmp(argv[1], "minedges") && argc >= 3) {
        rec_set_min_edges(atoi(argv[2]));
        printf("rec minedges=%d (fewer is treated as a blip and discarded)\n",
               rec_get_min_edges());
    } else if (!strcmp(argv[1], "thresh") && argc >= 3) {
        rec_set_thresh(atoi(argv[2]));
        printf("rec thresh=%d (%s) - watch 'mag now' with no signal, set above that\n",
               rec_get_thresh(), rec_get_thresh() > 0 ? "fixed" : "auto");
    } else if (!strcmp(argv[1], "gain") && argc >= 3) {
        rec_set_gain((int)(atof(argv[2]) * 10 + 0.5));
        printf("rec gain set\n");
    } else if (!strcmp(argv[1], "save")) {
        char path[64];
        int n = rec_save(argc >= 3 ? argv[2] : "capture", path, sizeof(path));
        if (n > 0) printf("wrote %s (%d edges)\n", path, n);
        else if (n == -1) printf("nothing captured yet\n");
        /**/
        else if (n == -3) printf("capture still running - wait for it to end\n");
        /**/
        else if (n == -4) {
            rec_status_t sst;
            rec_get_status(&sst);
            char detail[80];
            rec_space_format_shortage(detail, sizeof(detail),
                rec_space_estimate_bytes(sst.edges), sst.bytes_free);
            printf("save refused: %s\n", detail);
        }
        else printf("write failed (%d)\n", n);
    } else if (!strcmp(argv[1], "list")) {
        char buf[512];
        /**/
        /* Report the true total and flag byte-buffer truncation so the
           console cannot claim a shorter list is complete when longer
           names filled the buffer before every capture was named. */
        bool trunc = false;
        int n = rec_list(buf, sizeof(buf), &trunc);
        printf("%d capture(s)%s%s%s\n", n, n ? ": " : "", buf,
               trunc ? " (list truncated)" : "");
    } else if (!strcmp(argv[1], "cat") && argc >= 3) {
        int n = rec_dump(argv[2], rec_emit_console, NULL);
        if (n < 0) printf("no such capture '%s'\n", argv[2]);
    } else if (!strcmp(argv[1], "rm") && argc >= 3) {
        printf("%s\n", rec_remove(argv[2]) == 0 ? "removed" : "not found");
    } else {
        printf("usage: rec <freq MHz|gain <dB>|thresh <n>|gap <ms>|bw <kHz>|minp <us>|"
               "maxspan <ms>|minedges <n>|arm|stop|save <name>|list|cat|rm>\n");
    }
    return 0;
}

static int cmd_beep(int argc, char **argv)
{
    (void)argc; (void)argv;
    pa_on();
    audio_out_ensure_unmuted();
    snd_boot();
    /* Say where the amplifier's power actually comes from. */

#if defined(LS_BOARD_PA_EN_GPIO) && (LS_BOARD_PA_EN_GPIO >= 0)
    printf("boot chime played (vol=%d mute=%d pa=GPIO%d ring_avail=%lu)\n",
           audio_volume_get(), audio_is_muted(), PA_CTRL_GPIO,
           (unsigned long)audio_out_ring_avail());
#elif defined(LS_BOARD_XL_AUDIO_PWR_EN)
    printf("boot chime played (vol=%d mute=%d pa=XL9535 IO6 ring_avail=%lu)\n",
           audio_volume_get(), audio_is_muted(),
           (unsigned long)audio_out_ring_avail());
#else
    printf("boot chime played (vol=%d mute=%d no PA control ring_avail=%lu)\n",
           audio_volume_get(), audio_is_muted(),
           (unsigned long)audio_out_ring_avail());
#endif

    printf("beep: %lu block(s) dropped since boot - any at all is a sound "
           "written faster than it plays\n",
           (unsigned long)audio_drops_get());
    /* And how close the sound worker has come to the end of its
       stack. That number would have named the boot crash in one line; it
       took a capture on the UART instead. */
    printf("beep: sound worker stack %u of %u bytes never used\n",
           snd_test_stack_unused(), (unsigned)SND_WORKER_STACK_BYTES);
    return 0;
}

/**/
#define C6_EN_GPIO ((gpio_num_t)LS_BOARD_C6_EN_GPIO)

/* Both of these built a pin mask with (1ULL << -1) on a board whose
   C6 enable is not a GPIO - the same undefined shift the PA path had.  The
   board now answers where its enable lives; this just asks. */
static int c6_read_en(void)
{
    return ls_board_hw_c6_state();
}

static void c6_drive(int level)
{
    ls_board_hw_c6_enable(level != 0);
}

static void c6_release(void)
{
    esp_err_t e = ls_board_hw_c6_release();
    if (e != ESP_OK) ESP_LOGE(TAG, "C6 EN release failed: %s", esp_err_to_name(e));
}

/**/
static void c6_print_versions(void)
{
    printf("c6 host esp_hosted : %d.%d.%d\n",
           ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1,
           ESP_HOSTED_VERSION_PATCH_1);

    esp_hosted_coprocessor_fwver_t v = { 0 };
    int e = esp_hosted_get_coprocessor_fwversion(&v);
    if (e != 0) {
        printf("c6 slave firmware  : unreachable (rc=%d) - is the C6 up? try 'c6 up'\n", e);
        return;
    }
    printf("c6 slave firmware  : %lu.%lu.%lu\n",
           (unsigned long)v.major1, (unsigned long)v.minor1,
           (unsigned long)v.patch1);

    uint32_t chip_id = 0;
    char     target[24] = { 0 };
    if (esp_hosted_get_cp_info(&chip_id, target, sizeof(target)) == 0) {
        printf("c6 co-processor    : %s (chip id 0x%lx)\n",
               target[0] ? target : "?", (unsigned long)chip_id);
    }

    if ((uint32_t)ESP_HOSTED_VERSION_MAJOR_1 != v.major1 ||
        (uint32_t)ESP_HOSTED_VERSION_MINOR_1 != v.minor1) {
        printf("c6 MISMATCH on major.minor - this is the documented cause of RPC and "
               "HCI timeouts. Reflash the C6 with the matching esp_hosted slave "
               "build.\n");
    } else if ((uint32_t)ESP_HOSTED_VERSION_PATCH_1 != v.patch1) {
        printf("c6 patch levels differ (host %d, slave %lu). esp_hosted only enforces "
               "major.minor, but 2.12.9 against 2.12.3 passed that and the transport "
               "still framed packets differently - reflash the C6.\n",
               ESP_HOSTED_VERSION_PATCH_1, (unsigned long)v.patch1);
    } else {
        printf("c6 host and co-processor firmware are in step.\n");
    }
}

static int cmd_c6(int argc, char **argv)
{
    if (argc < 2) {
        /* Say where the enable actually is. */

#if defined(LS_BOARD_XL_C6_EN)
        printf("c6 en=%d (XL9535 IO14)  1=C6 running, 0=in reset\n",
               c6_read_en());
#elif defined(LS_BOARD_C6_EN_GPIO) && (LS_BOARD_C6_EN_GPIO >= 0)
        printf("c6 en=%d (GPIO%d)  1=C6 running, 0=in reset\n",
               c6_read_en(), (int)C6_EN_GPIO);
#else
        printf("c6 en=%d (this board declares no enable line)\n", c6_read_en());
#endif
        c6_print_versions();
        printf("usage: c6 <0|1|up [force]|ver|reset|release>\n"
               "  0/1      drive EN low/high and hold it\n"
               "  up       connect ESP-Hosted (matching slave firmware required)\n"
               "  ver      host vs co-processor firmware versions\n"
               "  reset    pulse EN low 200ms then high, rebooting the C6\n"
               "  release  stop driving; let the board pull-up hold it\n");
        return 0;
    }

    if (!strcmp(argv[1], "ver")) {
        c6_print_versions();
    } else if (!strcmp(argv[1], "up")) {
#if !CONFIG_LS_C6_LINK
        if (argc < 3 || strcmp(argv[2], "force")) {
            printf("c6: disabled in this build; install matching ESP-Hosted "
                   "slave firmware before enabling the link.\n");
            return 0;
        }
#endif
        int e = esp_hosted_connect_to_slave();
        printf("c6: esp_hosted_connect_to_slave -> %d\n", e);
        if (e) printf("c6: check slave firmware and SDIO slot 1 wiring.\n");
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
        char name[32], addr[20], filt[24], pin[24];
        uint32_t rx, tx, drops;
        ble_link_peer(name, sizeof(name), addr, sizeof(addr));
        ble_link_get_name_filter(filt, sizeof(filt));
        ble_link_stats(&rx, &tx, &drops);
        uint32_t heap_us, snapshot_us, write_us;
        ble_link_perf_stats(&heap_us, &snapshot_us, &write_us);
        printf("ble timing max: heap=%lu snapshot=%lu write=%lu us\n",
               (unsigned long)heap_us, (unsigned long)snapshot_us,
               (unsigned long)write_us);
        /* Show the pinned peer and whether this board currently holds
           a link, so `ble` alone answers "which board am I talking to". */
        bool pinned = ble_link_get_pinned(pin, sizeof(pin));
        printf("ble=%s filter=\"%s\" peer=%s [%s] pin=%s conn=%d tel=%dHz  "
               "rx_lines=%lu tx_frames=%lu drops=%lu\n",
               ble_link_state_name(), filt,
               name[0] ? name : "-", addr[0] ? addr : "-",
               pinned ? pin : "-", ble_link_is_connected() ? 1 : 0,
               ble_link_tel_hz(),
               (unsigned long)rx, (unsigned long)tx, (unsigned long)drops);
#if CONFIG_LS_BLE_HEAD
        printf("ble discovery: pressure_drops=%lu\n",
               (unsigned long)ble_hci_rx_advertisement_drops());
#endif
        /**/
        {
            uint16_t th = 0, ss = 0, se = 0;
            uint32_t foreign = 0;
            ble_link_rx_debug(&th, &ss, &se, &foreign);
            uint32_t nrx = 0, nbytes = 0;
            ble_link_notify_stats(&nrx, &nbytes);
            printf("ble rx path: notify handle=%u service=%u..%u  ignored=%lu\n",
                   th, ss, se, (unsigned long)foreign);
            printf("ble rx raw : notifications=%lu bytes=%lu -> lines=%lu%s\n",
                   (unsigned long)nrx, (unsigned long)nbytes, (unsigned long)rx,
                   (nrx && rx > nrx)
                       ? "   (more lines than notifications: the head packs "
                         "repeats into one notification)"
                   : (rx && nrx > rx)
                       ? "   (more notifications than lines: partial frames)"
                       : "");
            if (rx == 0 && tx > 50) {
                printf("*** the head has never sent us a line. It receives fine "
                       "(%lu frames out), so this is the Flipper->P4 direction: "
                       "either the app is not notifying, or it notifies on a "
                       "handle outside %u..%u (see 'ignored' above).\n",
                       (unsigned long)tx, ss, se);
            }
        }
        if (ble_link_passkey_pending()) {
            printf("*** PAIRING: the head is showing a 6-digit code - "
                   "enter it with:  ble passkey <code>\n");
        }
        /**/
        if (ble_link_stock_head_seen() && rx == 0) {
            printf("*** a Flipper is on the air advertising its own BLE "
                   "profile, not ours - open the LakeShark app on it\n");
        }
        printf("usage: ble <on|off|rescan|passkey <code>|name <substr>|tel <hz>|"
               "verbose <0|1>|pin [addr]|unpin|forget|show>\n");
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
    } else if (!strcmp(argv[1], "passkey") && argc >= 3) {

        esp_err_t e = ble_link_submit_passkey((uint32_t)strtoul(argv[2], NULL, 10));
        if (e == ESP_ERR_INVALID_STATE) {
            printf("ble: nothing is waiting for a passkey right now\n");
        } else if (e == ESP_ERR_INVALID_ARG) {
            printf("ble: the passkey is 6 digits (0-999999)\n");
        } else {
            printf("ble passkey: %s\n", esp_err_to_name(e));
        }
    /**/
    } else if (!strcmp(argv[1], "pin")) {
        const char *addr = (argc >= 3) ? argv[2] : NULL;
        esp_err_t e = ble_link_pin_peer(addr);
        if (e == ESP_ERR_INVALID_ARG) {
            printf("ble: pin needs an address like aa:bb:cc:dd:ee:ff\n");
        } else if (e == ESP_ERR_NOT_FOUND) {
            printf("ble: nothing connected - pin an explicit address, or wait "
                   "for a link\n");
        } else {
            char cur[24];
            ble_link_get_pinned(cur, sizeof(cur));
            printf("ble pin=%s (persists across reboot; other advertisers with "
                   "our service are ignored)\n", cur);
        }
    } else if (!strcmp(argv[1], "unpin")) {
        ble_link_unpin_peer();
        printf("ble unpinned (scanner falls back to any matching device)\n");
    /* Manual escape hatch for a bond left in NVS by older firmware that
       still asked for bonding + Secure Connections against a GapPairingNone
       head.  New firmware never initiates pairing, so the auto-wipe on AUTHREQ
       never fires. */
    } else if (!strcmp(argv[1], "forget")) {
        esp_err_t e = ble_link_forget_bonds();
        printf("ble forget: %s\n", esp_err_to_name(e));
    } else if (!strcmp(argv[1], "show")) {
        char name[32], addr[20], pin[24];
        ble_link_peer(name, sizeof(name), addr, sizeof(addr));
        bool pinned = ble_link_get_pinned(pin, sizeof(pin));
        printf("ble state=%s conn=%d peer=%s [%s] pin=%s\n",
               ble_link_state_name(), ble_link_is_connected() ? 1 : 0,
               name[0] ? name : "-", addr[0] ? addr : "-",
               pinned ? pin : "-");
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
        printf("usage: ble <on|off|rescan|passkey <code>|name <substr>|tel <hz>|"
               "verbose <0|1>|pin [addr]|unpin|forget|show>\n");
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

/* Bus visibility.  This board's parts are almost all I2C, and until
   now there was no way to ask the hardware what is actually present - the
   panel SKU, whether the expander answered, whether the codec is on the
   second bus.  A scan settles all three in a second, and it is how an
   unknown T-Display-P4 is told apart: the 4.05" TFT's touch controller ACKs
   at 0x68 and the 4.1" AMOLED's GT9895 at 0x5D. */
static void i2c_scan_bus(ls_i2c_bus_id_t id, const char *name)
{
    int sda = -1, scl = -1;
    if (!ls_i2c_pins(id, &sda, &scl)) {
        printf("  %-9s not present on this board\n", name);
        return;
    }

    printf("  %-9s SDA%d/SCL%d:", name, sda, scl);
    int found = 0, ghosts = 0;
    for (uint8_t addr = 0x08; addr < 0x78; ++addr) {

        int hits = 0;
        for (int t = 0; t < 3; t++)
            if (ls_i2c_probe(id, addr, 20) == ESP_OK) hits++;
        if (hits == 3) { printf(" %02x", addr); ++found; }
        else if (hits == 2) { printf(" %02x?", addr); ++ghosts; }
    }
    if (!found && !ghosts) printf(" (nothing answered)");
    printf("\n");
    if (ghosts)
        printf("  %-9s %d address(es) marked ? answered twice of three - a "
               "device on a marginal bus, or noise\n", "", ghosts);
}

static int cmd_i2c_read(int argc, char **argv)
{
    if (argc < 4) {
        printf("usage: i2c read <primary|secondary> <addr> <reg> [count]\n");
        return 1;
    }
    ls_i2c_bus_id_t id;
    if      (!strcmp(argv[1], "primary"))   id = LS_I2C_PRIMARY;
    else if (!strcmp(argv[1], "secondary")) id = LS_I2C_SECONDARY;
    else { printf("i2c: no bus '%s'\n", argv[1]); return 1; }

    const long addr = strtol(argv[2], NULL, 0);
    const long reg  = strtol(argv[3], NULL, 0);
    long n = (argc > 4) ? strtol(argv[4], NULL, 0) : 1;
    if (addr < 0x08 || addr > 0x77) { printf("i2c: address out of range\n"); return 1; }
    if (reg < 0 || reg > 0xFF)      { printf("i2c: register out of range\n"); return 1; }
    if (n < 1) n = 1;
    if (n > 16) n = 16;

    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = ls_i2c_device(id, (uint8_t)addr, 100000, &dev);
    if (err != ESP_OK || !dev) {
        printf("i2c: cannot open 0x%02lx: %s\n", addr, esp_err_to_name(err));
        return 1;
    }

    uint8_t r = (uint8_t)reg;
    uint8_t buf[16] = {0};
    err = i2c_master_transmit_receive(dev, &r, 1, buf, (size_t)n, 200);
    if (err != ESP_OK) {
        printf("i2c: 0x%02lx reg 0x%02lx did not answer: %s\n",
               addr, reg, esp_err_to_name(err));
        i2c_master_bus_rm_device(dev);
        return 1;
    }
    printf("i2c: 0x%02lx reg 0x%02lx =", addr, reg);
    for (long i = 0; i < n; i++) printf(" %02x", buf[i]);
    printf("\n");
    i2c_master_bus_rm_device(dev);
    return 0;
}

static int cmd_i2c(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "read"))
        return cmd_i2c_read(argc - 1, argv + 1);
    if(argc==2 && !strcmp(argv[1],"pins")) {
        uint64_t mask=0;
        for(int id=0;id<LS_I2C_BUS_COUNT;id++) {
            int sda,scl;
            if(ls_i2c_pins((ls_i2c_bus_id_t)id,&sda,&scl))mask|=(1ULL<<sda)|(1ULL<<scl);
        }
        return gpio_dump_io_configuration(stdout,mask)==ESP_OK?0:1;
    }
    if (argc > 1) {
        if (argc != 2 || strcmp(argv[1], "reset-touch") != 0) {
            printf("usage: i2c [reset-touch | read <bus> <addr> <reg> [n]]\n");
            return 1;
        }
        esp_err_t err = ls_board_hw_touch_reset();
        printf("touch reset: %s\n", esp_err_to_name(err));
        if (err != ESP_OK) return 1;
    }
    printf("I2C scan:\n");
    i2c_scan_bus(LS_I2C_PRIMARY, "primary");
    i2c_scan_bus(LS_I2C_SECONDARY, "secondary");
#if LS_HAS_IO_EXPANDER
    printf("  expander @ 0x%02x: %s\n", LS_BOARD_IO_EXPANDER_ADDR,
           ls_board_hw_ready() ? "up" : "NOT RESPONDING");
#else
    printf("  expander: none declared for this board\n");
#endif
    return 0;
}

static const char *imu_pose_word(ls_imu_pose_t p)
{
    switch (p) {

    case LS_IMU_UP:    return "portrait          -> rotation 0";
    case LS_IMU_DOWN:  return "portrait inverted -> rotation 2";
    case LS_IMU_LEFT:  return "landscape         -> rotation 1";
    case LS_IMU_RIGHT: return "landscape other   -> rotation 3";
    default:           return "flat - gravity says nothing about rotation";
    }
}

static int cmd_imu(int argc, char **argv)
{
    const int n = (argc > 1) ? atoi(argv[1]) : 1;

    esp_err_t err = ls_imu_start();
    if (err != ESP_OK) {
        printf("imu: not available: %s\n", esp_err_to_name(err));
        return 1;
    }

    for (int i = 0; i < (n > 0 ? n : 1); i++) {
        ls_imu_sample_t s;
        if (!ls_imu_read(&s)) { printf("imu: read failed\n"); return 1; }

        /* The magnitude of the acceleration vector is the cheapest health
           check there is: at rest it is gravity, so it should read about
           1.00 g whichever way the board is held. A number far from that is
           a scale factor wrong, not a board being moved. */
        const float g = sqrtf(s.ax * s.ax + s.ay * s.ay + s.az * s.az);

        /* The AXIS NAMES are printed with the numbers, because the
           mounting has now been got wrong twice and both times the argument
           was conducted in the dark. Screen X is to the right and screen Y
           is up the display; hold the board with one edge pointing at the
           ceiling, run this, and the axis reading about +1 g is that edge.
           That is a look, not a deduction. */
        /* The mounting angle is a board fact and only exists on a
           board that declares a sensor, so the reference has to be compiled
           out where there is none - the P4 NANO and WIFI6 builds have failed
           on this line since added it, and the whole-tree gate is the
           only thing that looks. Nothing changes for a board that has one:
           ls_imu_start() above has already refused on a board that does not,
           so this line was never reached there anyway. */
#if LS_HAS_IMU
        printf("imu: mounted %d deg from the screen\n", LS_BOARD_IMU_MOUNT_DEG);
#endif
        printf("imu: screen  right %+6.3f  up %+6.3f  out %+6.3f g  |a|=%.3f\n",
               s.ax, s.ay, s.az, g);
        printf("imu: gyro  %+7.2f %+7.2f %+7.2f dps\n", s.gx, s.gy, s.gz);
        if (s.mag_valid) {
            const float m = sqrtf(s.mx * s.mx + s.my * s.my + s.mz * s.mz);
            printf("imu: mag   %+7.1f %+7.1f %+7.1f uT  |m|=%.1f  heading %.0f\n",
                   s.mx, s.my, s.mz, m, (double)ls_imu_heading());
        } else {
            printf("imu: mag   not answering\n");
        }
        printf("imu: %.1f C  pose: %s\n", s.temp_c, imu_pose_word(ls_imu_pose()));
        if (i + 1 < n) { printf("\n"); vTaskDelay(pdMS_TO_TICKS(400)); }
    }
    return 0;
}

/* The notice hook, which until now was a weak no-op. */

void ls_notify_alert_hw(bool ring, bool vibe)
{
    if (vibe) ls_haptic_play(LS_HAPTIC_ALERT);
    if (ring) {
        pa_on();
        audio_out_ensure_unmuted();
        snd_alert_start();
    }
}

/* A notice, posted from the console, so the whole path can be tested. */

static int cmd_notify(int argc, char **argv)
{
    bool ring = ls_notify_ring();
    bool vibe = ls_notify_vibe();
    bool forced = false;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "ring")) { ring = true; forced = true; }
        else if (!strcmp(argv[i], "vibe")) { vibe = true; forced = true; }
        else if (!strcmp(argv[i], "quiet")) { ring = vibe = false; forced = true; }
        else {
            printf("usage: notify [ring|vibe|quiet]\n");
            printf("notify: posts a test notice through the real path.\n");
            printf("        no argument uses your stored settings; the words\n");
            printf("        force one half on for this notice only.\n");
            return 0;
        }
    }

    const bool stored_ring = ls_notify_ring();
    const bool stored_vibe = ls_notify_vibe();
    if (forced) ls_notify_set_alerts(ring, vibe);

    ls_notice_t n;
    memset(&n, 0, sizeof(n));
    snprintf(n.title, sizeof(n.title), "TEST");
    snprintf(n.body, sizeof(n.body), "console notice - this is the real path");
    n.hue = 6;          /* cyan, the shell's own accent */
    n.screen = -1;      /* nothing to open: this notice has no app behind it */
    ls_notify_post(&n);

    if (forced) ls_notify_set_alerts(stored_ring, stored_vibe);

    printf("notify: posted.  ring=%s vibe=%s%s\n",
           ring ? "on" : "off", vibe ? "on" : "off",
           forced ? "  (forced for this notice only)" : "");
    printf("notify: %d unread.  Expect a banner on the panel for six seconds.\n",
           ls_notify_unread());
    if (vibe && !ls_haptic_present())
        printf("notify: vibrate is on but no haptic driver came up - nothing will buzz\n");
    return 0;
}

/* The antenna switch, on the console.

   The RADIOS page has the same control; this is here because the answer to
   "which way is it pointing" is worth having without navigating, and because
   a transmit test is run from here. */
static int cmd_ant(int argc, char **argv)
{
#if !LS_HAS_RF_SWITCH
    (void)argc; (void)argv;
    printf("ant: this board has no antenna switch\n");
    return 1;
#else
    if (argc > 1) {
        bool ext;
        if      (!strcmp(argv[1], "ext") || !strcmp(argv[1], "mmcx1")) ext = true;
        else if (!strcmp(argv[1], "int") || !strcmp(argv[1], "internal")) ext = false;
        else { printf("usage: ant [int|ext]\n"); return 1; }

        const esp_err_t e = ls_board_hw_antenna_external(ext);
        if (e != ESP_OK) {
            printf("ant: switch refused: %s\n", esp_err_to_name(e));
            return 1;
        }
        settings_set_antenna_external(ext);
    }

    const bool ext = ls_board_hw_antenna_is_external();
    printf("ant: %s\n", ext ? "EXTERNAL - MMCX1" : "internal");
    printf("ant: stored, so it comes back this way after a reboot\n");
    if (ext)
        printf("ant: MMCX1 must have an antenna on it before transmitting - "
               "the vendor warns that transmitting into an open port may "
               "damage the RF front end. MMCX2 is not connected.\n");
    return 0;
#endif
}

/* The haptic driver, on the console. */

static int cmd_haptic(int argc, char **argv)
{
    esp_err_t err = ls_haptic_start();
    if (err != ESP_OK) {
        printf("haptic: not available: %s\n", esp_err_to_name(err));
        return 1;
    }

    if (argc > 1 && strcmp(argv[1], "diag") == 0) {
        ls_haptic_diag_t d;
        if (!ls_haptic_diag(&d)) { printf("haptic: diagnostic failed\n"); return 1; }
        /* Same as the IMU line above, and broken since for the
           same reason: the address and the resonance are declared only by a
           board that has the part, and ls_haptic_start() has already refused
           on one that does not. */
#if LS_HAS_HAPTIC
        printf("haptic: AW86224 at 0x%02X, driving %d Hz from the board header\n",
               LS_BOARD_HAPTIC_I2C_ADDR, LS_BOARD_HAPTIC_F0_HZ);
#endif
        printf("haptic: coil %.2f ohms   supply %.2f V\n",
               (double)d.lra_ohms, (double)d.vdd_v);
        if (d.lra_ohms < 1.0f)
            printf("haptic: that is too low for an LRA - open circuit, or no motor fitted\n");
        else if (d.lra_ohms > 60.0f)
            printf("haptic: that is high for an LRA - check the measurement gain\n");
        if (d.f0_hz > 0.0f) {
            printf("haptic: tracked resonance %.1f Hz during the last buzz\n",
                   (double)d.f0_hz);
            /* And the header it is being compared against, which
               only a board that declares a motor has. */
#if LS_HAS_HAPTIC
            printf("haptic: if that disagrees with %d Hz, the header is what is wrong\n",
                   LS_BOARD_HAPTIC_F0_HZ);
#endif
        } else {
            printf("haptic: nothing tracked yet - run 'haptic buzz' first\n");
        }
        if (d.over_current || d.over_temp || d.under_voltage)
            printf("haptic: FAULT%s%s%s\n",
                   d.over_current  ? " over-current" : "",
                   d.over_temp     ? " over-temperature" : "",
                   d.under_voltage ? " under-voltage" : "");
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "stop") == 0) { ls_haptic_stop(); return 0; }

    if (argc > 1 && strcmp(argv[1], "buzz") == 0) {
        const int ms  = (argc > 2) ? atoi(argv[2]) : 150;
        const int pct = (argc > 3) ? atoi(argv[3]) : 100;
        if (!ls_haptic_buzz(ms, pct)) { printf("haptic: refused\n"); return 1; }
        printf("haptic: %d ms at %d%%\n", ls_haptic_clamp_ms(ms), pct);
        return 0;
    }

    static const char *const names[LS_HAPTIC_EFFECT_COUNT] = {
        "tick", "click", "confirm", "alert", "warn"
    };
    if (argc > 1) {
        for (int i = 0; i < LS_HAPTIC_EFFECT_COUNT; i++) {
            if (strcmp(argv[1], names[i]) != 0) continue;
            if (!ls_haptic_play((ls_haptic_effect_t)i)) {
                printf("haptic: refused - already busy\n");
                return 1;
            }
            printf("haptic: %s\n", names[i]);
            return 0;
        }
        printf("haptic: no such effect\n");
    }

    printf("haptic: effects -");
    for (int i = 0; i < LS_HAPTIC_EFFECT_COUNT; i++) printf(" %s", names[i]);
    printf("\nhaptic: also 'buzz <ms> [percent]', 'stop', 'diag'\n");
    return 0;
}

/* The microphone, on the console. */

static int cmd_mic(int argc, char **argv)
{
    if (!ls_audio_hw_has_mic()) {
        printf("mic: no capture path on this board\n");
        return 1;
    }
    const int ms = (argc > 1) ? atoi(argv[1]) : 500;
    if (argc > 2) {
        const float g = strtof(argv[2], NULL);
        esp_err_t e = ls_audio_hw_in_gain(g);
        printf("mic: gain %.0f dB: %s\n", g, esp_err_to_name(e));
    }

    /* 16 kHz stereo 16-bit is what ls_audio_hw_init opens with. */
    const int frames = (16000 * (ms > 0 ? ms : 500)) / 1000;
    const size_t bytes = (size_t)frames * 2 * sizeof(int16_t);
    int16_t *buf = (int16_t *)malloc(bytes);
    if (!buf) { printf("mic: out of memory for %u bytes\n", (unsigned)bytes); return 1; }

    /* Throw the first window away. */

    size_t flushed = 0;
    ls_audio_hw_read(buf, bytes, &flushed);

    size_t got = 0;
    esp_err_t err = ls_audio_hw_read(buf, bytes, &got);
    if (err != ESP_OK) {
        printf("mic: read failed: %s\n", esp_err_to_name(err));
        free(buf);
        return 1;
    }

    const size_t n = got / sizeof(int16_t);
    int32_t peak = 0;
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        const int32_t v = buf[i];
        const int32_t a = v < 0 ? -v : v;
        if (a > peak) peak = a;
        sum += (double)v * (double)v;
    }
    const double rms = n ? sqrt(sum / (double)n) : 0.0;
    free(buf);

    printf("mic: %u ms, %u samples  peak %ld  rms %.0f  (full scale 32767)\n",
           (unsigned)ms, (unsigned)n, (long)peak, rms);
    if (peak == 0)
        printf("mic: silent - the ADC is clocked and returning nothing\n");
    return 0;
}

static int cmd_track(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "on")) {
        const esp_err_t e = ls_track_rec_start();
        if (e == ESP_ERR_NOT_FOUND) {
            printf("track: no card to record onto\n");
            return 1;
        }
        if (e != ESP_OK) { printf("track: %s\n", esp_err_to_name(e)); return 1; }
        float mv; uint32_t gap;
        ls_track_get_rule(&mv, &gap);
        printf("track: recording - a point every %.0f m of movement or %lu s\n",
               (double)mv, (unsigned long)gap);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "off")) {
        ls_track_rec_stop();
        printf("track: stopped, %d points held\n", ls_track_points());
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "clear")) {
        if (!ls_track_clear()) {
            printf("track: refused - stop recording first, or there is no "
                   "card\n");
            return 1;
        }
        printf("track: cleared\n");
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "export")) {
        char where[64] = { 0 };
        const int n = ls_track_export(argc >= 3 ? argv[2] : NULL,
                                      where, sizeof(where));
        /* The two failures send somebody to two different places. */
        if (n == LS_TRACK_EXPORT_NO_LOG) {
            printf("track: there is no track log on the card - nothing has "
                   "been recorded on this unit\n");
            return 1;
        }
        if (n < 0)  { printf("track: could not write the file - check the "
                             "card has room\n"); return 1; }
        if (n == 0) { printf("track: the log is there and empty - nothing "
                             "recorded yet\n"); return 0; }
        printf("track: %d points to %s\n", n, where);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "nodes")) {

        printf("track: %d node sighting(s) held - each is a node heard at a "
               "place where we had a fix\n", ls_mesh_sightings());
        if (argc >= 3 && !strcmp(argv[2], "clear"))
            printf("track: %s\n",
                   ls_mesh_sight_clear() ? "cleared" : "no card");
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "rule")) {
        ls_track_set_rule((float)atof(argv[2]), (uint32_t)atoi(argv[3]));
    }

    float mv; uint32_t gap;
    ls_track_get_rule(&mv, &gap);
    printf("track: %s, %d points, a point every %.0f m or %lu s\n",
           ls_track_rec_running() ? "RECORDING" : "stopped",
           ls_track_points(), (double)mv, (unsigned long)gap);
    /* The two things that stop a track being a track, said before
       they are asked about. A recorder with no sky writes nothing and looks
       identical to one that is broken. */
    if (!ls_gps_running())
        printf("track: the receiver is not running - 'track on' starts it\n");
    else {
        ls_gps_state_t g;
        ls_gps_get(&g);
        if (!g.fix)
            printf("track: no fix yet (%u sats seen) - nothing is being "
                   "recorded until there is one\n", (unsigned)g.sats_visible);
    }
    if (ls_mesh_sightings())
        printf("track: %d node sighting(s) export as waypoints alongside it\n",
               ls_mesh_sightings());
    printf("track: 'track on|off|clear|export [path]|rule <m> <s>|nodes [clear]'\n");
    return 0;
}

/* WHICH TASK HAS THE CPU. */

typedef struct {
    TaskHandle_t h;
    uint32_t     rt;
} top_seen_t;

typedef struct {
    char     name[configMAX_TASK_NAME_LEN];
    int      core;
    unsigned prio;
    uint32_t used;
} top_row_t;

static int top_row_cmp(const void *a, const void *b)
{
    const uint32_t ua = ((const top_row_t *)a)->used;
    const uint32_t ub = ((const top_row_t *)b)->used;
    return ua < ub ? 1 : (ua > ub ? -1 : 0);
}

#define TOP_IRQ_WINDOW_US 100000u
#define TOP_PSRAM_BYTES   (1024u * 1024u)

typedef struct {
    TaskHandle_t waiter;
    uint32_t    *psram;
    uint32_t     stolen_us, gaps, worst_us;
    uint32_t     wr_us, rd_us;
} top_probe_t;

static void top_probe_task(void *arg)
{
    top_probe_t *p = (top_probe_t *)arg;
    const uint32_t mhz = esp_rom_get_cpu_ticks_per_us();
    const uint32_t window = TOP_IRQ_WINDOW_US * mhz;
    uint32_t stolen = 0, gaps = 0, worst = 0;
    const uint32_t start = (uint32_t)esp_cpu_get_cycle_count();
    uint32_t prev = start;
    for (;;) {
        const uint32_t now = (uint32_t)esp_cpu_get_cycle_count();
        const uint32_t d = now - prev;
        prev = now;
        if (d > mhz) {
            stolen += d;
            gaps++;
            if (d > worst) worst = d;
        }
        if (now - start >= window) break;
    }
    p->stolen_us = stolen / mhz;
    p->gaps      = gaps;
    p->worst_us  = worst / mhz;

    if (p->psram) {
        volatile uint32_t *b = p->psram;
        const size_t words = TOP_PSRAM_BYTES / sizeof(uint32_t);
        const uint32_t c0 = (uint32_t)esp_cpu_get_cycle_count();
        for (size_t i = 0; i < words; i++) b[i] = (uint32_t)i;
        const uint32_t c1 = (uint32_t)esp_cpu_get_cycle_count();
        uint32_t sum = 0;
        /* One word per 64-byte line: every line is fetched, and the loop is
           not what gets measured. */
        for (size_t i = 0; i < words; i += 16) sum += b[i];
        const uint32_t c2 = (uint32_t)esp_cpu_get_cycle_count();
        (void)sum;
        p->wr_us = (c1 - c0) / mhz;
        p->rd_us = (c2 - c1) / mhz;
    }
    xTaskNotifyGive(p->waiter);
    vTaskDelete(NULL);
}

static int top_irq(void)
{
    top_probe_t p[2];
    memset(p, 0, sizeof(p));
    uint32_t *psram = heap_caps_malloc(TOP_PSRAM_BYTES, MALLOC_CAP_SPIRAM);
    if (!psram) printf("top: no PSRAM for the bus test - interrupts only\n");

    bool ran[2] = { false, false };
    for (int core = 0; core < 2; core++) {
        p[core].waiter = xTaskGetCurrentTaskHandle();
        p[core].psram  = psram;
        (void)ulTaskNotifyTake(pdTRUE, 0);
        if (xTaskCreatePinnedToCore(top_probe_task, "top_probe", 2048, &p[core],
                                    configMAX_PRIORITIES - 1, NULL,
                                    core) != pdPASS) {
            printf("top: could not start the probe on core %d\n", core);
            continue;
        }
        if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000))) {
            /* Still running, and still writing to the buffer: leaking it is
               the only safe thing to do with it. */
            printf("top: the probe on core %d did not finish\n", core);
            return 0;
        }
        ran[core] = true;
    }
    free(psram);

    printf("top irq: %lu ms on each core at the highest priority\n",
           (unsigned long)(TOP_IRQ_WINDOW_US / 1000u));
    for (int core = 0; core < 2; core++) {
        if (!ran[core]) continue;
        const uint32_t pm = p[core].stolen_us * 1000u / TOP_IRQ_WINDOW_US;
        printf("  core %d  interrupts took %lu.%lu%%  (%lu of them, longest %lu us)\n",
               core, (unsigned long)(pm / 10u), (unsigned long)(pm % 10u),
               (unsigned long)p[core].gaps, (unsigned long)p[core].worst_us);
        if (psram && p[core].wr_us && p[core].rd_us)
            printf("          psram   write %lu MB/s  read %lu MB/s\n",
                   (unsigned long)(TOP_PSRAM_BYTES / p[core].wr_us),
                   (unsigned long)(TOP_PSRAM_BYTES / p[core].rd_us));
    }
    return 0;
}

static int cmd_top(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "irq")) return top_irq();
    int ms = (argc >= 2) ? atoi(argv[1]) : 1000;
    if (ms < 200)  ms = 200;
    if (ms > 5000) ms = 5000;

    /* The snapshot is written with the scheduler suspended, so it lives in
       internal RAM - see ls_cpu_busy.c. Only one is held: the first is boiled
       down to handle and counter before the second is taken, which halves
       what this borrows from the scarcest pool. */
    const UBaseType_t cap = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *ts   = heap_caps_malloc(cap * sizeof(*ts), MALLOC_CAP_INTERNAL);
    top_seen_t   *seen = heap_caps_malloc(cap * sizeof(*seen), MALLOC_CAP_SPIRAM);
    top_row_t    *rows = heap_caps_malloc(cap * sizeof(*rows), MALLOC_CAP_SPIRAM);
    if (!ts || !seen || !rows) {
        printf("top: no memory for a snapshot (internal free %u)\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        free(ts);
        free(seen);
        free(rows);
        return 0;
    }

    configRUN_TIME_COUNTER_TYPE t0 = 0, t1 = 0;
    const UBaseType_t n0 = uxTaskGetSystemState(ts, cap, &t0);
    for (UBaseType_t i = 0; i < n0; i++) {
        seen[i].h  = ts[i].xHandle;
        seen[i].rt = (uint32_t)ts[i].ulRunTimeCounter;
    }
    vTaskDelay(pdMS_TO_TICKS(ms));
    const UBaseType_t n1 = uxTaskGetSystemState(ts, cap, &t1);
    const uint32_t span = (uint32_t)(t1 - t0);

    int nr = 0;
    for (UBaseType_t i = 0; i < n1; i++) {
        /* A task born inside the window has no first reading, and all of
           its counter was spent inside the window. */
        uint32_t before = 0;
        for (UBaseType_t j = 0; j < n0; j++) {
            if (seen[j].h == ts[i].xHandle) { before = seen[j].rt; break; }
        }
        top_row_t *r = &rows[nr++];
        r->used = (uint32_t)ts[i].ulRunTimeCounter - before;
        r->core = (int)ts[i].xCoreID;
        r->prio = (unsigned)ts[i].uxCurrentPriority;
        strncpy(r->name, ts[i].pcTaskName ? ts[i].pcTaskName : "?",
                sizeof(r->name) - 1);
        r->name[sizeof(r->name) - 1] = '\0';
    }
    free(ts);

    if (!span) {
        printf("top: the run-time clock did not move\n");
        free(seen);
        free(rows);
        return 0;
    }
    qsort(rows, (size_t)nr, sizeof(*rows), top_row_cmp);

    printf("top: %d ms, each task's share of one core (under 0.1%% not shown)\n", ms);
    printf("  %-16s %-4s %4s %7s\n", "task", "core", "prio", "cpu");
    for (int i = 0; i < nr; i++) {
        const uint32_t pm = (uint32_t)((uint64_t)rows[i].used * 1000u / span);
        if (!pm) continue;
        char core[4];
        if (rows[i].core == 0 || rows[i].core == 1)
            snprintf(core, sizeof(core), "%d", rows[i].core);
        else
            snprintf(core, sizeof(core), "any");
        printf("  %-16s %-4s %4u %5lu.%lu%%\n", rows[i].name, core, rows[i].prio,
               (unsigned long)(pm / 10u), (unsigned long)(pm % 10u));
    }
    free(seen);
    free(rows);
    return 0;
}

/* Bounded allocator summaries only: never enumerate tasks or scan stacks.
 * Run between load measurements; allocator inspection itself is not free. */
static int cmd_memory(int argc, char **argv)
{
    (void)argc; (void)argv;
    multi_heap_info_t internal, dma, psram;
    heap_caps_get_info(&internal, MALLOC_CAP_INTERNAL);
    heap_caps_get_info(&dma, MALLOC_CAP_DMA);
    heap_caps_get_info(&psram, MALLOC_CAP_SPIRAM);
    printf("memory: internal_free=%u internal_largest=%u internal_min=%u "
           "dma_free=%u dma_largest=%u dma_min=%u psram_free=%u\n",
           (unsigned)internal.total_free_bytes, (unsigned)internal.largest_free_block,
           (unsigned)internal.minimum_free_bytes, (unsigned)dma.total_free_bytes,
           (unsigned)dma.largest_free_block, (unsigned)dma.minimum_free_bytes,
           (unsigned)psram.total_free_bytes);
    return 0;
}

static int cmd_heap(int argc, char **argv)
{
    /* 'heap dma' dumps the DMA-capable regions one at a time. */

    /* 'heap stages' says WHERE the memory went during boot. */

    if (argc >= 2 && !strcmp(argv[1], "stages")) {
        const int n = ls_vitals_mark_count();
        if (n <= 0) {
            printf("heap: no boot stages recorded\n");
            return 0;
        }
        /* EACH STAGE'S COST ON ITS OWN ROW. */

        printf("heap: what each boot stage cost, in bytes\n");
        printf("  %-14s %12s %10s %10s\n",
               "stage", "free after", "internal", "dma");
        for (int i = 0; i < n; i++) {
            ls_vitals_mark_t m, nx;
            if (!ls_vitals_mark_at(i, &m)) continue;
            if (!ls_vitals_mark_at(i + 1, &nx)) {
                printf("  %-14s %12lu %10s %10s   <- still running\n",
                       m.name, (unsigned long)m.internal, "-", "-");
                continue;
            }
            printf("  %-14s %12lu %10ld %10ld\n", m.name,
                   (unsigned long)nx.internal,
                   (long)m.internal - (long)nx.internal,
                   (long)m.dma - (long)nx.dma);
        }
        return 0;
    }

    if (argc >= 2 && !strcmp(argv[1], "dma")) {
        printf("heap: DMA-capable regions, as the allocator sees them\n");
        heap_caps_print_heap_info(MALLOC_CAP_DMA);
        printf("\nheap: and internal, for comparison - a region that is in\n"
               "      both lists is DMA-capable, one only here is not\n");
        heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
        return 0;
    }

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

    int core0_pct, core1_pct;
    if (ls_cpu_busy(&core0_pct, &core1_pct)) {
        printf("cpu busy : core0=%d%% core1=%d%%\n", core0_pct, core1_pct);
    } else {
        printf("cpu busy : core0=-- core1=-- (first sample; run heap again)\n");
    }

    printf("usb iq   : %d transfer slots in flight, %llu B dropped, ring avail=%u\n",
           rtlsdr_stream_slots(),
           (unsigned long long)rtlsdr_stream_dropped(),
           (unsigned)rtlsdr_stream_avail());

    uint32_t done = 0, dropped = 0, commits = 0;
    settings_write_stats(&done, &dropped, &commits);
    printf("nvs      : %lu writes in %lu commits, %lu dropped\n",
           (unsigned long)done, (unsigned long)commits, (unsigned long)dropped);

    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *ts = calloc(n, sizeof(TaskStatus_t));
    if (ts) {
        n = uxTaskGetSystemState(ts, n, NULL);
        printf("tasks    : %-16s %-8s %-10s %s\n", "name", "stack", "base", "free-hi-water");
        for (UBaseType_t i = 0; i < n; i++) {
            bool ext = esp_ptr_external_ram(ts[i].pxStackBase);
            printf("           %-16s %-8s %p %u%s\n",
                   ts[i].pcTaskName, ext ? "PSRAM" :
                   (esp_ptr_in_dram(ts[i].pxStackBase) ? "DRAM" : "internal"),
                   (void *)ts[i].pxStackBase,
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

/**/
/* full=false registers only the recovery set. Everything below
   dereferences the radio backend, the audio path or the BLE link, none of
   which safe mode started. */
static bool console_start(bool full)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "lakeshark>";
    repl_cfg.max_cmdline_length = 128;

    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_err_t cerr = esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl);
    if (cerr != ESP_OK) {
        /* IDF 5.4.3's error path deletes the UART driver after pointing the
           console VFS at it, and leaves the VFS pointing there. Every write
           to stdout then fails inside uart_write_bytes, which logs "uart
           driver error" - to stdout. That recursion overflowed the main task
           on every boot with Wi-Fi joined, where the REPL task could not get
           its 4 KB stack (3.8 KB internal left), and the log line below was
           the first to fall into it. Put the VFS back on the plain FIFO
           before anything prints. */
        if (!uart_is_driver_installed(uart_cfg.channel))
            uart_vfs_dev_use_nonblocking(uart_cfg.channel);
        ESP_LOGE(TAG, "console REPL unavailable: %s (internal=%u largest=%u). "
                      "Radio and links keep running; the head still works.",
                 esp_err_to_name(cerr),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return false;
    }

    /* This table used to consume roughly a kilobyte of the already-small
       IDF main-task stack every time the console was installed.  Keep the
       immutable descriptors in flash instead; normal boot subsequently has
       to initialize the complete compact UI on this same task. */
    static const esp_console_cmd_t cmds[] = {
        { .command="cellperf", .help="HackRF focused session; on/off restarts, ordinary reset returns to normal", .func=cell_performance_command },
        { .command = "status", .help = "Show mode, freq, volume, gain, mute, heap",
          .func = &cmd_status },
        { .command = "p2", .help = "Experimental Phase II manual voice: on|off|status|config WACN SYS NAC slot", .func = &cmd_p2 },
        /**/
        { .command = "rtl", .help = "RTL health; 'rtl reset' resets the dongle, 'rtl detach' simulates an unplug",
          .func = &cmd_rtl },
        { .command = "mode",   .help = "Switch mode", .hint = "p25|adsb|fm|rec|next",
          .func = &cmd_mode },
        { .command = "fm",     .help = "FM sub-mode (hops into FM)",
          .hint = "listen|scan|pocsag|wfm|am|acars|flex", .func = &cmd_fm },
        { .command = "vol",    .help = "Volume 0-100 (or +n / -n)", .hint = "<n|+n|-n>",
          .func = &cmd_vol },
        { .command = "freq",   .help = "Tune the current mode", .hint = "<MHz>",
          .func = &cmd_freq },
        { .command = "gain",   .help = "RF gain in dB, or 'auto'", .hint = "<dB|auto>",
          .func = &cmd_gain },
        { .command = "feed",   .help = "ADS-B JSON feed to console (CartoTUI)",
          .hint = "on|off", .func = &cmd_feed },
        { .command = "mute",   .help = "Toggle audio mute", .func = &cmd_mute },
        { .command = "rec",    .help = "OOK recorder - captures to a Flipper SubGhz .sub file",
          .hint = "<freq MHz|gain <dB>|arm|stop|save <name>|list|cat <name>|rm <name>>",
          .func = &cmd_rec },
        { .command = "beep",   .help = "Play the boot chime - proves the speaker path",
          .func = &cmd_beep },
        { .command = "ble",    .help = "BLE control head link (P4 is central)",
          .hint = "<on|off|rescan|passkey <code>|name <s>|tel <hz>|"
                  "verbose <0|1>|pin [addr]|unpin|forget|show>",
          .func = &cmd_ble },
        { .command = "c6",     .help = "ESP32-C6 co-processor: enable line, transport, firmware versions",
          .hint = "<0|1|up|ver|reset|release>", .func = &cmd_c6 },
        { .command = "link",   .help = "Flipper serial head control",
          .hint = "[on|off|verbose <0|1>|baud <n>|tel <hz>|pins <rx> <tx>]",
          .func = &cmd_link },
        { .command = "moto",   .help = "Motorola-style radio alerts on the speaker",
          .hint = "[on|alert|bonk|all]", .func = &cmd_moto },
        { .command = "i2c",    .help = "I2C: scan the buses, or 'i2c read <bus> <addr> <reg> [n]' to identify a part",
          .func = &cmd_i2c },
        { .command = "mic",    .help = "Microphone: capture and report the level. 'mic <ms> [gain dB]'",
          .func = &cmd_mic },
        { .command = "ant",    .help = "Antenna path: internal or external through MMCX1",
          .hint = "[int|ext]", .func = &cmd_ant },
        { .command = "notify", .help = "Post a test notice through the real path: banner, count, ring and vibrate",
          .hint = "[ring|vibe|quiet]", .func = &cmd_notify },
        { .command = "haptic", .help = "Vibration: play an effect, buzz for a time, or measure the motor",
          .hint = "[tick|click|confirm|alert|warn|buzz <ms> [pct]|stop|diag]",
          .func = &cmd_haptic },
        { .command = "track",  .help = "GPS track: 'track on|off' records to the card, 'track export' writes GPX, 'track rule <m> <s>' sets how often a point is kept",
          .func = &cmd_track },
        { .command = "imu",    .help = "Nine-axis sensor: accel, gyro, magnetometer, heading and pose. 'imu <n>' repeats",
          .func = &cmd_imu },
        { .command = "memory", .help = "Allocator headroom without task/stack enumeration", .func = &cmd_memory },
        { .command = "heap",   .help = "Internal/DMA/PSRAM free, USB IQ slots, NVS write stats. 'heap dma' dumps the regions, 'heap stages' the boot profile",
          .func = &cmd_heap },
        /**/
        { .command = "top",    .help = "Which task has the CPU: each task's share of a core over a second ('top <ms>'). 'top irq' measures what interrupts take on each core and the PSRAM bus rate",
          .func = &cmd_top },
        { .command = "log",   .help = "Runtime log level per tag (hot tags are quiet by default)",
          .hint = "<tag|*> <none|error|warn|info|debug|verbose>", .func = &cmd_log },
        { .command = "tel",    .help = "Print the flipper-link telemetry frame N times (0.5s apart)",
          .hint = "[n]", .func = &cmd_tel },
        { .command = "fl",     .help = "Run one flipper-link protocol line locally",
          .hint = "<PING|FREQ|VOL|GAIN|DEMOD|...>", .func = &cmd_fl },
    };
    esp_console_register_help_command();
    if (full) {
        for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
            esp_err_t rerr = esp_console_cmd_register(&cmds[i]);
            if (rerr != ESP_OK) {
                ESP_LOGW(TAG, "console command '%s' not registered: %s",
                         cmds[i].command, esp_err_to_name(rerr));
            }
        }
        ls_ctl_register_commands();
    } else {
        (void)cmds;
        ls_ctl_register_recovery_commands();
    }
    esp_err_t serr = esp_console_start_repl(repl);
    if (serr != ESP_OK) {
        ESP_LOGE(TAG, "console REPL would not start: %s", esp_err_to_name(serr));
        return false;
    }
    return true;
}

static void console_retry_start(void)
{
    if (console_start(true))
        ESP_LOGI(TAG, "console ready after main-task memory release");
    else
        ESP_LOGE(TAG, "console retry failed; no further retries this boot");
}

/* Headless safe mode. No display to fall back on, so the report is
   the serial log and the recovery console. Nothing that can fault is started:
   no gpio_init/PA, no NVS, no SPIFFS, no codec, no C6, no BLE, no backend, no
   mode restore, no boot chime. */
static void headless_safe_main(const ls_safe_boot_plan_t *plan)
{
    ls_crash_boot_setup();
    ls_safe_note_dump(ls_crash_present() ? LS_SAFE_DUMP_PRESENT
                                         : LS_SAFE_DUMP_NONE);

    static char report[LS_SAFE_REPORT_MAX];
    ls_safe_report(report, sizeof(report));
    ESP_LOGE(TAG, "SAFE MODE report follows");
    fputs(report, stdout);
    fflush(stdout);

    if (plan->console) console_start(false);
    ESP_LOGW(TAG, "safe mode ready - radio, audio, C6 and BLE were not "
                  "started. 'safemode normal' retries a normal boot.");
}

void app_main(void)
{
    bool wifi_autojoin_ready = false;
    cell_performance_boot();
    /* Bluetooth command dispatch does not require a wired UART worker. */
    flipper_link_set_host(&s_link_host);
    /* First, before anything that can fault. */
    const ls_safe_boot_t *boot = ls_safe_boot_begin();
    ls_safe_boot_plan_t plan;
    ls_safe_boot_plan(boot->safe, &plan);
    if (boot->safe) {
        headless_safe_main(&plan);
        return;
    }

    ls_safe_stage(LS_SAFE_STAGE_NVS);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Normal-boot behaviour only; safe mode never reaches here, so
           a recovery session cannot erase the settings partition. */
        if (plan.storage_autorepair) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            err = nvs_flash_init();
        }
    }
    ESP_ERROR_CHECK(err);

    /* initialize before BLE can load its pinned peer. Storage is
       static cache-safe DRAM, so this does not depend on heap availability. */
    esp_err_t nvs_worker_err = ls_nvs_init();
    if (nvs_worker_err != ESP_OK)
        ESP_LOGE(TAG, "NVS dispatcher init failed: %s",
                 esp_err_to_name(nvs_worker_err));

    gpio_init();

    /* Before storage, the codec, USB host or the co-processor: on a
       board whose rails live behind an I2C expander, none of those exist
       until this has run.  A no-op on every board that is hard-powered. */
    ls_board_hw_early_init();
    ls_hosted_board_init();

    /* Seed the wall clock from the RTC here, right after the expander
       brings the I2C rails up and before anything can stamp a file. Without
       this the part keeps perfect time and nothing ever asks it: measured on
       hardware 2026-09-09, the RTC read back correctly across a hard reset
       while `date` still answered "up 17s - no wall clock yet". A clock
       nobody reads is not a clock. Declines quietly when the oscillator flag
       says the stored value is not a time. */
    ls_rtc_seed_system_time();

    ls_panel_test_start();

    defer_start();

    /**/
    ls_crash_boot_setup();
    /**/
    ls_safe_note_dump(ls_crash_present() ? LS_SAFE_DUMP_PRESENT
                                         : LS_SAFE_DUMP_NONE);

    ls_safe_stage(LS_SAFE_STAGE_STORAGE);
    /* Storage is optional here, matching the card below. compact_ui_start
       owns the panel handoff, so boot always continues to it. */
    {
        const esp_err_t fs = bsp_spiffs_mount();
        if (fs != ESP_OK)
            ESP_LOGW(TAG, "no SPIFFS (%s) - running without it",
                     esp_err_to_name(fs));
    }

    /* The card, from this board's own pins. */

#if LS_HAS_SDIO
    {
        const esp_err_t sd = ls_sdcard_mount();
        /* No ls_hub_set_sd here: the hub is the GUI profile's status bar and
           this profile has no hub. The TUI reads ls_sdcard_mounted directly. */
        if (sd == ESP_OK) {
            ESP_LOGI(TAG, "SD card mounted at %s", LS_SDCARD_MOUNT);

            const int pts = ls_track_attach();
            if (pts > 0)
                ESP_LOGI(TAG, "track log on the card holds %d point(s)", pts);
        } else {
            ESP_LOGW(TAG, "no SD card (%s) - running without it",
                     esp_err_to_name(sd));
        }
    }
#endif
    ls_safe_stage(LS_SAFE_STAGE_CODEC);

#if LS_HAS_AUDIO
    /* false, so the capture channel is built too.

       The argument has been ignored since it was added, and the codec came
       up output-only whatever anybody passed. It means what it says now, and
       boot asks for both directions because the microphone costs one I2S
       channel that is idle until something reads it. */
    if (!cell_performance_active()) {
        const esp_err_t codec = ls_audio_hw_init(false);
        if (codec != ESP_OK)
            ESP_LOGE(TAG, "codec init failed (%s) - speaker disabled",
                     esp_err_to_name(codec));
    }
#else
    ESP_LOGW(TAG, "audio: no codec driver for this board - speaker disabled");
#endif

#if LS_HAS_HAPTIC
    if (ls_haptic_start() != ESP_OK)
        ESP_LOGW(TAG, "haptic: no driver at 0x%02X - vibration unavailable",
                 LS_BOARD_HAPTIC_I2C_ADDR);
#endif

    ESP_LOGI(TAG, "LakeShark radio-core boot (%s)",
             LS_BOARD_NAME);
    ESP_LOGI(TAG, "BOOT button (GPIO%d) cycles P25 -> ADS-B -> FM",
             (int)BOOT_BTN_GPIO);
    if (LS_HAS_VBUS_CTRL) {
        ESP_LOGI(TAG, "USB host VBUS switch on GPIO%d", (int)USB_VBUS_GPIO);
    } else {
        ESP_LOGI(TAG, "no software USB host VBUS switch; dongle power depends on the board supply");
    }

    ESP_LOGW(TAG, "heap before C6/BLE: internal=%u DMA=%u largest-DMA=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    ls_safe_stage(LS_SAFE_STAGE_C6);
#if !CONFIG_LS_C6_LINK
    /* The T-Display-P4's C6 still carries LilyGO's factory image,
       which does not speak esp_hosted.  Attempting the link there does not
       fail once - the SDIO transport retries forever and prints a CPU
       register dump per failed command, which floods the console until
       nothing else can be typed.  Skip it until c6_firmware/ has been
       written to the co-processor. */
    ESP_LOGW(TAG, "C6 link disabled at build time (CONFIG_LS_C6_LINK=n) - "
                  "co-processor needs the esp_hosted slave firmware");
#else
    {
        int e = cell_performance_active() ? -1 : esp_hosted_connect_to_slave();
        ESP_LOGI(TAG, "ESP-Hosted co-processor link: %s (%d)",
                 e == 0 ? "up" : "FAILED", e);

        if (e == 0) {
            /**/
            esp_hosted_coprocessor_fwver_t v = { 0 };
            if (esp_hosted_get_coprocessor_fwversion(&v) == 0) {
                snprintf(s_c6_fw, sizeof(s_c6_fw), "%lu.%lu.%lu",
                         (unsigned long)v.major1, (unsigned long)v.minor1,
                         (unsigned long)v.patch1);
                /* Patch counts: 2.12.9 against 2.12.3 passed this and the
                   transport still framed packets differently. */
                bool skew = ((uint32_t)ESP_HOSTED_VERSION_MAJOR_1 != v.major1 ||
                             (uint32_t)ESP_HOSTED_VERSION_MINOR_1 != v.minor1 ||
                             (uint32_t)ESP_HOSTED_VERSION_PATCH_1 != v.patch1);
                ESP_LOGW(TAG, "C6 esp_hosted: host %d.%d.%d, co-processor %s%s",
                         ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1,
                         ESP_HOSTED_VERSION_PATCH_1, s_c6_fw,
                         skew ? "  <-- MISMATCH, expect RPC/HCI timeouts" : "");
            } else {
                /* connect_to_slave() answers 0 even when the transport gave up,
                   so a failed query here is the first sign the link is dead. */
                ESP_LOGW(TAG, "C6 firmware version query failed");
            }

            /* BLE scans continuously once started, so a board with no
               control head attached pays for it all day. The preference is
               honoured here rather than by stopping it afterwards, which
               would still have brought the radio up. */
            esp_err_t be = ESP_OK;
            if (settings_peek_ble_at_boot()) {
                be = ble_link_start();
                ESP_LOGI(TAG, "BLE control head link: %s", esp_err_to_name(be));
            } else {
                ESP_LOGI(TAG, "BLE held off at boot by preference ('radios ble on' starts it)");
            }
            /* Peek, not get: settings_init() has not run yet, so the getter
               would answer from its default and 'radios wifi off' would be
               stored, reported and ignored. */
            const bool wifi_pref = settings_peek_wifi_at_boot();
            if (!wifi_pref)
                ESP_LOGI(TAG, "WiFi autojoin held off at boot by preference ('radios wifi on' restores it)");
            wifi_autojoin_ready = be == ESP_OK && wifi_pref;
        }
    }
#endif /* CONFIG_LS_C6_LINK */
    ESP_LOGW(TAG, "heap after C6/BLE: internal=%u DMA=%u largest-DMA=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    /* ESP-Hosted Wi-Fi and the SX1262 mesh both need a contiguous internal
       control block.  USB host enumeration fragments that heap down to
       sub-kilobyte pieces, so reserve the always-on links first. */
    if (wifi_autojoin_ready) {
        esp_err_t we = ls_wifi_sta_autojoin();
        if (we == ESP_OK) {
            ESP_LOGI(TAG, "WiFi station: rejoining saved network");
        } else if (we != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "WiFi station autojoin: %s", esp_err_to_name(we));
        }
    }

    ls_safe_stage(LS_SAFE_STAGE_BACKEND);
#if defined(LS_BOARD_LORA_CS_GPIO)
    /* Reserve radio DMA buffers before USB enumeration consumes transient heap. */
    esp_err_t lora_err = ls_lora_start();
    if (lora_err != ESP_OK)
        ESP_LOGW(TAG, "early LoRa initialization: %s", esp_err_to_name(lora_err));
#endif
    ls_mesh_boot();
    lakeshark_backend_start();

#if LS_HAS_RF_SWITCH
    if (settings_get_antenna_external()) {
        if (ls_board_hw_antenna_external(true) == ESP_OK)
            ESP_LOGW(TAG, "antenna: EXTERNAL (MMCX1) - stored preference. "
                          "Transmitting with nothing fitted may damage the front end");
        else
            ESP_LOGE(TAG, "antenna: could not select external, staying internal");
    } else {
        ESP_LOGI(TAG, "antenna: internal");
    }
#endif

    ls_safe_stage(LS_SAFE_STAGE_APPS);
    if(!cell_performance_active()) {
        /* Remember the receiver preference, but do not start its large USB
           stream during boot.  The first visible TUI screen will request the
           receiver it actually needs through ls_tui_radio_want().  Starting
           a restored REC session here consumed the last DMA/internal blocks
           before mesh and Wi-Fi could initialize, only for HOME to park it a
           moment later. */
        const char *resume = lakeshark_recovery_take_app();
        int m = settings_load_mode();
        if (resume && *resume) {
            if (hl_mode_index_by_name(resume, &m)) {
                ESP_LOGW(TAG, "rebooted to recover the USB dongle - '%s' is armed",
                         resume);
            } else {
                ESP_LOGW(TAG, "rebooted to recover the USB dongle but '%s' is not a "
                              "known mode - falling back to the saved mode", resume);
            }
        } else {
            ESP_LOGI(TAG, "saved mode armed: %s", s_modes[m].name);
        }
        s_mode = m;
        lakeshark_radio_park();
    }
    if(!cell_performance_active())audio_volume_set(settings_load_volume());

    vTaskDelay(pdMS_TO_TICKS(700));
    if(!cell_performance_active()) {
        pa_on();
        audio_out_ensure_unmuted();
    }
    /* The amplifier comes up here and the sound does NOT. The chime
       is the TUI splash's now - it plays on the first frame of the animation
       instead of a couple of seconds before the panel lights, which is what
       "give it a boot sound" actually meant. The splash also reads the
       boot-sound setting, which this call never did. */

    xTaskCreateStatic(boot_btn_task, "boot_btn", sizeof(s_boot_btn_stack),
                      NULL, 5, s_boot_btn_stack, &s_boot_btn_tcb);
    if(!cell_performance_active())xTaskCreateStatic(settings_task, "settings", SETTINGS_STACK_WORDS, NULL, 2,
                      s_settings_stack, &s_settings_tcb);

    /**/
    {
        radio_health_hooks_t rh = {
            .request_recovery = hl_health_recover,
            .power_cycle = hl_health_power_cycle,
            .power_cycle_endpoint_id = LS_RADIO_ENDPOINT_RTL_USB,
        };
        radio_health_init(&rh);
    }

    /* Probe before the link decides. The UI probes again later and that call
       is idempotent; what matters is that the answer exists by now. */
    ls_keypad_start();

    /* THE BATTERY INDICATOR NEEDS SOMEBODY TO OPEN THE GAUGE.

       Nothing did. draw_status asks ls_gauge_present() before it reserves a
       cell for the meter, and that only reports what a previous start found -
       it never starts one. ls_gauge_get() does start the gauge lazily, but
       the only path to it in the status bar is inside draw_battery, which
       draw_status will not call until ls_gauge_present() is already true. So
       the meter could not appear on its own, and did appear once something
       else read the gauge first: opening DIAG, or the mesh reading pack
       voltage for a telemetry frame. That is exactly the "works sometimes"
       this was reported as.

       It is started here rather than from the draw loop on purpose. A start
       with no cell in the bay costs an I2C probe with a 100 ms timeout, and
       from the draw loop that would be a 100 ms stall on EVERY frame for as
       long as the battery is out. Once, at boot, it costs 100 ms once.
       A failure is not an error: plenty of runs have no cell fitted. */
    (void)ls_gauge_start();

    vTaskDelay(pdMS_TO_TICKS(1500));
    /* The Flipper link and the keyboard radios want the same two pins. */

#if LS_HAS_LINK_UART
    esp_err_t lerr = ESP_ERR_NOT_SUPPORTED;
    flipper_link_cfg_t link_cfg = FLIPPER_LINK_CFG_DEFAULT();
    if (ls_keypad_present()) {
        ESP_LOGW(TAG, "flipper link held down: the keyboard owns GPIO%d/%d "
                      "(CC1101 GDO2 and nRF24 INT on the 2x8 header)",
                 link_cfg.rx_gpio, link_cfg.tx_gpio);
    } else {
        lerr = flipper_link_start(&link_cfg, &s_link_host);
        ESP_LOGI(TAG, "flipper link (rx=GPIO%d tx=GPIO%d @%lu): %s",
                 link_cfg.rx_gpio, link_cfg.tx_gpio,
                 (unsigned long)link_cfg.baud, esp_err_to_name(lerr));
    }
#else
    ESP_LOGI(TAG, "wired Flipper link: no board pins configured");
#endif

    esp_log_level_set("P25TEL",  ESP_LOG_ERROR);
    esp_log_level_set("P25DIAG", ESP_LOG_ERROR);
    esp_log_level_set("ADSB",    ESP_LOG_ERROR);

    esp_log_level_set("NimBLE",  ESP_LOG_WARN);

    ESP_LOGW(TAG, "opening BLE telemetry gate: internal=%u DMA=%u largest-DMA=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    if(!cell_performance_active())ble_link_allow_telemetry(true);

    if(cell_performance_active()) {
        ESP_LOGI(TAG,"CELL sensors: GPS=%s IMU=%s",
                 esp_err_to_name(ls_gps_start()),esp_err_to_name(ls_imu_start()));
    }
    const bool console_up = console_start(true);
#if LS_HAS_COMPACT_UI
    esp_err_t ui_err=compact_ui_start(compact_mode_changed);
    if (ui_err!=ESP_OK) ESP_LOGE(TAG,"compact UI: %s",esp_err_to_name(ui_err));
#endif
    if (console_up)
        ESP_LOGI(TAG, "console ready - type 'help' for commands");
    else
        ESP_LOGW(TAG, "boot finished WITHOUT a serial console - see the REPL line above");

    /* Not healthy yet: the one-shot at LS_SAFE_HEALTHY_MS clears the
       startup fault counter, not the end of app_main. */
    ls_safe_stage(LS_SAFE_STAGE_RUNNING);
    ls_safe_healthy_arm();
    if (!console_up && s_defer_q) atomic_store(&s_console_retry, true);
}
