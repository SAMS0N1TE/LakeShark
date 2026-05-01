#include "rtlsdr_dev.h"
#include "rtl-sdr.h"
#include "app_registry.h"
#include "settings.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "rtlsdr_dev";

static rtlsdr_dev_t *s_dev = NULL;

typedef struct {
    uint8_t                  dev_addr;
    usb_host_client_handle_t client;
} setup_arg_t;

static void rtlsdr_setup_task(void *arg)
{
    setup_arg_t *sa = (setup_arg_t *)arg;

    int r = rtlsdr_open(&s_dev, sa->dev_addr, sa->client);
    if (r < 0) {
        ESP_LOGE(TAG, "rtlsdr_open failed");
        vPortFree(sa);
        vTaskDelete(NULL);
        return;
    }

    rtlsdr_set_freq_correction(s_dev, 0);
    rtlsdr_reset_buffer(s_dev);

    event_bus_publish_simple(EVT_TUNER_LOCKED, "rtlsdr");
    ESP_LOGI(TAG, "device opened, awaiting app config");

    const app_t *cur = app_current();
    if (cur && cur->on_enter) cur->on_enter();

    vPortFree(sa);
    vTaskDelete(NULL);
}

void rtlsdr_dev_setup_async(uint8_t dev_addr, usb_host_client_handle_t client)
{
    setup_arg_t *sa = pvPortMalloc(sizeof(*sa));
    if (!sa) { ESP_LOGE(TAG, "setup arg alloc failed"); return; }
    sa->dev_addr = dev_addr;
    sa->client   = client;
    xTaskCreatePinnedToCore(rtlsdr_setup_task, "rtlsdr_setup", 8192,
                            sa, 4, NULL, 0);
}

uint32_t rtlsdr_dev_set_freq(uint32_t hz)
{
    if (!s_dev) return 0;
    rtlsdr_set_center_freq(s_dev, hz);
    return hz;
}
int rtlsdr_dev_set_gain(int tenths_db)
{
    if (!s_dev) return -1;
    /* gain == 0 means AGC: turn manual mode off and let the tuner
     * track. Otherwise switch to manual mode and apply the value. */
    if (tenths_db == 0) {
        rtlsdr_set_tuner_gain_mode(s_dev, 0);
        return 0;
    }
    rtlsdr_set_tuner_gain_mode(s_dev, 1);
    return rtlsdr_set_tuner_gain(s_dev, tenths_db);
}
int rtlsdr_dev_set_sample_rate(uint32_t hz)
{
    if (!s_dev) return -1;
    return rtlsdr_set_sample_rate(s_dev, hz);
}

rtlsdr_dev_t *rtlsdr_dev_get(void) { return s_dev; }
