#include "app_registry.h"
#include "settings.h"
#include "event_bus.h"
#include "adsb_decode.h"
#include "adsb_state.h"
#include "adsb_app.h"
#include "adsb_demo.h"   /*LS-835*/
#include "iq_app_control.h"
#include "radio_endpoint.h"
#include "perf.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
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

static const char *TAG = "adsb";
static volatile bool s_age_running = false;
static volatile bool s_age_should_run = false;

static void age_task(void *arg)
{
    s_age_running = true;
    while (s_age_should_run) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        /*LS-835  Before ageing, so demo aircraft refresh their last_seen and
           are not immediately aged out by the very next call. */
        adsb_demo_tick();
        adsb_periodic_age(esp_timer_get_time());
    }
    s_age_running = false;
    vTaskDelete(NULL);
}

static uint32_t s_cfg_freq = 1090000000UL;
static int      s_cfg_gain = 496;

void adsb_request_gain(int gain_tenths_db)
{
    if (gain_tenths_db < 0) gain_tenths_db = 0;
    if (gain_tenths_db > 496) gain_tenths_db = 496;
    s_cfg_gain = gain_tenths_db;
    ls_iq_control_request_gain(&s_radio_control, gain_tenths_db);
}

static bool adsb_radio_open(void)
{
    const ls_radio_requirements_t requirements = {
        .required_caps = LS_RADIO_RX_IQ_U8,
        .min_hz = 1080000000UL,
        .max_hz = 1100000000UL,
        .sample_rate_hz = 2000000,
        .iq_format = LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
    };
    ls_radio_err_t error = ls_radio_acquire("adsb", &requirements,
                                            &s_session);
    if (error != LS_RADIO_OK) return false;

    const ls_radio_iq_config_t requested = {
        .center_hz = s_cfg_freq,
        .sample_rate_hz = 2000000,
        .bandwidth_hz = 0,
        .gain_mode = s_cfg_gain == 0 ? LS_RADIO_GAIN_AUTO
                                     : LS_RADIO_GAIN_MANUAL,
        .gain_tenths_db = s_cfg_gain,
    };
    ls_radio_iq_config_t actual;
    error = ls_radio_iq_configure(s_session, &requested, &actual);
    if (error == LS_RADIO_OK) error = ls_radio_iq_start(s_session);
    if (error != LS_RADIO_OK) {
        ESP_LOGE(TAG, "radio open failed: %s", ls_radio_err_name(error));
        ls_radio_release(s_session);
        s_session = NULL;
        return false;
    }
    ESP_LOGI(TAG, "radio %.3f MHz %lu SPS gain=%d", actual.center_hz / 1e6,
             (unsigned long)actual.sample_rate_hz, actual.gain_tenths_db);
    return true;
}

/* LS-180: This reader used to live in radio/stream.c and reached through the
 * RTL singleton, so app switches could leave a generic USB task dispatching
 * ADS-B samples after ownership changed.  The reader now owns an exact-format
 * session and every blocking read is bounded, making app drain independent of
 * the transport. */
static void adsb_rx_task(void *arg)
{
    (void)arg;
    uint8_t *buffer = heap_caps_malloc(ADSB_IQ_READ_BYTES,
                                       MALLOC_CAP_SPIRAM);
    if (!buffer) buffer = malloc(ADSB_IQ_READ_BYTES);
    if (!buffer) {
        ESP_LOGE(TAG, "OOM IQ buffer");
        s_rx_running = false;
        vTaskDelete(NULL);
        return;
    }

    uint64_t loops = 0, fulls = 0, shorts = 0, errors = 0;
    int64_t last_report_us = esp_timer_get_time();
    int64_t last_yield = last_report_us;
    s_rx_running = true;

    while (s_rx_should_run) {
        if (!s_session) {
            if (!adsb_radio_open()) {
                vTaskDelay(pdMS_TO_TICKS(100));
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
        size_t got = 0;
        bool full = true;
        while (got < ADSB_IQ_READ_BYTES && s_rx_should_run) {
            size_t part = 0;
            ls_radio_err_t error = ls_radio_iq_read(
                s_session, buffer + got, ADSB_IQ_READ_BYTES - got,
                ADSB_READ_TIMEOUT_MS, &part);
            if (error == LS_RADIO_OK) {
                got += part;
                continue;
            }
            if (error == LS_RADIO_ERR_TIMEOUT) continue;
            full = false;
            ++errors;
            if (error == LS_RADIO_ERR_DISCONNECTED) {
                ls_radio_release(s_session);
                s_session = NULL;
            }
            break;
        }

        if (full && got == ADSB_IQ_READ_BYTES) {
            ++fulls;
            perf_count_bytes(ADSB_IQ_READ_BYTES);
            adsb_on_sample(buffer, ADSB_IQ_READ_BYTES);
        } else if (got > 0) {
            ++shorts;
        }

        int64_t now = esp_timer_get_time();
        if (now - last_report_us >= 2000000) {
            ESP_LOGI(TAG, "loops=%llu full=%llu short=%llu errors=%llu",
                     loops, fulls, shorts, errors);
            last_report_us = now;
        }
        if (now - last_yield > 25000) {
            last_yield = now;
            vTaskDelay(1);
        }
    }

    if (s_session) {
        (void)ls_radio_iq_stop(s_session);
    }
    heap_caps_free(buffer);
    s_rx_running = false;
    vTaskDelete(NULL);
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
    if (gain <= 0) {
        gain = 496;
        settings_set_gain(a, gain);
    }
    s_cfg_freq = freq;
    s_cfg_gain = gain;
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

    /*LS-816  Biggest stack first, and check that it was actually created.

       adsb_age (3072) used to be created before adsb_rx (16384). On a board
       that has been running a while there is exactly one internal block left
       big enough for the reader, and putting the small task in first splits
       it - measured on the LCD 4.3, switching FM -> ADS-B:

           on FM      internal_largest=16384
           -> ADS-B   internal_largest=12800   own=? stream=0

       16384 - 3072 - allocator overhead is the 12800, so the reader's
       xTaskCreatePinnedToCore then failed. Nothing checked its return, so it
       failed in total silence: the app switched, drew its screen, and never
       acquired the radio. `fl FREQ` showed app=ADS-B park=0 rx=idle own=?
       stream=0 with no error logged anywhere, and the Flipper showed a live
       ADS-B screen with no aircraft. From a cold boot it worked, which is
       what made it look like an app-lifecycle bug rather than a memory one.

       Allocating the large stack first leaves the small one to fit anywhere.
       Neither ordering can conjure memory that is not there, so both creates
       now report failure instead of hiding it. */
    s_rx_should_run = true;
    if (xTaskCreatePinnedToCore(adsb_rx_task, "adsb_rx", 16384, NULL, 5,
                                NULL, 1) != pdPASS) {
        s_rx_should_run = false;
        ESP_LOGE(TAG, "adsb_rx task create failed - internal RAM exhausted "
                      "(largest free block %u B, need >%u). ADS-B will not "
                      "receive; free memory or reboot.",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                            MALLOC_CAP_8BIT),
                 (unsigned)16384);
        return;
    }

    s_age_should_run = true;
    if (xTaskCreatePinnedToCore(age_task, "adsb_age", 3072, NULL, 1,
                                NULL, 1) != pdPASS) {
        s_age_should_run = false;
        ESP_LOGE(TAG, "adsb_age task create failed - contacts will not time "
                      "out (largest free block %u B)",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                            MALLOC_CAP_8BIT));
    }
}

static void adsb_on_exit(void)
{
    s_rx_should_run = false;
    s_age_should_run   = false;

    int waited;
    for (waited = 0; waited < 300 && (s_rx_running || s_age_running); waited++)
        vTaskDelay(pdMS_TO_TICKS(10));

    if (s_rx_running || s_age_running) {

        ESP_LOGE(TAG, "*** ADS-B drain TIMEOUT after %dms (rx=%d age=%d) ***",
                 waited * 10, s_rx_running, s_age_running);
        ESP_LOGE(TAG, "*** Next app will see degraded throughput. Reboot ***");
    } else {
        if (s_session) {
            ls_radio_release(s_session);
            s_session = NULL;
        }
        ESP_LOGI(TAG, "ADS-B drained cleanly in %dms", waited * 10);
    }
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
    .on_exit      = adsb_on_exit,
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

    adsb_cache_settings(&ADSB_APP);
    return idx;
}
