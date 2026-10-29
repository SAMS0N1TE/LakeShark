/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
#include "rtlsdr_dev.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_libusb_private.h"
#include "esp_log.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "radio_endpoint.h"
#include "ls_radio_hf.h"
#include "ls_task_reap.h"
#include "radio_health.h"
#include "radio_frontend.h"
#include "rtl-sdr.h"
#include "rtl_adapter_private.h"
#include "rtl_sdr_private.h"
/**/
#include "usb_host.h"
#include "ls_cold.h"

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
static uint64_t s_fifo_reset_epoch;
static portMUX_TYPE s_provenance_mux=portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    unsigned generation;
    uint8_t dev_addr;
    usb_host_client_handle_t client;
} setup_arg_t;

/* HF from 500 kHz: direct sampling on a plain R820T dongle, the upconverter
   on a Blog V4 (ls_radio_hf.h). Apps that only want VHF/UHF still ask for
   24 MHz up and match, since a range covers any span inside it. */
static const ls_radio_range_t s_frequency_ranges[] = {
    {LS_HF_MIN_HZ, 1766000000},
};
static const ls_radio_range_t s_r828d_frequency_ranges[]={{28800000,1766000000}};

static void rtl_bind_fifo_epoch(bool reset_ok)
{
    ls_radio_iq_transport_t t={0};
    bool known=esp_libusb_stream_evidence(&t);
    portENTER_CRITICAL(&s_provenance_mux);
    s_fifo_reset_epoch=reset_ok && known?t.epoch:0;
    portEXIT_CRITICAL(&s_provenance_mux);
}
static bool rtl_transport_status(void *ctx,ls_radio_iq_transport_t *out)
{
    (void)ctx;
    if(!esp_libusb_stream_evidence(out))return false;
    portENTER_CRITICAL(&s_provenance_mux);
    out->fifo_reset_known=s_fifo_reset_epoch && s_fifo_reset_epoch==out->epoch;
    portEXIT_CRITICAL(&s_provenance_mux);
    return out->known;
}

/* Declared in librtlsdr.c beside the V4 tuner handling. */
int rtlsdr_check_dongle_model(void *dev, const char *manufact_check,
                              const char *product_check);

/* What the adapter last asked of the dongle, so a switch between the tuner
   and direct sampling can put the tuner back as it was: leaving direct
   sampling re-initialises the R820T, which forgets gain and bandwidth. */
static volatile int s_path = LS_RTL_PATH_NONE;
static volatile bool s_blog_v4;
static ls_radio_gain_mode_t s_gain_mode = LS_RADIO_GAIN_AUTO;
static int s_gain_tenths;
static uint32_t s_bw_hz;

static bool rtl_is_blog_v4(rtlsdr_dev_t *dev)
{
    return rtlsdr_check_dongle_model(dev, "RTLSDRBlog", "Blog V4") != 0;
}

static ls_radio_err_t rtl_iq_set_gain(void *ctx, ls_radio_gain_mode_t mode,
                                      int tenths_db, int *actual_tenths_db);

/* Put the dongle on the path `hz` needs. Only a crossing of the 24 MHz line
   on a plain R820T dongle does anything; a V4 and everything above 24 MHz
   stay on the tuner, whose own set_freq handles the V4 upconverter. */
static int rtl_route(rtlsdr_dev_t *dev, uint64_t hz)
{
    const bool v4 = rtl_is_blog_v4(dev);
    s_blog_v4 = v4;
    const ls_rtl_path_t path = ls_rtl_path_for(hz, v4);
    const int want = path == LS_RTL_PATH_DIRECT_Q ? 2 : 0;   /* 2: Q input */
    int error = 0;
    if (rtlsdr_get_direct_sampling(dev) != want) {
        if (want) {
            ESP_LOGI(TAG, "HF: %.3f MHz on direct sampling (Q branch), tuner off",
                     hz / 1e6);
            error = rtlsdr_set_direct_sampling(dev, want);
        } else {
            /* Name the new frequency while still in direct sampling (that
               only moves the DDC), so the tuner comes back up on it rather
               than on an HF frequency its PLL cannot lock. */
            ESP_LOGI(TAG, "HF: back on the tuner at %.3f MHz", hz / 1e6);
            (void)rtlsdr_set_center_freq(dev, (uint32_t)hz);
            error = rtlsdr_set_direct_sampling(dev, 0);
            /* 0 is what FM asks for: the filter follows the sample rate. */
            if (error == 0) error = rtlsdr_set_tuner_bandwidth(dev, s_bw_hz);
            int actual = 0;
            if (error == 0 &&
                rtl_iq_set_gain(dev, s_gain_mode, s_gain_tenths, &actual) != LS_RADIO_OK)
                error = -EIO;
        }
    }
    if (error == 0) s_path = path;
    return error;
}

int rtl_adapter_hf_path(void) { return s_path; }
bool rtl_adapter_blog_v4(void) { return s_blog_v4; }

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

/* Front-end state of the one dongle in the socket (radio_frontend.h). Set
   at attach from what is saved; the registry serializes every change with
   the other control calls. */
static struct {
    bool bias;
    int base_ppm;       /* the user's correction for this dongle          */
    int session_ppm;    /* an app's measured correction (CELL), on top    */
    int dagc;           /* 0 follow the gain mode, 1 on, 2 off            */
    bool offset;
    bool auto_gain;     /* the gain mode last set, for dagc "follow"      */
    bool regs_valid;    /* the GPIO registers as the last read found them */
    uint8_t gpo, gpoe, gpd;
} s_fe;

/* RTL2832U system block GPIO, from the chip's register map: block 2, 0x3001
   GPO (output levels), 0x3003 GPOE (output enables), 0x3004 GPD (direction,
   a set bit an input). RTL-SDR Blog V3/V4 and the dongles that copy them
   switch the bias-T's load switch from GPIO0, high = on. */
#define RTL_BLOCK_SYS 2
#define RTL_REG_GPO   0x3001
#define RTL_REG_GPOE  0x3003
#define RTL_REG_GPD   0x3004
#define RTL_BIAS_BIT  0x01

static int rtl_sys_read(rtlsdr_dev_t *dev, uint16_t reg, uint8_t *v)
{
    return rtlsdr_read_array(dev, RTL_BLOCK_SYS, reg, v, 1) == 1 ? 0 : -1;
}

static int rtl_sys_write(rtlsdr_dev_t *dev, uint16_t reg, uint8_t v)
{
    return rtlsdr_write_reg(dev, RTL_BLOCK_SYS, reg, v, 1) == 1 ? 0 : -1;
}

static int rtl_bias_read(rtlsdr_dev_t *dev, uint8_t *gpo, uint8_t *gpoe, uint8_t *gpd)
{
    if (rtl_sys_read(dev, RTL_REG_GPO, gpo) || rtl_sys_read(dev, RTL_REG_GPOE, gpoe) ||
        rtl_sys_read(dev, RTL_REG_GPD, gpd))
        return -1;
    return 0;
}

static bool rtl_bias_is_on(uint8_t gpo, uint8_t gpoe, uint8_t gpd)
{
    return (gpoe & RTL_BIAS_BIT) && !(gpd & RTL_BIAS_BIT) && (gpo & RTL_BIAS_BIT);
}

/* Level first, then direction and enable, so switching it on never drives
   the old level for a moment; off drives it low and leaves it an output. */
static int rtl_bias_write(rtlsdr_dev_t *dev, bool on)
{
    uint8_t gpo, gpoe, gpd;
    if (rtl_bias_read(dev, &gpo, &gpoe, &gpd)) return -1;
    gpo = on ? (uint8_t)(gpo | RTL_BIAS_BIT) : (uint8_t)(gpo & ~RTL_BIAS_BIT);
    if (rtl_sys_write(dev, RTL_REG_GPO, gpo) ||
        rtl_sys_write(dev, RTL_REG_GPD, (uint8_t)(gpd & ~RTL_BIAS_BIT)) ||
        rtl_sys_write(dev, RTL_REG_GPOE, (uint8_t)(gpoe | RTL_BIAS_BIT)))
        return -1;
    return 0;
}

static int rtl_apply_ppm(rtlsdr_dev_t *dev)
{
    int ppm = s_fe.base_ppm + s_fe.session_ppm;
    if (ppm > 200) ppm = 200;
    if (ppm < -200) ppm = -200;
    int error = rtlsdr_set_freq_correction(dev, ppm);
    return error == -2 ? 0 : error;   /* -2: already in force */
}

/* The RTL2832's own digital AGC: off with a manual tuner gain and on with
   the tuner's AGC, unless the front end says otherwise. */
static int rtl_apply_dagc(rtlsdr_dev_t *dev)
{
    const bool on = s_fe.dagc == 1 || (s_fe.dagc == 0 && s_fe.auto_gain);
    return rtlsdr_set_agc_mode(dev, on);
}

LS_COLD static ls_radio_err_t rtl_iq_set_gain(void *ctx, ls_radio_gain_mode_t mode,
                                      int tenths_db,
                                      int *actual_tenths_db)
{
    rtlsdr_dev_t *dev = (rtlsdr_dev_t *)ctx;
    s_gain_mode = mode;
    s_gain_tenths = tenths_db;
    if (rtlsdr_get_direct_sampling(dev) > 0) {
        /* The tuner is in standby and its gain means nothing; the RTL2832U's
           digital AGC is all there is, so AUTO turns it on. */
        *actual_tenths_db = 0;
        return rtl_error(rtlsdr_set_agc_mode(dev, mode == LS_RADIO_GAIN_AUTO));
    }
    int error = rtlsdr_set_tuner_gain_mode(dev,
                                           mode == LS_RADIO_GAIN_MANUAL);
    if (error != 0) return rtl_error(error);
    s_fe.auto_gain = mode == LS_RADIO_GAIN_AUTO;
    error = rtl_apply_dagc(dev);
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

LS_COLD static ls_radio_err_t rtl_iq_configure(void *ctx,
                                       const ls_radio_iq_config_t *requested,
                                       ls_radio_iq_config_t *actual)
{
    rtlsdr_dev_t *dev = (rtlsdr_dev_t *)ctx;
    if (!dev || requested->center_hz == 0 || requested->sample_rate_hz == 0)
        return LS_RADIO_ERR_INVALID;
    int error = rtlsdr_set_sample_rate(dev, requested->sample_rate_hz);
    if (error != 0) return rtl_error(error);
    s_bw_hz = requested->bandwidth_hz;
    error = rtl_route(dev, requested->center_hz);
    if (error != 0) return rtl_error(error);
    /* Direct sampling has no tuner filter to set. */
    if (rtlsdr_get_direct_sampling(dev) <= 0)
        error = rtlsdr_set_tuner_bandwidth(dev, requested->bandwidth_hz);
    if (error != 0) return rtl_error(error);
    error = rtlsdr_set_center_freq(dev, requested->center_hz);
    if (error != 0) return rtl_error(error);
    /* Offset tuning follows the rate, so it goes on again after one. */
    if (s_fe.offset) (void)rtlsdr_set_offset_tuning(dev, 1);

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

LS_COLD static ls_radio_err_t rtl_iq_start(void *ctx)
{
    rtlsdr_dev_t *dev = (rtlsdr_dev_t *)ctx;
    __atomic_store_n(&s_read_cancelled, false, __ATOMIC_RELEASE);
    rtl_bind_fifo_epoch(false);
    int error=rtlsdr_reset_buffer(dev);
    if(error!=0)return rtl_error(error);
    rtlsdr_stream_reset();
    error = rtlsdr_stream_start(dev);
    if (error == ESP_LIBUSB_ERR_BUSY) return LS_RADIO_ERR_BUSY;
    if (error == ESP_LIBUSB_ERR_NO_MEM) return LS_RADIO_ERR_NO_MEMORY;
    if (error != 0) return rtl_error(error);
    __atomic_store_n(&s_adapter_streaming, true, __ATOMIC_RELEASE);
    rtl_bind_fifo_epoch(true);
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

LS_COLD static ls_radio_err_t rtl_iq_retune(void *ctx, uint64_t center_hz, bool fast,
                                    uint64_t *actual_hz)
{
    rtlsdr_dev_t *dev = (rtlsdr_dev_t *)ctx;
    rtl_bind_fifo_epoch(false);
    bool was_streaming = __atomic_load_n(&s_adapter_streaming,
                                         __ATOMIC_ACQUIRE);
    if (was_streaming) {
        __atomic_store_n(&s_read_cancelled, true, __ATOMIC_RELEASE);
        rtlsdr_stream_stop_for(dev);
        __atomic_store_n(&s_adapter_streaming, false, __ATOMIC_RELEASE);
    }

    /* Name a nonsense tune where it is requested, not three layers down in the tuner. */

    if (center_hz < LS_HF_MIN_HZ || center_hz > 2000000000ull) {
        ESP_LOGE(TAG, "refusing an out-of-range tune: %llu Hz",
                 (unsigned long long)center_hz);
        return LS_RADIO_ERR_INVALID;
    }

    int error = rtl_route(dev, center_hz);
    if (error == 0) error = rtlsdr_set_center_freq(dev, (uint32_t)center_hz);
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
    rtl_bind_fifo_epoch(!fast && was_streaming);
    *actual_hz = rtlsdr_get_center_freq(dev);
    return *actual_hz != 0 ? LS_RADIO_OK : LS_RADIO_ERR_IO;
}

/* An app's correction rides on the user's for the dongle, so CELL's
   calibration measures what is left after the saved one, and its reset to 0
   puts the saved one back rather than wiping it. */
LS_COLD static ls_radio_err_t rtl_iq_set_ppm(void *ctx, int ppm)
{
    rtlsdr_dev_t *dev = (rtlsdr_dev_t *)ctx;
    if (!dev) return LS_RADIO_ERR_INVALID;
    s_fe.session_ppm = ppm;
    return rtl_error(rtl_apply_ppm(dev));
}

static bool rtl_offset_capable(rtlsdr_dev_t *dev)
{
    /* librtlsdr's offset tuning is for the zero-IF tuners; the R820T and
       R828D are low-IF already and have no DC spike to step away from. */
    const enum rtlsdr_tuner t = rtlsdr_get_tuner_type(dev);
    return t == RTLSDR_TUNER_E4000 || t == RTLSDR_TUNER_FC0012 ||
           t == RTLSDR_TUNER_FC0013 || t == RTLSDR_TUNER_FC2580;
}

LS_COLD static ls_radio_err_t rtl_fe_ctl(void *ctx, ls_radio_fe_ctl_t ctl, bool write, int *value)
{
    rtlsdr_dev_t *dev = (rtlsdr_dev_t *)ctx;
    if (!dev || !value) return LS_RADIO_ERR_INVALID;
    int error = 0;
    switch (ctl) {
    case LS_RADIO_FE_BIAS_T:
        if (write) {
            if (rtl_bias_write(dev, *value != 0)) return LS_RADIO_ERR_IO;
            s_fe.bias = *value != 0;
            ESP_LOGW(TAG, "bias-T %s", s_fe.bias ? "ON: DC on the antenna port" : "off");
        } else {
            uint8_t gpo, gpoe, gpd;
            s_fe.regs_valid = false;
            if (rtl_bias_read(dev, &gpo, &gpoe, &gpd)) return LS_RADIO_ERR_IO;
            s_fe.gpo = gpo; s_fe.gpoe = gpoe; s_fe.gpd = gpd;
            s_fe.regs_valid = true;
            *value = rtl_bias_is_on(gpo, gpoe, gpd);
        }
        return LS_RADIO_OK;
    case LS_RADIO_FE_PPM:
        if (!write) { *value = s_fe.base_ppm; return LS_RADIO_OK; }
        s_fe.base_ppm = *value;
        error = rtl_apply_ppm(dev);
        break;
    case LS_RADIO_FE_DIGITAL_AGC:
        if (!write) { *value = s_fe.dagc; return LS_RADIO_OK; }
        s_fe.dagc = *value;
        error = rtl_apply_dagc(dev);
        break;
    case LS_RADIO_FE_OFFSET_TUNING:
        if (!write) { *value = rtlsdr_get_offset_tuning(dev); return LS_RADIO_OK; }
        if (!rtl_offset_capable(dev)) return LS_RADIO_ERR_UNSUPPORTED;
        s_fe.offset = *value != 0;
        error = rtlsdr_set_offset_tuning(dev, s_fe.offset);
        break;
    default:
        /* LS_RADIO_FE_DIRECT_SAMPLING: the HF work answers it here. */
        return LS_RADIO_ERR_UNSUPPORTED;
    }
    return rtl_error(error);
}

static int rtl_fe_gains(void *ctx, int *tenths, int max)
{
    rtlsdr_dev_t *dev = (rtlsdr_dev_t *)ctx;
    if (!dev) return 0;
    const int n = rtlsdr_get_tuner_gains(dev, NULL);
    if (n <= 0 || n > 64) return 0;
    int all[64];
    if (rtlsdr_get_tuner_gains(dev, all) != n) return 0;
    const int k = n < max ? n : max;
    memcpy(tenths, all, (size_t)k * sizeof(int));
    return k;
}

/* `bias status`: the three GPIO registers as the last bias-T read found
   them (radio_frontend reads first, through the registry, which holds the
   device while it does). */
int rtl_adapter_bias_regs(uint8_t *gpo, uint8_t *gpoe, uint8_t *gpd)
{
    if (!s_fe.regs_valid) return -1;
    *gpo = s_fe.gpo; *gpoe = s_fe.gpoe; *gpd = s_fe.gpd;
    return 0;
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
    .iq_set_ppm = rtl_iq_set_ppm,
    .iq_transport_status=rtl_transport_status,
    .fe_ctl = rtl_fe_ctl,
    .fe_gains = rtl_fe_gains,
};

static ls_radio_err_t rtl_register_endpoint(rtlsdr_dev_t *dev)
{
    enum rtlsdr_tuner tuner=rtlsdr_get_tuner_type(dev);
    bool known=tuner==RTLSDR_TUNER_R820T || tuner==RTLSDR_TUNER_R828D;
    uint32_t fe = LS_RADIO_FE_BIT(LS_RADIO_FE_BIAS_T) | LS_RADIO_FE_BIT(LS_RADIO_FE_PPM) |
                  LS_RADIO_FE_BIT(LS_RADIO_FE_DIGITAL_AGC);
    if (rtl_offset_capable(dev)) fe |= LS_RADIO_FE_BIT(LS_RADIO_FE_OFFSET_TUNING);
    const ls_radio_endpoint_t endpoint = {
        .endpoint_id = LS_RADIO_ENDPOINT_RTL_USB,
        .name = "RTL-SDR USB IQ receiver",
        .capabilities = LS_RADIO_RX_IQ_U8,
        .iq_formats = LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
        .frequency_ranges = tuner==RTLSDR_TUNER_R828D?s_r828d_frequency_ranges:s_frequency_ranges,
        .frequency_range_count = 1,
        .tuner_model_known=known,
        .sample_rate_ranges = s_sample_rate_ranges,
        .sample_rate_range_count = sizeof(s_sample_rate_ranges) /
                                   sizeof(s_sample_rate_ranges[0]),
        .ops = &s_rtl_ops,
        .driver_ctx = dev,
        .fe_ctls = fe,
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

LS_COLD bool rtl_adapter_note_removed(usb_device_handle_t device)
{
    portENTER_CRITICAL(&s_lifecycle);
    ++s_remove_generation;
    rtlsdr_dev_t *dev = s_dev;
    bool matches = dev && rtlsdr_usb_device_handle(dev) == device;
    if (matches) { s_dev = NULL; s_cleanup_active = true; }
    portEXIT_CRITICAL(&s_lifecycle);
    esp_libusb_note_device_gone(device);
    if (!matches) return false;
    s_path = LS_RTL_PATH_NONE;
    rtl_unregister_endpoint();
    ls_rffe_note(LS_RFFE_RTL, LS_RADIO_FE_BIAS_T, 0, false);   /* no power, no bias */
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

    s_path = LS_RTL_PATH_NONE;
    rtl_unregister_endpoint();
    /* A software detach or port reset may leave VBUS up: switch the bias-T
       off while the handle still answers, so it never outlives the device
       object that knew it was on. */
    if (s_fe.bias) (void)rtl_bias_write(dev, false);
    s_fe.bias = false;
    ls_rffe_note(LS_RFFE_RTL, LS_RADIO_FE_BIAS_T, 0, false);
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

int rtl_adapter_port_attached(void)
{
    return usb_host_root_port_attached();
}

/* A new dongle starts from what is saved for it: its own PPM, the AGC and
   offset choices, and the bias-T off - forced off if a bus reset without a
   power cut left GPIO0 driving it - unless it is pinned on at plug-in. */
LS_COLD static void rtl_fe_attach(rtlsdr_dev_t *dev)
{
    static char m[256], p[256], s[256];  /* one setup task at a time */
    m[0] = p[0] = s[0] = 0;
    (void)rtlsdr_get_usb_strings(dev, m, p, s);
    const uint32_t key = ls_rffe_dongle_key(m, p, s);
    ls_rffe_rtl_attach(key);
    memset(&s_fe, 0, sizeof(s_fe));
    s_fe.auto_gain = true;
    s_fe.base_ppm = ls_rffe_saved(LS_RFFE_RTL, LS_RADIO_FE_PPM);
    s_fe.dagc = ls_rffe_saved(LS_RFFE_RTL, LS_RADIO_FE_DIGITAL_AGC);
    s_fe.offset = rtl_offset_capable(dev) && ls_rffe_saved(LS_RFFE_RTL, LS_RADIO_FE_OFFSET_TUNING);
    (void)rtl_apply_ppm(dev);
    if (s_fe.offset) (void)rtlsdr_set_offset_tuning(dev, 1);
    uint8_t gpo, gpoe, gpd;
    if (rtl_bias_read(dev, &gpo, &gpoe, &gpd) == 0 && rtl_bias_is_on(gpo, gpoe, gpd)) {
        ESP_LOGW(TAG, "bias-T was left on across a reset: switching it off");
        (void)rtl_bias_write(dev, false);
    }
    const bool pinned = ls_rffe_saved(LS_RFFE_RTL, LS_RADIO_FE_BIAS_T) != 0;
    s_fe.bias = pinned && rtl_bias_write(dev, true) == 0;
    if (s_fe.bias) ESP_LOGW(TAG, "bias-T ON at plug-in (pinned)");
    ESP_LOGI(TAG, "front end: dongle key %u ppm %d dagc %d offset %d bias %d",
             (unsigned)key, s_fe.base_ppm, s_fe.dagc, s_fe.offset, s_fe.bias);
    ls_rffe_note(LS_RFFE_RTL, LS_RADIO_FE_BIAS_T, s_fe.bias, s_fe.bias);
}

LS_COLD static void rtlsdr_setup_task(void *arg)
{
    setup_arg_t *setup = (setup_arg_t *)arg;
    rtlsdr_dev_t *dev = NULL;
    bool registered = false;
    int error = rtlsdr_open(&dev, setup->dev_addr, setup->client);
    if (error < 0) goto done;
    rtl_fe_attach(dev);
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
