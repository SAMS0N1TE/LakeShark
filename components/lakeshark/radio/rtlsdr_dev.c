/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#include "rtlsdr_dev.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#include "esp_libusb_private.h"
#include "esp_log.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "radio_endpoint.h"
#include "radio_health.h"
#include "rtl-sdr.h"
#include "rtl_adapter_private.h"
#include "rtl_sdr_private.h"

static const char *TAG = "rtlsdr_dev";
static rtlsdr_dev_t *s_dev;
static volatile bool s_read_cancelled;
static volatile bool s_adapter_streaming;

typedef struct {
    uint8_t dev_addr;
    usb_host_client_handle_t client;
} setup_arg_t;

static const ls_radio_range_t s_frequency_ranges[] = {
    {24000000, 1766000000},
};

/* The RTL resampler rejects the gap between 300 and 900 kSPS. */
static const ls_radio_range_t s_sample_rate_ranges[] = {
    {225001, 300000},
    {900001, 3200000},
};

static ls_radio_err_t rtl_error(int error)
{
    if (error == 0) return LS_RADIO_OK;
    if (error == -EINVAL) return LS_RADIO_ERR_INVALID;
    return LS_RADIO_ERR_IO;
}

static ls_radio_err_t rtl_iq_set_gain(void *ctx, ls_radio_gain_mode_t mode,
                                      int tenths_db,
                                      int *actual_tenths_db)
{
    rtlsdr_dev_t *dev = (rtlsdr_dev_t *)ctx;
    int error = rtlsdr_set_tuner_gain_mode(dev,
                                           mode == LS_RADIO_GAIN_MANUAL);
    if (error != 0) return rtl_error(error);
    error = rtlsdr_set_agc_mode(dev, mode == LS_RADIO_GAIN_AUTO);
    if (error != 0) return rtl_error(error);
    if (mode == LS_RADIO_GAIN_MANUAL) {
        error = rtlsdr_set_tuner_gain(dev, tenths_db);
        if (error != 0) return rtl_error(error);
        *actual_tenths_db = rtlsdr_get_tuner_gain(dev);
    } else {
        *actual_tenths_db = 0;
    }
    return LS_RADIO_OK;
}

static ls_radio_err_t rtl_iq_configure(void *ctx,
                                       const ls_radio_iq_config_t *requested,
                                       ls_radio_iq_config_t *actual)
{
    rtlsdr_dev_t *dev = (rtlsdr_dev_t *)ctx;
    if (!dev || requested->center_hz == 0 || requested->sample_rate_hz == 0)
        return LS_RADIO_ERR_INVALID;
    int error = rtlsdr_set_sample_rate(dev, requested->sample_rate_hz);
    if (error != 0) return rtl_error(error);
    error = rtlsdr_set_tuner_bandwidth(dev, requested->bandwidth_hz);
    if (error != 0) return rtl_error(error);
    error = rtlsdr_set_center_freq(dev, requested->center_hz);
    if (error != 0) return rtl_error(error);

    int actual_gain = 0;
    ls_radio_err_t radio_error = rtl_iq_set_gain(
        dev, requested->gain_mode, requested->gain_tenths_db, &actual_gain);
    if (radio_error != LS_RADIO_OK) return radio_error;

    *actual = *requested;
    actual->center_hz = rtlsdr_get_center_freq(dev);
    actual->sample_rate_hz = rtlsdr_get_sample_rate(dev);
    actual->bandwidth_hz = rtlsdr_get_tuner_bandwidth(dev);
    actual->gain_tenths_db = actual_gain;
    return LS_RADIO_OK;
}

static ls_radio_err_t rtl_iq_start(void *ctx)
{
    rtlsdr_dev_t *dev = (rtlsdr_dev_t *)ctx;
    __atomic_store_n(&s_read_cancelled, false, __ATOMIC_RELEASE);
    int error = rtlsdr_stream_start(dev);
    if (error == ESP_LIBUSB_ERR_BUSY) return LS_RADIO_ERR_BUSY;
    if (error == ESP_LIBUSB_ERR_NO_MEM) return LS_RADIO_ERR_NO_MEMORY;
    if (error != 0) return rtl_error(error);
    __atomic_store_n(&s_adapter_streaming, true, __ATOMIC_RELEASE);
    return LS_RADIO_OK;
}

static ls_radio_err_t rtl_iq_read(void *ctx, void *dst, size_t bytes,
                                  uint32_t timeout_ms, size_t *read_bytes)
{
    (void)ctx;
    int max_bytes = bytes > INT_MAX ? INT_MAX : (int)bytes;
    uint32_t waited_ms = 0;
    for (;;) {
        if (__atomic_load_n(&s_read_cancelled, __ATOMIC_ACQUIRE))
            return LS_RADIO_ERR_STOPPED;
        int count = rtlsdr_stream_read_timeout(dst, max_bytes,
                                               timeout_ms == 0 ? 0 : 1);
        if (count > 0) {
            *read_bytes = (size_t)count;
            return LS_RADIO_OK;
        }
        if (count < 0) return LS_RADIO_ERR_STOPPED;
        if (timeout_ms == 0 || ++waited_ms >= timeout_ms)
            return LS_RADIO_ERR_TIMEOUT;
    }
}

static ls_radio_err_t rtl_iq_retune(void *ctx, uint64_t center_hz, bool fast,
                                    uint64_t *actual_hz)
{
    rtlsdr_dev_t *dev = (rtlsdr_dev_t *)ctx;
    bool was_streaming = __atomic_load_n(&s_adapter_streaming,
                                         __ATOMIC_ACQUIRE);
    if (was_streaming) {
        __atomic_store_n(&s_read_cancelled, true, __ATOMIC_RELEASE);
        rtlsdr_stream_stop_for(dev);
        __atomic_store_n(&s_adapter_streaming, false, __ATOMIC_RELEASE);
    }

    /* Name a nonsense tune where it is requested, not three layers down in the tuner. */

    if (center_hz < 1000000ull || center_hz > 2000000000ull) {
        ESP_LOGE(TAG, "refusing an out-of-range tune: %llu Hz",
                 (unsigned long long)center_hz);
        return LS_RADIO_ERR_INVALID;
    }

    int error = rtlsdr_set_center_freq(dev, (uint32_t)center_hz);
    if (error == 0 && !fast)
        error = rtlsdr_reset_buffer(dev);
    rtlsdr_stream_reset();

    if (error == 0 && was_streaming) {
        __atomic_store_n(&s_read_cancelled, false, __ATOMIC_RELEASE);
        error = rtlsdr_stream_start(dev);
        if (error == 0)
            __atomic_store_n(&s_adapter_streaming, true, __ATOMIC_RELEASE);
    }
    if (error != 0) return rtl_error(error);
    *actual_hz = rtlsdr_get_center_freq(dev);
    return *actual_hz != 0 ? LS_RADIO_OK : LS_RADIO_ERR_IO;
}

static ls_radio_err_t rtl_iq_stop(void *ctx)
{
    (void)ctx;
    __atomic_store_n(&s_read_cancelled, true, __ATOMIC_RELEASE);
    rtlsdr_stream_stop_for((rtlsdr_dev_t *)ctx);
    __atomic_store_n(&s_adapter_streaming, false, __ATOMIC_RELEASE);
    return LS_RADIO_OK;
}

static void rtl_cancel_read(void *ctx)
{
    (void)ctx;
    __atomic_store_n(&s_read_cancelled, true, __ATOMIC_RELEASE);
}

static ls_radio_err_t rtl_recover(void *ctx)
{
    rtlsdr_dev_t *dev = (rtlsdr_dev_t *)ctx;
    ESP_LOGW(TAG, "resetting RTL USB interface to clear a wedged endpoint");
    return dev && rtlsdr_reset_interface(dev) == 0
               ? LS_RADIO_OK : LS_RADIO_ERR_IO;
}

static const ls_radio_driver_ops_t s_rtl_ops = {
    .iq_configure = rtl_iq_configure,
    .iq_set_gain = rtl_iq_set_gain,
    .iq_start = rtl_iq_start,
    .iq_read = rtl_iq_read,
    .iq_retune = rtl_iq_retune,
    .iq_stop = rtl_iq_stop,
    .cancel_read = rtl_cancel_read,
    .recover = rtl_recover,
};

static ls_radio_err_t rtl_register_endpoint(rtlsdr_dev_t *dev)
{
    const ls_radio_endpoint_t endpoint = {
        .endpoint_id = LS_RADIO_ENDPOINT_RTL_USB,
        .name = "RTL-SDR USB IQ receiver",
        .capabilities = LS_RADIO_RX_IQ_U8,
        .iq_formats = LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
        .frequency_ranges = s_frequency_ranges,
        .frequency_range_count = sizeof(s_frequency_ranges) /
                                 sizeof(s_frequency_ranges[0]),
        .sample_rate_ranges = s_sample_rate_ranges,
        .sample_rate_range_count = sizeof(s_sample_rate_ranges) /
                                   sizeof(s_sample_rate_ranges[0]),
        .ops = &s_rtl_ops,
        .driver_ctx = dev,
    };
    return ls_radio_endpoint_register(&endpoint);
}

static void rtl_unregister_endpoint(void)
{
    if (!s_dev) return;
    /* present is cleared before cancellation, so a read that races a
     * USB detach can only complete as DISCONNECTED and teardown cannot free
     * the rtlsdr_dev_t until that read has left the adapter. */
    (void)ls_radio_endpoint_unregister(LS_RADIO_ENDPOINT_RTL_USB);
}

bool rtl_adapter_note_removed(usb_device_handle_t device)
{
    if (!s_dev || rtlsdr_usb_device_handle(s_dev) != device) return false;
    /* A real unplug must invalidate USB handles before close tries to drain
     * transfer objects. unregister first wakes and quiesces every session read;
     * neither step calls an application lifecycle callback. */
    esp_libusb_note_device_gone(device);
    rtlsdr_dev_teardown();
    return true;
}

void rtl_adapter_note_transport_fault(void)
{
    radio_health_note_fault(LS_RADIO_ENDPOINT_RTL_USB);
}

/**/
void rtlsdr_dev_teardown(void)
{
    rtlsdr_dev_t *dev = s_dev;
    if (!dev) return;

    rtl_unregister_endpoint();
    /* On a requested/simulated detach the USB device is still reachable, so
     * stop and drain its transfer pool before rtlsdr_close marks handles gone.
     * On a physical removal note_removed already cleared the handles and this
     * becomes a transport-local no-op. */
    rtlsdr_stream_stop_for(dev);
    __atomic_store_n(&s_adapter_streaming, false, __ATOMIC_RELEASE);
    s_dev = NULL;
    rtlsdr_close(dev);
    ESP_LOGW(TAG, "device object released");
}

static void rtlsdr_setup_task(void *arg)
{
    setup_arg_t *setup = (setup_arg_t *)arg;
    if (s_dev) {
        ls_radio_endpoint_info_t info;
        if (ls_radio_endpoint_get(LS_RADIO_ENDPOINT_RTL_USB, &info) == LS_RADIO_OK &&
            info.present) {
            ESP_LOGI(TAG, "an RTL endpoint is already present - ignoring USB addr %u",
                     setup->dev_addr);
            vPortFree(setup);
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGW(TAG, "a device object was still open - releasing it before re-opening");
        rtlsdr_dev_teardown();
    }

    int error = rtlsdr_open(&s_dev, setup->dev_addr, setup->client);
    if (error < 0) {
        ESP_LOGI(TAG, "USB device is not a supported RTL-SDR endpoint");
        s_dev = NULL;
        vPortFree(setup);
        vTaskDelete(NULL);
        return;
    }

    rtlsdr_set_freq_correction(s_dev, 0);
    rtlsdr_reset_buffer(s_dev);
    ls_radio_err_t register_error = LS_RADIO_ERR_BUSY;
    /* a replacement dongle can enumerate before the old app task has
     * observed DISCONNECTED and released its invalid session. Reusing that
     * static session slot early would alias the stale handle; wait for the
     * bounded app read to release it, then publish the new attach. */
    for (int attempt = 0; attempt < 200; ++attempt) {
        register_error = rtl_register_endpoint(s_dev);
        if (register_error != LS_RADIO_ERR_BUSY) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (register_error != LS_RADIO_OK) {
        ESP_LOGE(TAG, "endpoint registration failed: %s",
                 ls_radio_err_name(register_error));
        rtlsdr_close(s_dev);
        s_dev = NULL;
        vPortFree(setup);
        vTaskDelete(NULL);
        return;
    }

    event_bus_publish_simple(EVT_TUNER_LOCKED, "rtlsdr");
    ESP_LOGI(TAG, "IQ endpoint registered, awaiting app config");

    vPortFree(setup);
    vTaskDelete(NULL);
}

void rtl_adapter_probe_async(uint8_t dev_addr,
                             usb_host_client_handle_t client)
{
    setup_arg_t *setup = pvPortMalloc(sizeof(*setup));
    if (!setup) {
        ESP_LOGE(TAG, "setup arg alloc failed");
        return;
    }
    setup->dev_addr = dev_addr;
    setup->client = client;
    xTaskCreatePinnedToCore(rtlsdr_setup_task, "rtlsdr_setup", 8192,
                            setup, 4, NULL, 0);
}
