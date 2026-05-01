/* stream.c - bulk IQ read loop, dispatches to active app's on_sample. */
#include "stream.h"
#include "rtl-sdr.h"
#include "app_registry.h"
#include "perf.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdlib.h>

static const char *TAG = "stream";

extern rtlsdr_dev_t *rtlsdr_dev_get(void);

extern volatile bool adsb_rx_should_run;
extern volatile bool adsb_rx_running;

void adsb_rx_task(void *arg)
{
    rtlsdr_dev_t *dev = rtlsdr_dev_get();
    if (!dev) { ESP_LOGE(TAG, "no device"); vTaskDelete(NULL); return; }

    uint8_t *buffer = malloc(STREAM_BUFFER_BYTES);
    if (!buffer) { ESP_LOGE(TAG, "OOM rx buffer"); vTaskDelete(NULL); return; }

    ESP_LOGI(TAG, "rx task started, buf=%d packet=%d", STREAM_BUFFER_BYTES, STREAM_PACKET_SIZE);

    int n_read = 0;
    uint64_t loops = 0;
    uint64_t fulls = 0;
    uint64_t partials = 0;
    uint64_t errors = 0;
    int64_t last_report_us = esp_timer_get_time();

    adsb_rx_running = true;

    while (adsb_rx_should_run) {
        loops++;
        bool full = true;
        for (int i = 0; i < STREAM_BUFFER_BYTES; i += STREAM_PACKET_SIZE) {
            /* Recheck the should_run flag between transfers so a switch
             * away from ADS-B exits between bulk reads, not mid-flight.
             * Without this, a switch can sit on the framework's
             * on_exit drain wait for the full transfer interval (and
             * any USB errors that occur on the last transfer get
             * surfaced to the user as scary red logs). */
            if (!adsb_rx_should_run) { full = false; break; }
            int r = rtlsdr_read_sync(dev, &buffer[i], STREAM_PACKET_SIZE, &n_read);
            if (r < 0) {
                errors++;
                full = false;
                /* Suppress error log if we're being torn down - the USB
                 * stack returning errors during teardown is expected. */
                if (adsb_rx_should_run) {
                    ESP_LOGW(TAG, "USB read err r=%d", r);
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                break;
            }
            if ((uint32_t)n_read < STREAM_PACKET_SIZE) {
                partials++;
                full = false;
                break;
            }
        }
        if (full) {
            fulls++;
            perf_count_bytes(STREAM_BUFFER_BYTES);
            const app_t *a = app_current();
            if (a && a->on_sample) a->on_sample(buffer, STREAM_BUFFER_BYTES);
        }

        int64_t now = esp_timer_get_time();
        if (now - last_report_us >= 2000000) {
            ESP_LOGI(TAG, "loops=%llu full=%llu partial=%llu err=%llu n_read=%d",
                     loops, fulls, partials, errors, n_read);
            last_report_us = now;
        }
    }

    free(buffer);
    /* Clear the running flag so the framework's on_exit drain wait
     * actually sees the task as done. */
    adsb_rx_running = false;
    vTaskDelete(NULL);
}

void radio_stream_start(void)
{
    /* Task is spawned by rtlsdr_dev_setup_task once the tuner is locked. */
}