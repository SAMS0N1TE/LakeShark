/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#include "hackrf_adapter_private.h"

#include <inttypes.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "esp_libusb_private.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hackrf_radio.h"
#include "radio_endpoint.h"
#include "settings.h"

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
    HACKRF_REQ_GET_M0_STATE = 41,
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
    uint16_t usb_api;
    /* A rate below the radio's own is served by decimating: the radio runs at
       out_rate_hz * decim and reads pass through the CIC. decim 1 is direct. */
    uint32_t out_rate_hz;
    unsigned decim;
    uint32_t shift_hz;    /* the radio is tuned this far above the centre */
    volatile int ppm;     /* reference error; see settings_hackrf_ppm_get */
    ls_hackrf_cic_t cic;
    uint8_t *raw;
} hackrf_dev_t;

#define HACKRF_RAW_BYTES (32 * 1024)

/* What the last read saw, for 'hackrf' on the console: the radio's own
   samples before decimation and what went out after it. */
typedef struct {
    uint32_t reads, raw_bytes, out_bytes;
    uint32_t overflows;         /* the ring filled before the app read it */
    uint64_t read_us, cic_us;   /* time in the ring read and in the decimator */
    uint32_t timed;
    uint32_t raw_ms;            /* mean square of a component, radio side */
    int32_t raw_dc_i, raw_dc_q; /* mean, thousandths of a step */
    uint32_t out_ms;            /* mean square of a component, app side */
    int lna_db, vga_db;
    bool amp;
} hackrf_stats_t;
static hackrf_stats_t s_stats;
/* 'hackrf dump' asks for the next read's samples either side of the
   decimator; the read copies them into buffers the command holds for the
   length of the dump, and clears the request. */
#define DUMP_RAW (8 * 1024)
#define DUMP_OUT (512 * 1024)
static volatile bool s_dump_req;
static uint8_t *s_dump_raw, *s_dump_out;
static size_t s_dump_raw_n, s_dump_out_n;
/* The last four seconds of what went to the app, while a decimating HackRF
   is open: 'hackrf snap' sends it, so a page another receiver has just
   decoded is in it. */
#define RING (2u * 1024u * 1024u)
static uint8_t *s_ring;
static volatile size_t s_ring_w;
static volatile bool s_ring_full, s_ring_hold;

static void note_block(const int8_t *raw, size_t raw_bytes, const uint8_t *out,
                       size_t out_bytes)
{
    int64_t sq = 0, si = 0, sqq = 0;
    size_t pairs = raw_bytes / 2;
    for (size_t i = 0; i + 1 < raw_bytes; i += 2) {
        sq += raw[i] * raw[i] + raw[i + 1] * raw[i + 1];
        si += raw[i];
        sqq += raw[i + 1];
    }
    int64_t osq = 0;
    for (size_t i = 0; i < out_bytes; ++i) {
        const int v = (int)out[i] - 128;
        osq += v * v;
    }
    s_stats.reads++;
    s_stats.raw_bytes = (uint32_t)raw_bytes;
    s_stats.out_bytes = (uint32_t)out_bytes;
    s_stats.raw_ms = pairs ? (uint32_t)(sq / (2 * (int64_t)pairs)) : 0;
    s_stats.raw_dc_i = pairs ? (int32_t)(si * 1000 / (int64_t)pairs) : 0;
    s_stats.raw_dc_q = pairs ? (int32_t)(sqq * 1000 / (int64_t)pairs) : 0;
    s_stats.out_ms = out_bytes ? (uint32_t)(osq / (int64_t)out_bytes) : 0;
}
/* What AUTO gets: the radio has no AGC, so a middle setting - LNA 24 dB,
   VGA 24 dB, amp off - that hears a VHF whip without overloading on the
   broadcast band. */
#define HACKRF_AUTO_GAIN_TENTHS 480

typedef struct {
    unsigned generation;
    uint8_t dev_addr;
    usb_host_client_handle_t client;
} hackrf_setup_arg_t;

static const char *TAG = "hackrf_dev";
static hackrf_dev_t *s_dev;
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
    if (probe) hackrf_adapter_probe_async(addr, client);
}


static volatile uint32_t s_alloc_fail_count;
static volatile uint32_t s_alloc_fail_size;
static volatile uint32_t s_alloc_fail_caps;

static void IRAM_ATTR note_alloc_failure(size_t size, uint32_t caps,
                                          const char *function)
{
    (void)function;
    __atomic_store_n(&s_alloc_fail_size, (uint32_t)size, __ATOMIC_RELAXED);
    __atomic_store_n(&s_alloc_fail_caps, caps, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_alloc_fail_count, 1, __ATOMIC_RELEASE);
}

static bool supported_pid(uint16_t pid)
{
    return pid == HACKRF_USB_PID_ONE || pid == HACKRF_USB_PID_JAWBREAKER ||
           pid == HACKRF_USB_PID_RAD1O;
}

bool hackrf_adapter_matches(uint16_t vid, uint16_t pid)
{
    return vid == HACKRF_USB_VID && supported_pid(pid);
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
    /* A fast reference puts the radio high by ppm; ask for that much less. */
    uint64_t lo = frequency_hz + dev->shift_hz;
    lo -= (int64_t)lo * dev->ppm / 1000000;
    ls_hackrf_encode_frequency(lo, params);
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
    if (mode == LS_RADIO_GAIN_AUTO) tenths_db = HACKRF_AUTO_GAIN_TENTHS;
    if (tenths_db < 0 || tenths_db > 1130) return LS_RADIO_ERR_UNSUPPORTED;

    bool amp = tenths_db > 1020;
    int remaining = tenths_db - (amp ? 110 : 0);
    /* About half to the LNA (8 dB steps, at most 40) and the rest to the
       VGA (2 dB steps, at most 62). Filling the LNA first left 40 dB as
       LNA 40 + VGA 0, which starves the baseband and reads as a dead radio. */
    int lna = (remaining / 2 / 80) * 80;
    if (lna > 400) lna = 400;
    int vga = ((remaining - lna) / 20) * 20;
    if (vga > 620) {
        vga = 620;
        lna = ((remaining - vga) / 80) * 80;
        if (lna > 400) lna = 400;
    }

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
    s_stats.lna_db = lna / 10;
    s_stats.vga_db = vga / 10;
    s_stats.amp = amp;
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

    const unsigned decim = ls_hackrf_decimation(requested->sample_rate_hz);
    if (decim == 0) return LS_RADIO_ERR_UNSUPPORTED;
    if (decim > 1 && !dev->raw) {
        dev->raw = heap_caps_malloc(HACKRF_RAW_BYTES,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!dev->raw) return LS_RADIO_ERR_NO_MEMORY;
    }
    if (decim > 1 && !s_ring) {
        /* Diagnostics only: without it, 'hackrf snap' has nothing to send. */
        s_ring_w = 0;
        s_ring_full = false;
        s_ring = heap_caps_malloc(RING, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    const uint32_t hw_rate = requested->sample_rate_hz * decim;
    ls_radio_err_t error = set_sample_rate(dev, hw_rate);
    if (error != LS_RADIO_OK) return error;
    dev->decim = decim;
    dev->out_rate_hz = requested->sample_rate_hz;
    dev->shift_hz = decim > 1 ? ls_hackrf_shift(decim) * requested->sample_rate_hz : 0;
    if (decim > 1) {
        ls_hackrf_cic_init(&dev->cic, decim, ls_hackrf_shift(decim));
        dev->cic.agc = true;
    }
    /* The analog filter is set for the radio's rate; the CIC does the rest. */
    uint32_t bandwidth = ls_hackrf_filter_bandwidth(
        decim > 1 ? 0 : requested->bandwidth_hz, hw_rate);
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
    actual->sample_rate_hz = dev->out_rate_hz;
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
    /* Console output can block longer than the IQ ring covers at high rates.
     * Print before enabling RX so startup logging cannot overflow the ring. */
    ESP_LOGI(TAG, "RX needs %" PRIu32 " B/s; 256 KiB ring covers %.3f ms",
             dev->sample_rate_hz * 2,
             262144000.0 / (dev->sample_rate_hz * 2));
    dev->dropped_at_start = esp_libusb_stream_dropped();
    if (dev->decim > 1) {
        /* The AGC keeps its level across a restart or retune; the filter
           state does not survive the gap in the stream. */
        const int32_t level = dev->cic.agc_q8;
        ls_hackrf_cic_init(&dev->cic, dev->decim, ls_hackrf_shift(dev->decim));
        dev->cic.agc = true;
        dev->cic.agc_q8 = level > 0 ? level : 256;
    }
    ls_radio_err_t error = control_out(dev, HACKRF_REQ_SET_TRANSCEIVER_MODE,
                                       HACKRF_MODE_RECEIVE, 0, NULL, 0);
    if (error != LS_RADIO_OK) {
        esp_libusb_stream_stop_for(&dev->usb);
        return error;
    }
    dev->streaming = true;
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
        const bool decimating = dev->decim > 1 && dev->raw;
        size_t want = decimating ? bytes * dev->decim : bytes;
        if (decimating && want > HACKRF_RAW_BYTES) want = HACKRF_RAW_BYTES;
        int max_bytes = want > INT_MAX ? INT_MAX : (int)want;
        const int64_t t0 = esp_timer_get_time();
        int count = esp_libusb_stream_read_timeout(
            decimating ? dev->raw : (uint8_t *)dst, max_bytes,
            timeout_ms == 0 ? 0 : 1);
        if (count > 0) s_stats.read_us += (uint64_t)(esp_timer_get_time() - t0);
        if (count < 0) return LS_RADIO_ERR_STOPPED;
        if (count == 0) {
            if (timeout_ms == 0 || ++waited_ms >= timeout_ms)
                return LS_RADIO_ERR_TIMEOUT;
            continue;
        }
        if (esp_libusb_stream_dropped() != dev->dropped_at_start) {
            /* The app fell behind and samples were lost. Say so once, as
               an error, so it knows the stream has a gap; then take the
               stream up again from here. Refusing every read after one
               overflow left a receiver dead for good. */
            dev->dropped_at_start = esp_libusb_stream_dropped();
            if (dev->decim > 1) {
                const int32_t level = dev->cic.agc_q8;
                ls_hackrf_cic_init(&dev->cic, dev->decim, ls_hackrf_shift(dev->decim));
                dev->cic.agc = true;
                dev->cic.agc_q8 = level;
            }
            if (s_stats.overflows++ == 0)
                ESP_LOGW(TAG, "IQ ring overflow: the app is reading too slowly; a gap is reported");
            return LS_RADIO_ERR_IO;
        }
        if (decimating) {
            /* Too few samples for one output is not a timeout: read on. */
            const int64_t t1 = esp_timer_get_time();
            const size_t made = ls_hackrf_cic_run(&dev->cic, dev->raw,
                                                  (size_t)count, (uint8_t *)dst);
            s_stats.cic_us += (uint64_t)(esp_timer_get_time() - t1);
            s_stats.timed++;
            if (made == 0) continue;
            if (s_ring && !s_ring_hold) {
                size_t w = s_ring_w, first = made < RING - w ? made : RING - w;
                memcpy(s_ring + w, dst, first);
                memcpy(s_ring, (const uint8_t *)dst + first, made - first);
                w += made;
                if (w >= RING) { w -= RING; s_ring_full = true; }
                s_ring_w = w;
            }
            if (s_dump_req) {
                /* Consecutive reads, until either side is full. */
                size_t r = (size_t)count, o = made;
                if (r > DUMP_RAW - s_dump_raw_n) r = DUMP_RAW - s_dump_raw_n;
                if (o > DUMP_OUT - s_dump_out_n) o = DUMP_OUT - s_dump_out_n;
                memcpy(s_dump_raw + s_dump_raw_n, dev->raw, r);
                memcpy(s_dump_out + s_dump_out_n, dst, o);
                s_dump_raw_n += r;
                s_dump_out_n += o;
                if (s_dump_out_n == DUMP_OUT) s_dump_req = false;
            }
            if ((s_stats.reads & 63) == 0)
                note_block((const int8_t *)dev->raw, (size_t)count, (const uint8_t *)dst, made);
            else
                s_stats.reads++;
            *read_bytes = made;
            return LS_RADIO_OK;
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
    if (dev->decim > 1) {
        /* The AGC keeps its level across a restart or retune; the filter
           state does not survive the gap in the stream. */
        const int32_t level = dev->cic.agc_q8;
        ls_hackrf_cic_init(&dev->cic, dev->decim, ls_hackrf_shift(dev->decim));
        dev->cic.agc = true;
        dev->cic.agc_q8 = level > 0 ? level : 256;
    }
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

static ls_radio_err_t hackrf_iq_get_health(void *ctx, ls_radio_iq_health_t *out)
{
    hackrf_dev_t *dev = ctx;
    if (!dev || !out) return LS_RADIO_ERR_INVALID;
    memset(out, 0, sizeof(*out));
    out->usb_api = dev->usb_api;
    if (dev->usb_api < 0x0106) return LS_RADIO_ERR_UNSUPPORTED;
    if (dev->streaming) return LS_RADIO_ERR_BUSY;
    uint8_t wire[40];
    int n = esp_libusb_control_transfer(&dev->usb, CTRL_IN,
        HACKRF_REQ_GET_M0_STATE, 0, 0, wire, sizeof(wire), HACKRF_CONTROL_TIMEOUT_MS);
    if (!ls_hackrf_decode_m0_state(wire, n > 0 ? (size_t)n : 0, out)) {
        out->usb_api = dev->usb_api;
        return LS_RADIO_ERR_IO;
    }
    out->usb_api = dev->usb_api;
    return LS_RADIO_OK;
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
    .iq_get_health = hackrf_iq_get_health,
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
    heap_caps_free(dev->raw);
    heap_caps_free(s_ring);
    s_ring = NULL;
    free(dev);
}

bool hackrf_adapter_note_removed(usb_device_handle_t device)
{
    portENTER_CRITICAL(&s_lifecycle);
    ++s_remove_generation;
    hackrf_dev_t *dev = s_dev;
    bool matches = dev && dev->usb.dev_hdl == device;
    if (matches) { s_dev = NULL; s_cleanup_active = true; }
    portEXIT_CRITICAL(&s_lifecycle);
    esp_libusb_note_device_gone(device);
    if (!matches) return false;
    (void)ls_radio_endpoint_unregister(LS_RADIO_ENDPOINT_HACKRF_USB);
    close_device(dev, true);
    finish_lifecycle(false);
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
        !descriptor || !hackrf_adapter_matches(descriptor->idVendor,
                                               descriptor->idProduct))
        goto reject;
    dev->usb_api = descriptor->bcdDevice;
    if (s_dev) {
        ESP_LOGI(TAG, "HackRF endpoint already present; ignoring USB addr %u",
                 setup->dev_addr);
        goto reject;
    }
    uint32_t failures = __atomic_load_n(&s_alloc_fail_count, __ATOMIC_ACQUIRE);
    esp_err_t claim = usb_host_interface_claim(dev->usb.client_hdl,
                                                dev->usb.dev_hdl, 0, 0);
    if (claim != ESP_OK) {
        ESP_LOGE(TAG, "interface claim: %s; internal=%u DMA=%u PSRAM=%u",
                 esp_err_to_name(claim), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        if (__atomic_load_n(&s_alloc_fail_count, __ATOMIC_ACQUIRE) != failures)
            ESP_LOGE(TAG, "allocation failed: bytes=%lu caps=0x%lx",
                     (unsigned long)s_alloc_fail_size, (unsigned long)s_alloc_fail_caps);
        goto reject;
    }

    if (init_adsb_dev() != ESP_OK) { close_device(dev, false); goto done; }
    dev->ppm = settings_hackrf_ppm_get();
    ls_radio_err_t error = LS_RADIO_ERR_BUSY;
    for (int attempt = 0; attempt < 200; ++attempt) {
        portENTER_CRITICAL(&s_lifecycle);
        bool cancelled = setup->generation != s_remove_generation;
        portEXIT_CRITICAL(&s_lifecycle);
        if (cancelled) break;
        error = register_endpoint(dev);
        if (error != LS_RADIO_ERR_BUSY) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    portENTER_CRITICAL(&s_lifecycle);
    bool publish = error == LS_RADIO_OK && setup->generation == s_remove_generation;
    if (publish) s_dev = dev;
    portEXIT_CRITICAL(&s_lifecycle);
    if (!publish) {
        if (error == LS_RADIO_OK) (void)ls_radio_endpoint_unregister(LS_RADIO_ENDPOINT_HACKRF_USB);
        ESP_LOGE(TAG, "endpoint registration failed: %s",
                 ls_radio_err_name(error));
        close_device(dev, false);
        goto done;
    }
    ESP_LOGI(TAG, "registered %s, RX-only half-duplex, 2-20 MSPS, lower by decimation",
             LS_RADIO_ENDPOINT_HACKRF_USB);
    goto done;

reject:
    if (dev->usb.dev_hdl)
        (void)usb_host_device_close(dev->usb.client_hdl, dev->usb.dev_hdl);
    free(dev);
done:
    finish_lifecycle(true);
    vPortFree(setup);
    vTaskDelete(NULL);
}

void hackrf_adapter_probe_async(uint8_t dev_addr,
                                usb_host_client_handle_t client)
{
    heap_caps_register_failed_alloc_callback(note_alloc_failure);
    hackrf_setup_arg_t *setup = pvPortMalloc(sizeof(*setup));
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
    if (xTaskCreatePinnedToCore(setup_task, "hackrf_setup", 4096, setup, 4,
                                NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "setup task creation failed");
        finish_lifecycle(true);
        vPortFree(setup);
    }
}

/* The console's view of the HackRF: rates, gain, and the levels either
   side of the decimator. */
void ls_hackrf_print_status(void)
{
    hackrf_dev_t *dev = s_dev;
    if (!dev) { printf("hackrf: not attached\n"); return; }
    /* The radio's own count of samples it had to drop because nobody
       collected them in time - the gaps the host never sees. */
    if (dev->usb_api >= 0x0106) {
        uint8_t wire[40];
        ls_radio_iq_health_t m0;
        int n = esp_libusb_control_transfer(&dev->usb, CTRL_IN, HACKRF_REQ_GET_M0_STATE,
                                            0, 0, wire, sizeof(wire), HACKRF_CONTROL_TIMEOUT_MS);
        if (ls_hackrf_decode_m0_state(wire, n > 0 ? (size_t)n : 0, &m0))
            printf("m0: shortfalls %lu  longest %lu  bytes in %lu  out %lu\n",
                   (unsigned long)m0.num_shortfalls, (unsigned long)m0.longest_shortfall,
                   (unsigned long)m0.m0_count, (unsigned long)m0.m4_count);
        else
            printf("m0: no answer (%d)\n", n);
    }
    printf("hackrf: %s  radio %lu S/s  out %lu S/s  decim %u  shift %lu Hz  centre %llu Hz\n",
           dev->streaming ? "streaming" : "idle", (unsigned long)dev->sample_rate_hz,
           (unsigned long)dev->out_rate_hz, dev->decim, (unsigned long)dev->shift_hz,
           (unsigned long long)dev->center_hz);
    printf("ppm: %d  ring overflows %lu\n", dev->ppm, (unsigned long)s_stats.overflows);
    if (s_stats.timed)
        printf("per read: ring %llu us  decimator %llu us  (%lu reads)\n",
               (unsigned long long)(s_stats.read_us / s_stats.timed),
               (unsigned long long)(s_stats.cic_us / s_stats.timed), (unsigned long)s_stats.timed);
    printf("gain: lna %d vga %d amp %s  (asked %d.%d dB)\n", s_stats.lna_db, s_stats.vga_db,
           s_stats.amp ? "on" : "off", dev->gain_tenths_db / 10, dev->gain_tenths_db % 10);
    printf("radio side: mean_square %lu  dc_milli %ld/%ld  (%lu bytes)\n",
           (unsigned long)s_stats.raw_ms, (long)s_stats.raw_dc_i, (long)s_stats.raw_dc_q,
           (unsigned long)s_stats.raw_bytes);
    printf("app side:   mean_square %lu  agc x%ld.%02ld  env %ld  (%lu bytes, %lu reads)\n",
           (unsigned long)s_stats.out_ms, (long)(dev->cic.agc_q8 / 256),
           (long)(dev->cic.agc_q8 % 256 * 100 / 256), (long)dev->cic.agc_env,
           (unsigned long)s_stats.out_bytes, (unsigned long)s_stats.reads);
}

/* `n` bytes of `buf` (a ring of `size`) from `start`, as the screenshot is
   sent: base64 lines starting '~' between a begin and an end that carries
   the size and CRC-32, so tools/ls_console.py can take it whole or know it
   did not. */
static void emit(const uint8_t *buf, size_t size, size_t start, size_t n)
{
    static const char B64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= buf[(start + i) % size];
        for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    crc ^= 0xFFFFFFFFu;
    printf("hackrf: png begin iq %u\n", (unsigned)n);
    char line[80];
    unsigned lines = 0;
    for (size_t i = 0; i < n; i += 57) {
        const size_t m = n - i < 57 ? n - i : 57;
        size_t o = 0;
        uint8_t b[57];
        for (size_t k = 0; k < m; ++k) b[k] = buf[(start + i + k) % size];
        line[o++] = '~';
        for (size_t k = 0; k < m; k += 3) {
            const uint32_t v = (uint32_t)b[k] << 16 | (k + 1 < m ? (uint32_t)b[k + 1] << 8 : 0) |
                               (k + 2 < m ? b[k + 2] : 0);
            line[o++] = B64[v >> 18 & 63];
            line[o++] = B64[v >> 12 & 63];
            line[o++] = k + 1 < m ? B64[v >> 6 & 63] : '=';
            line[o++] = k + 2 < m ? B64[v & 63] : '=';
        }
        line[o] = 0;
        printf("%s\n", line);
        if (++lines % 32 == 0) vTaskDelay(pdMS_TO_TICKS(30));   /* let the UART drain */
    }
    printf("hackrf: png end iq %u bytes crc %08lx\n", (unsigned)n, (unsigned long)crc);
}

/* One read's samples either side of the decimator, as hex lines
   "raw <hex>" (signed I/Q from the radio) and "out <hex>" (offset binary). */
void ls_hackrf_dump(void)
{
    if (!s_dev || !s_dev->streaming) { printf("hackrf: not streaming\n"); return; }
    s_dump_raw = heap_caps_malloc(DUMP_RAW, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_dump_out = heap_caps_malloc(DUMP_OUT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_dump_raw || !s_dump_out) {
        printf("hackrf: no memory for a dump\n");
    } else {
        s_dump_raw_n = s_dump_out_n = 0;
        s_dump_req = true;
        for (int i = 0; i < 400 && s_dump_req; ++i) vTaskDelay(pdMS_TO_TICKS(10));
        if (s_dump_req) {
            s_dump_req = false;
            vTaskDelay(pdMS_TO_TICKS(20));      /* a read in progress finishes */
            printf("hackrf: dump did not fill in four seconds\n");
        } else {
            for (size_t i = 0; i < s_dump_raw_n; i += 64) {
                printf("raw ");
                for (size_t k = i; k < i + 64 && k < s_dump_raw_n; ++k) printf("%02x", s_dump_raw[k]);
                printf("\n");
            }
            emit(s_dump_out, s_dump_out_n, 0, s_dump_out_n);
        }
    }
    heap_caps_free(s_dump_raw);
    heap_caps_free(s_dump_out);
    s_dump_raw = s_dump_out = NULL;
}

/* The last four seconds the app was given, oldest first. */
void ls_hackrf_snap(void)
{
    if (!s_ring) {
        printf("hackrf: nothing recorded; the ring runs while a HackRF streams below 2 MSPS\n");
        return;
    }
    s_ring_hold = true;
    vTaskDelay(pdMS_TO_TICKS(20));          /* a read in progress finishes */
    const size_t n = s_ring_full ? RING : s_ring_w;
    emit(s_ring, RING, s_ring_full ? s_ring_w : 0, n);
    s_ring_hold = false;
}

/* The reference correction, now and saved; the next tune uses it. */
void ls_hackrf_set_ppm(int ppm)
{
    if (ppm < -200 || ppm > 200) { printf("hackrf: ppm is -200 to 200\n"); return; }
    settings_hackrf_ppm_set(ppm);
    if (s_dev) {
        s_dev->ppm = ppm;
        if (s_dev->center_hz) (void)set_frequency(s_dev, s_dev->center_hz);
    }
    printf("hackrf: ppm %d saved%s\n", ppm, s_dev ? ", retuned" : "");
}
