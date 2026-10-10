/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#include "rtlsdr_dev.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_libusb_private.h"
#include "esp_log.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "radio_endpoint.h"
#include "ls_task_reap.h"
#include "radio_health.h"
#include "rtl-sdr.h"
#include "rtl_adapter_private.h"
#include "rtl_sdr_private.h"
/**/
#include "usb_host.h"

static const char *TAG = "rtlsdr_dev";
static rtlsdr_dev_t *s_dev;
/* Setup owns its private object until publication. Removal never waits for
 * setup (the USB client must remain able to deliver control completions). */
static portMUX_TYPE s_lifecycle = portMUX_INITIALIZER_UNLOCKED;
static bool s_setup_active, s_cleanup_active;
static bool s_probe_pending;
static uint8_t s_probe_addr;
static usb_host_client_handle_t s_probe_client;
static unsigned s_probe_generation;
static unsigned s_remove_generation;

/* Keep at most the latest replacement probe while the old owner drains. */
static void finish_lifecycle(bool setup)
{
    portENTER_CRITICAL(&s_lifecycle);
    if (setup) s_setup_active = false;
    else s_cleanup_active = false;
    bool probe = !s_setup_active && !s_cleanup_active && !s_dev &&
                 s_probe_pending && s_probe_generation == s_remove_generation;
    uint8_t addr = s_probe_addr;
    usb_host_client_handle_t client = s_probe_client;
    if (probe || s_probe_generation != s_remove_generation) s_probe_pending = false;
    portEXIT_CRITICAL(&s_lifecycle);
    if (probe) rtl_adapter_probe_async(addr, client);
}


static volatile bool s_read_cancelled;
static volatile bool s_adapter_streaming;

typedef struct {
    unsigned generation;
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
    /* No s_dev check: teardown, the one caller, has already claimed and
       cleared it, so a check here skipped the unregister and left the old
       endpoint standing. The next attach then failed with "exists" and
       every app talked to a device that was gone. */
    /* present is cleared before cancellation, so a read that races a
     * USB detach can only complete as DISCONNECTED and teardown cannot free
     * the rtlsdr_dev_t until that read has left the adapter. */
    (void)ls_radio_endpoint_unregister(LS_RADIO_ENDPOINT_RTL_USB);
}

bool rtl_adapter_note_removed(usb_device_handle_t device)
{
    portENTER_CRITICAL(&s_lifecycle);
    ++s_remove_generation;
    rtlsdr_dev_t *dev = s_dev;
    bool matches = dev && rtlsdr_usb_device_handle(dev) == device;
    if (matches) { s_dev = NULL; s_cleanup_active = true; }
    portEXIT_CRITICAL(&s_lifecycle);
    esp_libusb_note_device_gone(device);
    if (!matches) return false;
    rtl_unregister_endpoint();
    rtlsdr_stream_stop_for(dev);
    __atomic_store_n(&s_adapter_streaming, false, __ATOMIC_RELEASE);
    rtlsdr_close(dev);
    finish_lifecycle(false);
    return true;
}

void rtl_adapter_note_transport_fault(void)
{
    radio_health_note_fault(LS_RADIO_ENDPOINT_RTL_USB);
}

/**/
void rtlsdr_dev_teardown(void)
{
    /* Claimed, not read. A software port reset and a real unplug can
       now both arrive here at once, and two callers that each read s_dev
       would both close and free the same device; the loser of the exchange
       returns. Clearing it before the drain rather than after changes
       nothing else: the ops reach the device through their own context
       pointer, and a probe that sees NULL early waits on endpoint
       registration, as it would during an ordinary attach. */
    portENTER_CRITICAL(&s_lifecycle);
    ++s_remove_generation;
    rtlsdr_dev_t *dev = s_dev;
    s_dev = NULL;
    if (dev) s_cleanup_active = true;
    portEXIT_CRITICAL(&s_lifecycle);
    if (!dev) return;

    rtl_unregister_endpoint();
    /* On a requested/simulated detach the USB device is still reachable, so
     * stop and drain its transfer pool before rtlsdr_close marks handles gone.
     * On a physical removal note_removed already cleared the handles and this
     * becomes a transport-local no-op. */
    rtlsdr_stream_stop_for(dev);
    __atomic_store_n(&s_adapter_streaming, false, __ATOMIC_RELEASE);
    rtlsdr_close(dev);
    ESP_LOGW(TAG, "device object released");
    finish_lifecycle(false);
}

/* The software stand-in for a replug on a board with no VBUS switch.

   Observed on the T-Display-P4, 2026-09-11, firmware 1.0.3-g951f9933: the
   dongle dropped off USB and came back six times in eighteen minutes, and
   after the seventh attach every demod register read and write failed. FM
   retried its open every ~150 ms and P25 showed RX stopped until an
   app-only flash rebooted the board - with no replug, and the boot log's
   root port found the R820T after a run of "Root port reset failed". A bus
   reset was enough; VBUS never dropped.

   Order is the detach-safe teardown, because the device is still on the bus: free while its
   handle is valid (teardown unregisters, drains the transfer pool with
   halt/flush/clear, then closes), and only then take the port away. Cutting
   the port first would make this the real-unplug path, which nulls the
   handle before the drain. With nothing holding the device open, the host
   library frees it the moment the port drops, and the power-on after that
   enumerates it afresh through the usual probe. */
usb_port_cycle_result_t rtl_adapter_port_reset(void)
{
    /* Except when the dongle has a control transfer the host still owns -
       one that timed out on our side, from a device that stopped answering.
       IDF cannot cancel it and asserts if the device is closed under it, and
       that is the device most likely to be reset. So that case takes the
       real-unplug order on purpose: the port goes first, the host retires
       the transfer as the device leaves (its callback runs before DEV_GONE
       is delivered), and rtl_adapter_note_removed then tears down a device
       with nothing in flight. */
    portENTER_CRITICAL(&s_lifecycle);
    const usb_device_handle_t hdl = s_dev ? rtlsdr_usb_device_handle(s_dev) : NULL;
    portEXIT_CRITICAL(&s_lifecycle);
    if (hdl) {
        const bool locked = esp_libusb_ctrl_lock(3000);
        const bool stuck = esp_libusb_ctrl_pending(hdl);
        if (locked) esp_libusb_ctrl_unlock();
        if (stuck) {
            ESP_LOGW(TAG, "port reset: a control transfer is stuck on the "
                          "dongle - dropping the port first, as an unplug");
            return usb_host_root_port_cycle();
        }
    }
    rtlsdr_dev_teardown();
    return usb_host_root_port_cycle();
}

bool rtl_adapter_port_reset_possible(void)
{
    return usb_host_root_port_has_device();
}

static void rtlsdr_setup_task(void *arg)
{
    setup_arg_t *setup = (setup_arg_t *)arg;
    rtlsdr_dev_t *dev = NULL;
    bool registered = false;
    int error = rtlsdr_open(&dev, setup->dev_addr, setup->client);
    if (error < 0) goto done;
    rtlsdr_set_freq_correction(dev, 0);
    rtlsdr_reset_buffer(dev);
    for (int attempt = 0; attempt < 200; ++attempt) {
        portENTER_CRITICAL(&s_lifecycle);
        bool cancelled = setup->generation != s_remove_generation;
        portEXIT_CRITICAL(&s_lifecycle);
        if (cancelled) break;
        ls_radio_err_t result = rtl_register_endpoint(dev);
        if (result == LS_RADIO_OK) { registered = true; break; }
        if (result != LS_RADIO_ERR_BUSY) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    portENTER_CRITICAL(&s_lifecycle);
    bool publish = registered && setup->generation == s_remove_generation;
    if (publish) { s_dev = dev; dev = NULL; }
    portEXIT_CRITICAL(&s_lifecycle);
    if (dev) {
        if (registered) rtl_unregister_endpoint();
        rtlsdr_close(dev);
    }
    if (publish) event_bus_publish_simple(EVT_TUNER_LOCKED, "rtlsdr");
done:
    finish_lifecycle(true);
    vPortFree(setup);
    ls_task_retire_self();
}

void rtl_adapter_probe_async(uint8_t dev_addr,
                             usb_host_client_handle_t client)
{
    setup_arg_t *setup = pvPortMalloc(sizeof(*setup));
    if (!setup) {
        ESP_LOGE(TAG, "setup arg alloc failed");
        return;
    }
    portENTER_CRITICAL(&s_lifecycle);
    if (s_setup_active || s_cleanup_active) {
        s_probe_pending = true;
        s_probe_addr = dev_addr;
        s_probe_client = client;
        s_probe_generation = s_remove_generation;
        portEXIT_CRITICAL(&s_lifecycle);
        vPortFree(setup);
        return;
    }
    if (s_dev) {
        portEXIT_CRITICAL(&s_lifecycle);
        vPortFree(setup);
        return;
    }
    s_setup_active = true;
    setup->generation = s_remove_generation;
    portEXIT_CRITICAL(&s_lifecycle);
    setup->dev_addr = dev_addr;
    setup->client = client;
    /* Opening an RTL device is deliberately asynchronous, but its temporary
       8 KB setup stack used to come from scarce DMA-capable internal RAM.
       When Wi-Fi and MeshCore were correctly initialized before USB, that
       stack left no block for endpoint publication and enumeration ended in
       LS_RADIO_ERR_NO_MEMORY.  The setup path does not retain stack-backed
       USB buffers; put this short-lived worker in PSRAM like the other
       bounded radio workers and keep internal RAM for the USB endpoint.
       Created with caps, so every exit is ls_task_retire_self: a plain
       vTaskDelete never frees the TCB (internal) or stack, and each attach
       would keep both, while deleting itself would need internal RAM at
       the moment of the attach, when there is least of it. */
    if (xTaskCreatePinnedToCoreWithCaps(rtlsdr_setup_task, "rtlsdr_setup", 8192,
            setup, 4, NULL, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "setup task create failed");
        finish_lifecycle(true);
        vPortFree(setup);
    }
}
