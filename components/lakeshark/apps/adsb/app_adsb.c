#include "app_registry.h"
#include "settings.h"
#include "event_bus.h"
#include "adsb_decode.h"
#include "adsb_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "rtl-sdr.h"
#include "sdkconfig.h"

#ifdef CONFIG_ENABLE_TUI
#include "tui.h"
extern void adsb_draw_main(int top, int rows, int cols);
extern void adsb_draw_signal(int top, int rows, int cols);
extern void adsb_draw_diag(int top, int rows, int cols);
extern void adsb_on_enter_tui(void);
#endif

extern void adsb_rx_task(void *arg);
extern rtlsdr_dev_t *rtlsdr_dev_get(void);

volatile bool adsb_rx_should_run = false;
volatile bool adsb_rx_running    = false;

static const char *TAG = "adsb";
static volatile bool s_age_running = false;
static volatile bool s_age_should_run = false;

static void age_task(void *arg)
{
    s_age_running = true;
    while (s_age_should_run) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        adsb_periodic_age(esp_timer_get_time());
    }
    s_age_running = false;
    vTaskDelete(NULL);
}

static void adsb_on_enter(void)
{

    if (adsb_rx_should_run) return;

#ifdef CONFIG_ENABLE_TUI
    adsb_on_enter_tui();
#endif

    rtlsdr_dev_t *dev = rtlsdr_dev_get();
    if (dev) {
        const app_t *cur = app_current();
        uint32_t freq = cur ? settings_get_freq(cur) : 1090000000UL;
        int      gain = cur ? settings_get_gain(cur) : 496;
        if (freq < 1080000000UL || freq > 1100000000UL) {
            freq = 1090000000UL;
            if (cur) settings_set_freq(cur, freq);
        }
        if (gain <= 0) {
            gain = 496;
            if (cur) settings_set_gain(cur, gain);
        }
        rtlsdr_set_center_freq(dev, freq);
        rtlsdr_set_sample_rate(dev, 2000000);
        rtlsdr_set_tuner_gain_mode(dev, 1);
        rtlsdr_set_tuner_gain(dev, gain);
        rtlsdr_set_agc_mode(dev, 0);
        rtlsdr_reset_buffer(dev);
        ESP_LOGI(TAG, "tuned %lu Hz 2 MSPS gain=%d", (unsigned long)freq, gain);
    }

    s_age_should_run = true;
    xTaskCreatePinnedToCore(age_task, "adsb_age", 3072, NULL, 1, NULL, 1);

    adsb_rx_should_run = true;
    xTaskCreatePinnedToCore(adsb_rx_task, "adsb_rx", 16384, NULL, 5, NULL, 1);
}

static void adsb_on_exit(void)
{
    adsb_rx_should_run = false;
    s_age_should_run   = false;

    int waited;
    for (waited = 0; waited < 300 && (adsb_rx_running || s_age_running); waited++)
        vTaskDelay(pdMS_TO_TICKS(10));

    if (adsb_rx_running || s_age_running) {

        ESP_LOGE(TAG, "*** ADS-B drain TIMEOUT after %dms (rx=%d age=%d) ***",
                 waited * 10, adsb_rx_running, s_age_running);
        ESP_LOGE(TAG, "*** Next app will see degraded throughput. Reboot ***");
    } else {
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
    return app_register(&ADSB_APP);
}
