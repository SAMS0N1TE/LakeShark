#include "app_registry.h"
#include "settings.h"
#include "event_bus.h"
#include "adsb_decode.h"
#include "adsb_state.h"
#include "adsb_app.h"
#include "adsb_demo.h"   /**/
#include "adsb_source.h"
#include "uat_stream.h"
#include "uat_weather.h"
#include <string.h>
#include "iq_app_control.h"
#include "radio_endpoint.h"
#include "radio_choice.h"
#include "radio_decode_worker.h"
#include "perf.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "ls_lora.h"
#include "sdkconfig.h"
#include <stdlib.h>

#ifdef CONFIG_ENABLE_TUI
#include "tui.h"
extern void adsb_draw_main(int top, int rows, int cols);
extern void adsb_draw_signal(int top, int rows, int cols);
extern void adsb_draw_diag(int top, int rows, int cols);
extern void adsb_on_enter_tui(void);
#endif

#define ADSB_IQ_READ_BYTES 8192
#define ADSB_READ_TIMEOUT_MS 20

static volatile bool s_rx_should_run = false;
static volatile bool s_rx_running    = false;
static ls_radio_session_t *s_session;
static ls_iq_control_t s_radio_control;
static bool s_dual_chip;
static volatile bool s_console_uat;
static int s_app_index = -1;
static EXT_RAM_BSS_ATTR uat_stream_t s_uat;
static uat_stats_t s_uat_stats;
static portMUX_TYPE s_uat_lock = portMUX_INITIALIZER_UNLOCKED;

void adsb_uat_stats(uat_stats_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_uat_lock);
    *out = s_uat_stats;
    portEXIT_CRITICAL(&s_uat_lock);
}

bool adsb_uat_start(void)
{
    if (s_app_index < 0) return false;
    s_console_uat = true;
    app_switch_to(s_app_index);
    return true;
}

void adsb_uat_stop(void)
{
    s_console_uat = false;
    if (app_current_index() == s_app_index) {
        s_rx_should_run = false;
        app_park();
    }
}

void adsb_uat_use_picker(void) { s_console_uat = false; }

static void uat_down(const uat_fix_t *fix, void *user)
{
    (void)user;
    adsb_decode_on_uat(fix);
}

static void uat_up(const uint8_t *payload, void *user)
{
    (void)user;
    uat_weather_uplink(payload, esp_timer_get_time());
}

/* Which receiver is feeding the decoder right now. Written by the rx task,
   read by whatever reports radio status. */
static volatile adsb_source_t s_source = ADSB_SRC_NONE;
static const char *volatile s_status = "stopped";
const char *adsb_receiver_status(void) { return s_status; }

adsb_source_t adsb_active_source(void) { return s_source; }
const char *adsb_active_source_name(void) { return adsb_source_name(s_source); }

/* The mesh owns the LoRa socket between sessions, so a Mode S session has to
   ask, as an FSK session does. Declared rather than included: ls_mesh.h belongs
   to the meshcore component, which depends on this one. Weak, so a build
   without a mesh links and reports the radio as free, which it is. */
__attribute__((weak)) bool ls_mesh_radio_hold_bg(bool hold) { (void)hold; return true; }
__attribute__((weak)) bool ls_mesh_radio_wanted(void) { return false; }

static const adsb_modes_ops_t LORA_OPS = {
    .begin    = ls_lora_modes_begin,
    .poll     = ls_lora_modes_poll,
    .set_gain = ls_lora_modes_set_gain,
    .end      = ls_lora_modes_end,
};

static const char *TAG = "adsb";
static volatile bool s_age_running = false;
static volatile bool s_age_should_run = false;
static TaskHandle_t  s_age_task;

/* Its stack is in PSRAM: internal memory is all DMA-capable on the P4, and
   ADS-B streaming leaves little of it for the SPI and SD transfers that need
   it. Nothing here writes flash. The task stops itself and the stopping side
   deletes it; deleting itself WithCaps would have IDF start a helper task in
   internal memory to do it. */
static void age_task(void *arg)
{
    s_age_running = true;
    while (s_age_should_run) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        /* Before ageing, so demo aircraft refresh their last_seen and
           are not immediately aged out by the very next call. */
        adsb_demo_tick();
        adsb_periodic_age(esp_timer_get_time());
    }
    s_age_running = false;
    vTaskSuspend(NULL);
}

static void age_reap(void)
{
    if (s_age_task && !s_age_running) {
        vTaskDeleteWithCaps(s_age_task);
        s_age_task = NULL;
    }
}

static uint32_t s_cfg_freq = 1090000000UL;
static int      s_cfg_gain = 496;
/* The mini map's FOLLOW mode, -1 until one is chosen. Kept with the gain so
   the screen reads RAM: NVS is read once, at registration. */
static int      s_cfg_follow = -1;

int adsb_requested_gain(void)
{
    return __atomic_load_n(&s_cfg_gain, __ATOMIC_RELAXED);
}

int adsb_map_follow(void)
{
    return __atomic_load_n(&s_cfg_follow, __ATOMIC_RELAXED);
}

void adsb_set_map_follow(int mode)
{
    if (mode < 0 || mode > 15) return;
    if (__atomic_exchange_n(&s_cfg_follow, mode, __ATOMIC_RELAXED) != mode)
        settings_set_adsb_follow(mode);
}

void adsb_request_gain(int gain_tenths_db)
{
    /* The RTL-SDR tops out at 49.6 dB; the HackRF's LNA, VGA and amp
       reach 113. */
    const int most = ls_rsel_gain_max(LS_RSEL_ADSB);
    if (gain_tenths_db < 0) gain_tenths_db = 0;
    if (gain_tenths_db > most) gain_tenths_db = most;
    __atomic_store_n(&s_cfg_gain, gain_tenths_db, __ATOMIC_RELAXED);
    ls_iq_control_request_gain(&s_radio_control, gain_tenths_db);
}

static const ls_radio_requirements_t IQ_REQUIREMENTS = {
    .required_caps = LS_RADIO_RX_IQ_U8,
    .min_hz = 1080000000UL,
    .max_hz = 1100000000UL,
    .sample_rate_hz = 2000000,
    .iq_format = LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
};

static bool adsb_iq_present(void)
{
    return ls_radio_endpoint_available(&IQ_REQUIREMENTS);
}

/* Where frames should come from now: the RADIO picker's choice, falling
   back as the picker's button does. RAM reads only - this runs on the
   receive task. */
static adsb_source_t adsb_choose(void)
{
    if (ls_rsel_saved(LS_RSEL_ADSB) == LS_RSEL_ADSB_DUAL &&
        (ls_lora_caps() & LS_LORA_CAP_MODES_RX)) {
        ls_radio_requirements_t uat = IQ_REQUIREMENTS;
        uat.min_hz = uat.max_hz = 978000000;
        uat.sample_rate_hz = UAT_SAMPLE_RATE;
        uat.preferred_endpoint_id = LS_RADIO_ENDPOINT_RTL_USB;
        if (ls_radio_endpoint_available(&uat)) return ADSB_SRC_DUAL;
        return ADSB_SRC_LORA;
    }
    if (s_console_uat) return ADSB_SRC_UAT;
    return adsb_source_choose(ls_rsel_saved(LS_RSEL_ADSB) == LS_RSEL_LORA,
                              adsb_iq_present(), ls_lora_caps());
}

static bool adsb_radio_open(adsb_source_t source)
{
    ls_radio_requirements_t requirements = IQ_REQUIREMENTS;
    bool uat = source == ADSB_SRC_DUAL || source == ADSB_SRC_UAT;
    if (uat) {
        requirements.min_hz = requirements.max_hz = 978000000;
        requirements.sample_rate_hz = UAT_SAMPLE_RATE;
    }
    requirements.preferred_endpoint_id = uat ? LS_RADIO_ENDPOINT_RTL_USB : ls_rsel_sdr_endpoint(LS_RSEL_ADSB);
    ls_radio_err_t error = ls_radio_acquire("adsb", &requirements,
                                            &s_session);
    if (error != LS_RADIO_OK) {
        s_status = error == LS_RADIO_ERR_BUSY ? "radio busy: retrying" :
                   uat && !ls_radio_endpoint_available(&requirements) ? "UAT needs RTL-SDR at 978 MHz" :
                   adsb_iq_present() ? "open failed: retrying" : "no receiver: attach radio";
        return false;
    }

    const ls_radio_iq_config_t requested = {
        .center_hz = uat ? 978000000 : s_cfg_freq,
        .sample_rate_hz = uat ? UAT_SAMPLE_RATE : 2000000,
        .bandwidth_hz = 0,
        .gain_mode = s_cfg_gain == 0 ? LS_RADIO_GAIN_AUTO
                                     : LS_RADIO_GAIN_MANUAL,
        .gain_tenths_db = s_cfg_gain,
    };
    ls_radio_iq_config_t actual;
    error = ls_radio_iq_configure(s_session, &requested, &actual);
    if (error == LS_RADIO_OK && uat &&
        (actual.sample_rate_hz < UAT_SAMPLE_RATE - 2 || actual.sample_rate_hz > UAT_SAMPLE_RATE + 2 ||
         actual.center_hz != 978000000))
        error = LS_RADIO_ERR_UNSUPPORTED;
    if (error == LS_RADIO_OK) error = ls_radio_iq_start(s_session);
    if (error != LS_RADIO_OK) {
        s_status = "start failed: retrying";
        ESP_LOGE(TAG, "radio open failed: %s", ls_radio_err_name(error));
        ls_radio_release(s_session);
        s_session = NULL;
        return false;
    }
    ESP_LOGI(TAG, "source: %s, radio %.3f MHz %lu SPS gain=%d",
             adsb_source_name(ADSB_SRC_IQ), actual.center_hz / 1e6,
             (unsigned long)actual.sample_rate_hz, actual.gain_tenths_db);
    return true;
}

/* Take the LoRa socket's radio from the mesh, as a background holder that
   gives it to anything else that asks (an experiment, a sweep, LoRa Labs). */
static bool adsb_lora_hold(void)
{
    bool held = false;
    const int64_t deadline = esp_timer_get_time() + 1000000;
    while (!(held = ls_mesh_radio_hold_bg(true)) && !ls_mesh_radio_wanted() &&
           esp_timer_get_time() < deadline)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (!held) ls_mesh_radio_hold_bg(false);
    return held;
}

/* The Mode S session on the LoRa socket's chip, until the app stops, the
   choice moves to an IQ receiver (one that appears is taken unless the chip
   was chosen), something else asks for the chip, or the chip stops
   answering. Returns with the chip back in standby and the radio handed
   back to the mesh. */
static void adsb_lora_run(void)
{
    if (ls_mesh_radio_wanted()) {
        s_status = "radio busy: retrying";
        vTaskDelay(pdMS_TO_TICKS(200));
        return;
    }
    if (!adsb_lora_hold()) {
        s_status = "radio busy: retrying";
        if (!ls_mesh_radio_wanted()) ESP_LOGW(TAG, "mesh would not release the LoRa radio");
        vTaskDelay(pdMS_TO_TICKS(500));
        return;
    }

    int step = adsb_lr_gain_step(adsb_requested_gain());
    const esp_err_t error = LORA_OPS.begin(s_cfg_freq, step);
    if (error != ESP_OK) {
        s_status = "chip failed: retrying";
        ESP_LOGE(TAG, "Mode S session on the LoRa chip failed: %s", esp_err_to_name(error));
        ls_mesh_radio_hold_bg(false);
        vTaskDelay(pdMS_TO_TICKS(1000));
        return;
    }
    adsb_decode_reset_stream();
    s_source = ADSB_SRC_LORA;
    s_status = ls_rsel_saved(LS_RSEL_ADSB) == LS_RSEL_ADSB_DUAL ?
        "1090 LR; dual needs RTL-SDR" : "receiving: lr-chip";
    adsb_chip_diag_mark_start();     /* the rates in `adsb chips` run from here */
    ESP_LOGI(TAG, "source: %s, %.3f MHz gain step %d", adsb_source_name(ADSB_SRC_LORA),
             s_cfg_freq / 1e6, step);

    adsb_modes_stats_t stats = { 0 };
    int failed_in_a_row = 0;
    int64_t now = esp_timer_get_time();
    int64_t next_iq_check = now + 500000;
    int64_t last_report_us = now;

    while (s_rx_should_run) {
        if (ls_mesh_radio_wanted()) {
            ESP_LOGI(TAG, "LoRa chip asked for elsewhere, letting it go");
            break;
        }
        /* The gain request flag is only an edge; the setting itself is read
           below, so a request that arrived before the session is not stale. */
        ls_iq_control_request_t control;
        (void)ls_iq_control_take(&s_radio_control, &control);
        const int want = adsb_lr_gain_step(adsb_requested_gain());
        if (want != step) {
            if (LORA_OPS.set_gain(want) == ESP_OK) step = want;
            else ESP_LOGW(TAG, "gain step %d refused", want);
        }

        const int n = adsb_modes_pump(&LORA_OPS, &stats, 9);
        if (n < 0) {
            if (++failed_in_a_row >= 20) {
                s_status = "chip stalled: retrying";
                ESP_LOGE(TAG, "LoRa chip stopped answering, restarting the session");
                break;
            }
        } else {
            failed_in_a_row = 0;
        }
        vTaskDelay(1);   /* 1 ms: the chip holds nine frames */

        now = esp_timer_get_time();
        if (now >= next_iq_check) {
            next_iq_check = now + 500000;
            if (adsb_choose() != ADSB_SRC_LORA) {
                ESP_LOGI(TAG, "IQ receiver attached or chosen, leaving the LoRa chip");
                break;
            }
        }
        if (now - last_report_us >= 2000000) {
            ESP_LOGI(TAG, "lr-chip frames=%lu decoded=%lu bad_chips=%lu bad_crc=%lu errors=%lu",
                     (unsigned long)stats.frames, (unsigned long)stats.decoded,
                     (unsigned long)stats.bad_chips, (unsigned long)stats.bad_crc,
                     (unsigned long)stats.errors);
            last_report_us = now;
        }
    }

    (void)LORA_OPS.end();
    ls_mesh_radio_hold_bg(false);
    s_source = ADSB_SRC_NONE;
}

static void adsb_rx_close(void)
{
    if (s_session) {
        (void)ls_radio_iq_stop(s_session);
        ls_radio_release(s_session);
        s_session = NULL;
    }
    if (s_dual_chip) {
        (void)LORA_OPS.end();
        ls_mesh_radio_hold_bg(false);
        s_dual_chip = false;
    }
    s_source = ADSB_SRC_NONE;
}

static void adsb_rx_task(void *arg)
{
    (void)arg;
    uint8_t *buffer = heap_caps_malloc(ADSB_IQ_READ_BYTES,
                                       MALLOC_CAP_SPIRAM);
    if (!buffer) buffer = malloc(ADSB_IQ_READ_BYTES);
    if (!buffer) {
        s_status = "out of memory: re-enter";
        ESP_LOGE(TAG, "OOM IQ buffer");
        s_rx_running = false;
        return;
    }

    uint64_t loops = 0, fulls = 0, shorts = 0, errors = 0;
    int64_t last_report_us = esp_timer_get_time();
    int64_t last_yield = last_report_us;
    int64_t last_data_us = last_report_us;
    size_t got = 0;
    s_rx_running = true;
    adsb_modes_stats_t dual_stats = {0};
    int dual_errors = 0, dual_gain = 0;
    uat_stream_init(&s_uat, uat_down, uat_up, NULL);
    portENTER_CRITICAL(&s_uat_lock);
    memset(&s_uat_stats, 0, sizeof(s_uat_stats));
    portEXIT_CRITICAL(&s_uat_lock);

    while (s_rx_should_run) {
        if (!s_session) {
            /* The chosen receiver, falling back to whichever of an IQ
               receiver and the LoRa socket's chip can do Mode S. With
               neither, the IQ open below keeps retrying, as it always has. */
            adsb_source_t chosen = adsb_choose();
            if (chosen == ADSB_SRC_LORA) {
                adsb_lora_run();
                continue;
            }
            if (chosen == ADSB_SRC_DUAL) {
                if (ls_mesh_radio_wanted() || !adsb_lora_hold()) {
                    s_status = "LR2021 busy: retrying dual";
                    vTaskDelay(pdMS_TO_TICKS(100));
                    continue;
                }
                dual_gain = adsb_lr_gain_step(adsb_requested_gain());
                if (LORA_OPS.begin(1090000000, dual_gain) != ESP_OK) {
                    (void)LORA_OPS.end();
                    ls_mesh_radio_hold_bg(false);
                    s_status = "LR2021 failed: retrying dual";
                    vTaskDelay(pdMS_TO_TICKS(100));
                    continue;
                }
                s_dual_chip = true;
                dual_errors = 0;
                memset(&dual_stats, 0, sizeof(dual_stats));
                adsb_chip_diag_mark_start();
            }
            if (!adsb_radio_open(chosen)) {
                adsb_rx_close();
                s_source = ADSB_SRC_NONE;
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            adsb_decode_reset_stream();
            got = 0;
            last_data_us = esp_timer_get_time();
            s_source = chosen;
            if (chosen == ADSB_SRC_DUAL || chosen == ADSB_SRC_UAT) {
                /* A gap or retune invalidates phase, byte carry and partial
                   frames. Counters continue for this app entry. */
                uat_stats_t saved = s_uat.stats;
                uat_stream_init(&s_uat, uat_down, uat_up, NULL);
                s_uat.stats = saved;
            }
            s_status = chosen == ADSB_SRC_IQ && ls_rsel_saved(LS_RSEL_ADSB) == LS_RSEL_ADSB_DUAL ?
                "dual needs LR2021; waiting 1090 IQ" : "waiting for IQ data";
        }

        if (s_dual_chip) {
            if (ls_mesh_radio_wanted()) {
                s_status = "LR2021 requested elsewhere";
                adsb_rx_close();
                got = 0;
                continue;
            }
            int want = adsb_lr_gain_step(adsb_requested_gain());
            if (want != dual_gain && LORA_OPS.set_gain(want) == ESP_OK) dual_gain = want;
            if (adsb_modes_pump(&LORA_OPS, &dual_stats, 9) < 0) dual_errors++;
            else dual_errors = 0;
            if (dual_errors >= 20) {
                s_status = "LR2021 stalled: retrying dual";
                adsb_rx_close();
                got = 0;
                continue;
            }
        }
        ls_iq_control_request_t control;
        if (ls_iq_control_take(&s_radio_control, &control) &&
            (control.flags & LS_IQ_CONTROL_GAIN)) {
            int actual_gain = 0;
            ls_radio_err_t error = ls_radio_iq_set_gain(
                s_session,
                control.gain_tenths_db == 0 ? LS_RADIO_GAIN_AUTO
                                             : LS_RADIO_GAIN_MANUAL,
                control.gain_tenths_db, &actual_gain);
            if (error != LS_RADIO_OK)
                ESP_LOGW(TAG, "gain request failed: %s",
                         ls_radio_err_name(error));
        }

        loops++;
        size_t part = 0;
        ls_radio_err_t error = ls_radio_iq_read(
            s_session, buffer + got, ADSB_IQ_READ_BYTES - got,
            s_dual_chip ? 1 : ADSB_READ_TIMEOUT_MS, &part);
        if (error == LS_RADIO_OK && part) {
            s_status = s_source == ADSB_SRC_DUAL ? "1090 LR + 978 RTL receiving" :
                       s_source == ADSB_SRC_UAT ? "978 RTL receiving" :
                       ls_rsel_saved(LS_RSEL_ADSB) == LS_RSEL_ADSB_DUAL ?
                       "1090 IQ; dual needs LR2021" : "receiving: iq";
            got += part;
            last_data_us = esp_timer_get_time();
        } else if (error != LS_RADIO_OK && error != LS_RADIO_ERR_TIMEOUT) {
            ++errors;
            s_status = "read failed: retrying";
            adsb_rx_close();
            adsb_decode_reset_stream();
            got = 0;
        }
        if (got == ADSB_IQ_READ_BYTES) {
            ++fulls;
            perf_count_bytes(got);
            if (s_source == ADSB_SRC_DUAL || s_source == ADSB_SRC_UAT) {
                /* Drain Mode S between bounded UAT slices. Even a full USB
                   transfer must not defer the chip's nine-frame FIFO. */
                for (size_t at = 0; at < got; at += 2048) {
                    size_t bytes = got - at < 2048 ? got - at : 2048;
                    uat_stream_feed(&s_uat, buffer + at, bytes);
                    if (s_dual_chip) adsb_modes_pump(&LORA_OPS, &dual_stats, 9);
                }
                portENTER_CRITICAL(&s_uat_lock);
                s_uat_stats = s_uat.stats;
                portEXIT_CRITICAL(&s_uat_lock);
            } else adsb_on_sample(buffer, got);
            got = 0;
        }

        int64_t now = esp_timer_get_time();
        if (now - last_report_us >= 2000000) {
            ESP_LOGI(TAG, "loops=%llu full=%llu short=%llu errors=%llu",
                     loops, fulls, shorts, errors);
            last_report_us = now;
        }
        /* The LoRa chip chosen while the IQ receiver runs: give the dongle
           back and let the top of the loop start the chip's session. */
        if (s_session) {
            if (adsb_choose() != s_source || now - last_data_us >= 2000000) {
                s_status = adsb_choose() == ADSB_SRC_LORA ? "switching receiver" :
                           "IQ stalled: retrying";
                adsb_decode_reset_stream();
                got = 0;
                ESP_LOGI(TAG, "IQ receiver released: %s", s_status);
                adsb_rx_close();
                continue;
            }
        }
        if (now - last_yield > 25000) {
            last_yield = now;
            vTaskDelay(1);
        }
    }

    adsb_rx_close();
    s_source = ADSB_SRC_NONE;
    s_status = "stopped";
    adsb_decode_reset_stream();
    heap_caps_free(buffer);
    s_rx_running = false;
}

static void adsb_cache_settings(const app_t *a)
{
    if (!a) return;
    uint32_t freq = settings_get_freq(a);
    int      gain = settings_get_gain(a);

    if (freq < 1080000000UL || freq > 1100000000UL) {
        freq = 1090000000UL;
        settings_set_freq(a, freq);
    }
    if (gain < 0 || gain > 1130) {
        gain = 496;
        settings_set_gain(a, gain);
    }
    s_cfg_freq = freq;
    __atomic_store_n(&s_cfg_gain, gain, __ATOMIC_RELAXED);
    __atomic_store_n(&s_cfg_follow, settings_get_adsb_follow(), __ATOMIC_RELAXED);
}

static void adsb_on_enter(void)
{

    if (s_rx_should_run) return;

    for (int i = 0; i < 200 && (s_rx_running || s_age_running); i++)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (s_rx_running || s_age_running) {
        ESP_LOGE(TAG, "previous ADS-B tasks still alive (rx=%d age=%d) - refusing to start "
                      "a second reader; reboot to clear",
                 s_rx_running, s_age_running);
        return;
    }
    if (s_session) {
        ls_radio_release(s_session);
        s_session = NULL;
    }

#ifdef CONFIG_ENABLE_TUI
    adsb_on_enter_tui();
#endif

    /* Reuse the decoder stack reserved for the active radio app. */

    s_rx_should_run = true;
    s_rx_running = true;
    if (!ls_radio_decode_worker_start(adsb_rx_task, "adsb_rx", 5, 1)) {
        s_rx_should_run = false;
        s_rx_running = false;
        s_status = "decoder busy: re-enter";
        ESP_LOGE(TAG, "shared radio decoder is unavailable");
        return;
    }

    age_reap();
    s_age_should_run = true;
    s_age_running = true;
    if (xTaskCreatePinnedToCoreWithCaps(age_task, "adsb_age", 3072, NULL, 1,
                                        &s_age_task, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_age_task = NULL;
        s_age_should_run = false;
        s_age_running = false;
        ESP_LOGE(TAG, "adsb_age task create failed - contacts will not time "
                      "out (largest free block %u B)",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                            MALLOC_CAP_8BIT));
    }
}

static bool adsb_on_stop(void)
{
    s_rx_should_run = false;
    s_console_uat = false;
    s_age_should_run   = false;

    int waited;
    for (waited = 0; waited < 300 && (s_rx_running || s_age_running); waited++)
        vTaskDelay(pdMS_TO_TICKS(10));

    bool decoder_released = ls_radio_decode_worker_release(300, 10);
    if (s_rx_running || s_age_running || !decoder_released) {

        ESP_LOGE(TAG, "*** ADS-B drain TIMEOUT after %dms (rx=%d age=%d) ***",
                 waited * 10, s_rx_running, s_age_running);
        ESP_LOGE(TAG, "*** Next app will see degraded throughput. Reboot ***");
        return false;
    } else {
        age_reap();
        if (s_session) {
            ls_radio_release(s_session);
            s_session = NULL;
        }
        ESP_LOGI(TAG, "ADS-B drained cleanly in %dms", waited * 10);
    }
    return true;
}

static void adsb_on_key(tui_key_t k)
{
    int kk = (int)k;

#ifdef CONFIG_ENABLE_TUI

    page_t pg = page_current();
    if (pg == PAGE_MAIN || pg == PAGE_SIGNAL) {
        if (kk == TK_DOWN) {
            (void)adsb_select_get();
            adsb_select_next();
            tui_mark_dirty();
            return;
        }
        if (kk == TK_UP) {
            (void)adsb_select_get();
            adsb_select_prev();
            tui_mark_dirty();
            return;
        }
        if (kk == TK_ENTER && pg == PAGE_MAIN) {

            const adsb_aircraft_t *sel = adsb_select_get();
            if (sel) {
                page_set(PAGE_SIGNAL);
                tui_mark_dirty();
            }
            return;
        }
    }
#endif

    switch (kk) {
    case 't': case 'T':

        adsb_inject_fake_aircraft();
        ESP_LOGI(TAG, "test: injected fake aircraft");
        break;
    default:
        break;
    }
}

static const app_t ADSB_APP = {
    .name         = "ADS-B",
    .default_freq = 1090000000UL,
    .default_rate = 2000000,
    .default_gain = 496,
    .banner       = "ATC TERMINAL",
    .signal_label = "TRACK",
    .diag_label   = "DIAG",
    .on_enter     = adsb_on_enter,
    .on_stop      = adsb_on_stop,
    .on_sample    = adsb_on_sample,
#ifdef CONFIG_ENABLE_TUI
    .draw_main    = adsb_draw_main,
    .draw_signal  = adsb_draw_signal,
    .draw_diag    = adsb_draw_diag,
#else
    .draw_main    = NULL,
    .draw_signal  = NULL,
    .draw_diag    = NULL,
#endif
    .on_key       = adsb_on_key,
};

const app_t *adsb_app_desc(void) { return &ADSB_APP; }

int adsb_app_register(void)
{
    adsb_decode_init();
    int idx = app_register(&ADSB_APP);
    s_app_index = idx;

    adsb_cache_settings(&ADSB_APP);
    return idx;
}
