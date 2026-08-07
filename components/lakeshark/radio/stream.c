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
    if (!dev) { ESP_LOGE(TAG, "no device"); adsb_rx_running = false; vTaskDelete(NULL); return; }

    uint8_t *buffer = malloc(STREAM_BUFFER_BYTES);
    if (!buffer) { ESP_LOGE(TAG, "OOM rx buffer"); adsb_rx_running = false; vTaskDelete(NULL); return; }

    uint64_t loops = 0;
    uint64_t fulls = 0;
    uint64_t shorts = 0;
    uint64_t errors = 0;
    int      read_errors = 0;
    int64_t last_report_us = esp_timer_get_time();
    int64_t last_yield     = last_report_us;

    adsb_rx_running = true;

    bool stream_started = false;
    if (rtlsdr_stream_start(dev) == 0) stream_started = true;
    else ESP_LOGE(TAG, "stream start failed");

    ESP_LOGI(TAG, "rx task started, buf=%d streaming=%d", STREAM_BUFFER_BYTES, stream_started);

    while (adsb_rx_should_run) {
        loops++;

        bool full = true;
        if (!stream_started) {

            vTaskDelay(pdMS_TO_TICKS(100));
            if (rtlsdr_stream_start(dev) == 0) stream_started = true;
            continue;
        }

        int got = 0;
        while (got < STREAM_BUFFER_BYTES) {
            if (!adsb_rx_should_run) { full = false; break; }
            int r = rtlsdr_stream_read(&buffer[got], STREAM_BUFFER_BYTES - got);
            if (r > 0) { got += r; read_errors = 0; continue; }

            if (++read_errors > 50) { errors++; read_errors = 0; full = false; break; }
            vTaskDelay(1);
        }

        if (full && got >= STREAM_BUFFER_BYTES) {
            fulls++;
            perf_count_bytes(STREAM_BUFFER_BYTES);
            const app_t *a = app_current();
            if (a && a->on_sample) a->on_sample(buffer, STREAM_BUFFER_BYTES);
        } else if (got > 0) {
            shorts++;
        }

        int64_t now = esp_timer_get_time();
        if (now - last_report_us >= 2000000) {
            ESP_LOGI(TAG, "loops=%llu full=%llu short=%llu dry=%llu",
                     loops, fulls, shorts, errors);
            last_report_us = now;
        }

        if (now - last_yield > 25000) { last_yield = now; vTaskDelay(1); }
    }

    if (stream_started) rtlsdr_stream_stop();
    free(buffer);

    adsb_rx_running = false;
    vTaskDelete(NULL);
}

void radio_stream_start(void)
{

}
