/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#include "hackrf_adapter_private.h"

#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "esp_libusb_private.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hackrf_radio.h"
#include "radio_endpoint.h"

#define HACKRF_USB_VID             UINT16_C(0x1d50)
#define HACKRF_USB_PID_JAWBREAKER  UINT16_C(0x604b)
#define HACKRF_USB_PID_ONE         UINT16_C(0x6089)
#define HACKRF_USB_PID_RAD1O       UINT16_C(0xcc15)
#define HACKRF_RX_ENDPOINT         UINT8_C(0x81)
#define HACKRF_CONTROL_TIMEOUT_MS  100

enum {
    HACKRF_REQ_SET_TRANSCEIVER_MODE = 1,
    HACKRF_REQ_SET_SAMPLE_RATE = 6,
    HACKRF_REQ_SET_BANDWIDTH = 7,
    HACKRF_REQ_SET_FREQUENCY = 16,
    HACKRF_REQ_SET_AMP_ENABLE = 17,
    HACKRF_REQ_SET_LNA_GAIN = 19,
    HACKRF_REQ_SET_VGA_GAIN = 20,
};

enum {
    HACKRF_MODE_OFF = 0,
    HACKRF_MODE_RECEIVE = 1,
};

typedef struct {
    class_driver_t usb;
    uint64_t center_hz;
    uint32_t sample_rate_hz;
    uint32_t bandwidth_hz;
    int gain_tenths_db;
    uint64_t dropped_at_start;
    volatile bool read_cancelled;
    bool streaming;
} hackrf_dev_t;

typedef struct {
    uint8_t dev_addr;
    usb_host_client_handle_t client;
} hackrf_setup_arg_t;

static const char *TAG = "hackrf_dev";
static hackrf_dev_t *s_dev;

static bool supported_pid(uint16_t pid)
{
    return pid == HACKRF_USB_PID_ONE || pid == HACKRF_USB_PID_JAWBREAKER ||
           pid == HACKRF_USB_PID_RAD1O;
}

static ls_radio_err_t control_out(hackrf_dev_t *dev, uint8_t request,
                                  uint16_t value, uint16_t index,
                                  uint8_t *data, uint16_t bytes)
{
    int result = esp_libusb_control_transfer(
        &dev->usb, CTRL_OUT, request, value, index, data, bytes,
        HACKRF_CONTROL_TIMEOUT_MS);
    return result == bytes ? LS_RADIO_OK : LS_RADIO_ERR_IO;
}

static ls_radio_err_t control_gain(hackrf_dev_t *dev, uint8_t request,
                                   uint16_t gain_db)
{
    uint8_t accepted = 0;
    int result = esp_libusb_control_transfer(
        &dev->usb, CTRL_IN, request, 0, gain_db, &accepted, 1,
        HACKRF_CONTROL_TIMEOUT_MS);
    return result == 1 && accepted ? LS_RADIO_OK : LS_RADIO_ERR_IO;
}

static ls_radio_err_t set_frequency(hackrf_dev_t *dev, uint64_t frequency_hz)
{
    uint8_t params[8];
    ls_hackrf_encode_frequency(frequency_hz, params);
    ls_radio_err_t error = control_out(dev, HACKRF_REQ_SET_FREQUENCY, 0, 0,
                                       params, sizeof(params));
    if (error == LS_RADIO_OK) dev->center_hz = frequency_hz;
    return error;
}

static ls_radio_err_t set_sample_rate(hackrf_dev_t *dev,
                                      uint32_t sample_rate_hz)
{
    uint8_t params[8];
    ls_hackrf_encode_sample_rate(sample_rate_hz, params);
    ls_radio_err_t error = control_out(dev, HACKRF_REQ_SET_SAMPLE_RATE, 0, 0,
                                       params, sizeof(params));
    if (error == LS_RADIO_OK) dev->sample_rate_hz = sample_rate_hz;
    return error;
}

static ls_radio_err_t set_bandwidth(hackrf_dev_t *dev, uint32_t bandwidth_hz)
{
    ls_radio_err_t error = control_out(
        dev, HACKRF_REQ_SET_BANDWIDTH, (uint16_t)bandwidth_hz,
        (uint16_t)(bandwidth_hz >> 16), NULL, 0);
    if (error == LS_RADIO_OK) dev->bandwidth_hz = bandwidth_hz;
    return error;
}

static ls_radio_err_t hackrf_iq_set_gain(void *ctx,
                                         ls_radio_gain_mode_t mode,
                                         int tenths_db,
                                         int *actual_tenths_db)
{
    hackrf_dev_t *dev = (hackrf_dev_t *)ctx;
    if (!dev || !actual_tenths_db) return LS_RADIO_ERR_INVALID;
    if (mode == LS_RADIO_GAIN_AUTO) return LS_RADIO_ERR_UNSUPPORTED;
    if (tenths_db < 0 || tenths_db > 1130) return LS_RADIO_ERR_UNSUPPORTED;

    bool amp = tenths_db > 1020;
    int remaining = tenths_db - (amp ? 110 : 0);
    int lna = remaining > 400 ? 400 : (remaining / 80) * 80;
    remaining -= lna;
    int vga = remaining > 620 ? 620 : (remaining / 20) * 20;

    ls_radio_err_t error = control_gain(dev, HACKRF_REQ_SET_LNA_GAIN,
                                        (uint16_t)(lna / 10));
    if (error != LS_RADIO_OK) return error;
    error = control_gain(dev, HACKRF_REQ_SET_VGA_GAIN,
                         (uint16_t)(vga / 10));
    if (error != LS_RADIO_OK) return error;
    error = control_out(dev, HACKRF_REQ_SET_AMP_ENABLE, amp ? 1 : 0, 0,
                        NULL, 0);
    if (error != LS_RADIO_OK) return error;

    dev->gain_tenths_db = lna + vga + (amp ? 110 : 0);
    *actual_tenths_db = dev->gain_tenths_db;
    return LS_RADIO_OK;
}

static ls_radio_err_t hackrf_iq_configure(
    void *ctx, const ls_radio_iq_config_t *requested,
    ls_radio_iq_config_t *actual)
{
    hackrf_dev_t *dev = (hackrf_dev_t *)ctx;
    if (!dev || !requested || !actual || requested->center_hz == 0 ||
        requested->sample_rate_hz == 0)
        return LS_RADIO_ERR_INVALID;

    ls_radio_err_t error = set_sample_rate(dev, requested->sample_rate_hz);
    if (error != LS_RADIO_OK) return error;
    uint32_t bandwidth = ls_hackrf_filter_bandwidth(
        requested->bandwidth_hz, requested->sample_rate_hz);
    error = set_bandwidth(dev, bandwidth);
    if (error != LS_RADIO_OK) return error;
    error = set_frequency(dev, requested->center_hz);
    if (error != LS_RADIO_OK) return error;
    int actual_gain = 0;
    error = hackrf_iq_set_gain(dev, requested->gain_mode,
                               requested->gain_tenths_db, &actual_gain);
    if (error != LS_RADIO_OK) return error;

    *actual = *requested;
    actual->center_hz = dev->center_hz;
    actual->sample_rate_hz = dev->sample_rate_hz;
    actual->bandwidth_hz = dev->bandwidth_hz;
    actual->gain_tenths_db = actual_gain;
    return LS_RADIO_OK;
}

static ls_radio_err_t hackrf_iq_start(void *ctx)
{
    hackrf_dev_t *dev = (hackrf_dev_t *)ctx;
    if (!dev) return LS_RADIO_ERR_INVALID;
    __atomic_store_n(&dev->read_cancelled, false, __ATOMIC_RELEASE);
    int result = esp_libusb_stream_start(&dev->usb, HACKRF_RX_ENDPOINT);
    if (result == ESP_LIBUSB_ERR_BUSY) return LS_RADIO_ERR_BUSY;
    if (result != 0) return LS_RADIO_ERR_IO;
    ls_radio_err_t error = control_out(dev, HACKRF_REQ_SET_TRANSCEIVER_MODE,
                                       HACKRF_MODE_RECEIVE, 0, NULL, 0);
    if (error != LS_RADIO_OK) {
        esp_libusb_stream_stop_for(&dev->usb);
        return error;
    }
    dev->dropped_at_start = esp_libusb_stream_dropped();
    dev->streaming = true;
    ESP_LOGI(TAG, "RX needs %" PRIu32 " B/s; 256 KiB ring covers %.3f ms",
             dev->sample_rate_hz * 2,
             262144000.0 / (dev->sample_rate_hz * 2));
    return LS_RADIO_OK;
}

static ls_radio_err_t hackrf_iq_read(void *ctx, void *dst, size_t bytes,
                                     uint32_t timeout_ms, size_t *read_bytes)
{
    hackrf_dev_t *dev = (hackrf_dev_t *)ctx;
    if (!dev || !dst || !read_bytes) return LS_RADIO_ERR_INVALID;
    uint32_t waited_ms = 0;
    for (;;) {
        if (__atomic_load_n(&dev->read_cancelled, __ATOMIC_ACQUIRE))
            return LS_RADIO_ERR_STOPPED;
        if (!esp_libusb_stream_owned_by(&dev->usb))
            return LS_RADIO_ERR_STOPPED;
        int max_bytes = bytes > INT_MAX ? INT_MAX : (int)bytes;
        int count = esp_libusb_stream_read_timeout((uint8_t *)dst, max_bytes,
                                                   timeout_ms == 0 ? 0 : 1);
        if (count < 0) return LS_RADIO_ERR_STOPPED;
        if (count == 0) {
            if (timeout_ms == 0 || ++waited_ms >= timeout_ms)
                return LS_RADIO_ERR_TIMEOUT;
            continue;
        }
        if (esp_libusb_stream_dropped() != dev->dropped_at_start) {
            ESP_LOGE(TAG,
                     "IQ ring overflow: refusing discontinuous sample data");
            return LS_RADIO_ERR_IO;
        }
        ls_hackrf_iq_s8_to_u8((uint8_t *)dst, (size_t)count);
        *read_bytes = (size_t)count;
        return LS_RADIO_OK;
    }
}

static ls_radio_err_t hackrf_iq_retune(void *ctx, uint64_t center_hz,
                                       bool fast, uint64_t *actual_hz)
{
    hackrf_dev_t *dev = (hackrf_dev_t *)ctx;
    (void)fast;
    ls_radio_err_t error = set_frequency(dev, center_hz);
    if (error != LS_RADIO_OK) return error;
    esp_libusb_stream_reset();
    *actual_hz = dev->center_hz;
    return LS_RADIO_OK;
}

static ls_radio_err_t hackrf_iq_stop(void *ctx)
{
    hackrf_dev_t *dev = (hackrf_dev_t *)ctx;
    if (!dev) return LS_RADIO_ERR_INVALID;
    __atomic_store_n(&dev->read_cancelled, true, __ATOMIC_RELEASE);
    ls_radio_err_t error = LS_RADIO_OK;
    if (dev->streaming)
        error = control_out(dev, HACKRF_REQ_SET_TRANSCEIVER_MODE,
                            HACKRF_MODE_OFF, 0, NULL, 0);
    esp_libusb_stream_stop_for(&dev->usb);
    dev->streaming = false;
    return error;
}

static void hackrf_cancel_read(void *ctx)
{
    hackrf_dev_t *dev = (hackrf_dev_t *)ctx;
    if (dev)
        __atomic_store_n(&dev->read_cancelled, true, __ATOMIC_RELEASE);
}

static ls_radio_err_t hackrf_recover(void *ctx)
{
    hackrf_dev_t *dev = (hackrf_dev_t *)ctx;
    if (!dev) return LS_RADIO_ERR_INVALID;
    esp_libusb_stream_stop_for(&dev->usb);
    esp_libusb_bulk_teardown_for(&dev->usb);
    esp_err_t error = usb_host_interface_release(dev->usb.client_hdl,
                                                  dev->usb.dev_hdl, 0);
    if (error != ESP_OK) return LS_RADIO_ERR_IO;
    vTaskDelay(pdMS_TO_TICKS(20));
    error = usb_host_interface_claim(dev->usb.client_hdl, dev->usb.dev_hdl,
                                     0, 0);
    return error == ESP_OK ? LS_RADIO_OK : LS_RADIO_ERR_IO;
}

static const ls_radio_driver_ops_t s_hackrf_ops = {
    .iq_configure = hackrf_iq_configure,
    .iq_set_gain = hackrf_iq_set_gain,
    .iq_start = hackrf_iq_start,
    .iq_read = hackrf_iq_read,
    .iq_retune = hackrf_iq_retune,
    .iq_stop = hackrf_iq_stop,
    .cancel_read = hackrf_cancel_read,
    .recover = hackrf_recover,
};

static ls_radio_err_t register_endpoint(hackrf_dev_t *dev)
{
    const ls_radio_endpoint_t endpoint = {
        .endpoint_id = LS_RADIO_ENDPOINT_HACKRF_USB,
        .name = "HackRF USB IQ transceiver (RX only)",
        .capabilities = LS_HACKRF_CAPABILITIES,
        .duplex = LS_HACKRF_DUPLEX,
        .iq_formats = LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
        .frequency_ranges = ls_hackrf_frequency_ranges,
        .frequency_range_count = 1,
        .sample_rate_ranges = ls_hackrf_sample_rate_ranges,
        .sample_rate_range_count = 1,
        .ops = &s_hackrf_ops,
        .driver_ctx = dev,
    };
    return ls_radio_endpoint_register(&endpoint);
}

static void close_device(hackrf_dev_t *dev, bool gone)
{
    if (!dev) return;
    if (!gone) (void)hackrf_iq_stop(dev);
    esp_libusb_note_device_gone(dev->usb.dev_hdl);
    if (dev->usb.dev_hdl) {
        (void)usb_host_interface_release(dev->usb.client_hdl,
                                         dev->usb.dev_hdl, 0);
        (void)usb_host_device_close(dev->usb.client_hdl, dev->usb.dev_hdl);
    }
    free(dev);
}

bool hackrf_adapter_note_removed(usb_device_handle_t device)
{
    hackrf_dev_t *dev = s_dev;
    if (!dev || dev->usb.dev_hdl != device) return false;
    (void)ls_radio_endpoint_unregister(LS_RADIO_ENDPOINT_HACKRF_USB);
    s_dev = NULL;
    close_device(dev, true);
    return true;
}

static void setup_task(void *arg)
{
    hackrf_setup_arg_t *setup = (hackrf_setup_arg_t *)arg;
    hackrf_dev_t *dev = calloc(1, sizeof(*dev));
    if (!dev) goto done;
    dev->usb.client_hdl = setup->client;
    dev->usb.dev_addr = setup->dev_addr;
    if (usb_host_device_open(dev->usb.client_hdl, dev->usb.dev_addr,
                             &dev->usb.dev_hdl) != ESP_OK)
        goto reject;

    const usb_device_desc_t *descriptor = NULL;
    if (usb_host_get_device_descriptor(dev->usb.dev_hdl, &descriptor) != ESP_OK ||
        !descriptor || descriptor->idVendor != HACKRF_USB_VID ||
        !supported_pid(descriptor->idProduct))
        goto reject;
    if (s_dev) {
        ESP_LOGI(TAG, "HackRF endpoint already present; ignoring USB addr %u",
                 setup->dev_addr);
        goto reject;
    }
    if (usb_host_interface_claim(dev->usb.client_hdl, dev->usb.dev_hdl, 0, 0) !=
        ESP_OK)
        goto reject;

    init_adsb_dev();
    s_dev = dev;
    ls_radio_err_t error = LS_RADIO_ERR_BUSY;
    for (int attempt = 0; attempt < 200; ++attempt) {
        error = register_endpoint(dev);
        if (error != LS_RADIO_ERR_BUSY) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (error != LS_RADIO_OK) {
        ESP_LOGE(TAG, "endpoint registration failed: %s",
                 ls_radio_err_name(error));
        s_dev = NULL;
        close_device(dev, false);
        goto done;
    }
    ESP_LOGI(TAG, "registered %s, RX-only half-duplex, 2-20 MSPS",
             LS_RADIO_ENDPOINT_HACKRF_USB);
    goto done;

reject:
    if (dev->usb.dev_hdl)
        (void)usb_host_device_close(dev->usb.client_hdl, dev->usb.dev_hdl);
    free(dev);
done:
    vPortFree(setup);
    vTaskDelete(NULL);
}

void hackrf_adapter_probe_async(uint8_t dev_addr,
                                usb_host_client_handle_t client)
{
    hackrf_setup_arg_t *setup = pvPortMalloc(sizeof(*setup));
    if (!setup) {
        ESP_LOGE(TAG, "setup arg alloc failed");
        return;
    }
    setup->dev_addr = dev_addr;
    setup->client = client;
    if (xTaskCreatePinnedToCore(setup_task, "hackrf_setup", 4096, setup, 4,
                                NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "setup task creation failed");
        vPortFree(setup);
    }
}
