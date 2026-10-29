/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
/* See radio_frontend.h. What is saved, what is in force, and the worker
   that carries a choice made on the UI task to the USB device. */
#include "radio_frontend.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings.h"
#include "ls_textpack.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
#define LOCK()   portENTER_CRITICAL(&s_mux)
#define UNLOCK() portEXIT_CRITICAL(&s_mux)
#else
#define LOCK()   ((void)0)
#define UNLOCK() ((void)0)
#endif

/* The RTL driver's GPIO read-back for `bias status`, where the driver is
   built in; the LR edition and the bench leave it out. */
int rtl_adapter_bias_regs(uint8_t *gpo, uint8_t *gpoe, uint8_t *gpd) __attribute__((weak));

static const char *const ENDPOINT[LS_RFFE_DEVS] = { LS_RADIO_ENDPOINT_RTL_USB, LS_RADIO_ENDPOINT_HACKRF_USB };
static const char *const NAME[LS_RFFE_DEVS] = { "RTL-SDR", "HackRF" };
/* What each kind of device has, attached or not. Offset tuning is the
   RTL's E4000-only path and direct sampling the HF work's; the driver's
   own fe_ctls narrows this to what the one in the socket answers. */
static const uint32_t HAS[LS_RFFE_DEVS] = {
    LS_RADIO_FE_BIT(LS_RADIO_FE_BIAS_T) | LS_RADIO_FE_BIT(LS_RADIO_FE_PPM) |
    LS_RADIO_FE_BIT(LS_RADIO_FE_DIGITAL_AGC) | LS_RADIO_FE_BIT(LS_RADIO_FE_OFFSET_TUNING) |
    LS_RADIO_FE_BIT(LS_RADIO_FE_DIRECT_SAMPLING),
    LS_RADIO_FE_BIT(LS_RADIO_FE_BIAS_T) | LS_RADIO_FE_BIT(LS_RADIO_FE_PPM) |
    LS_RADIO_FE_BIT(LS_RADIO_FE_AMP) | LS_RADIO_FE_BIT(LS_RADIO_FE_LNA_DB) |
    LS_RADIO_FE_BIT(LS_RADIO_FE_VGA_DB),
};

static ls_rffe_cfg_t s_cfg;
static bool s_cfg_loaded;
static int s_bias[LS_RFFE_DEVS];
static int s_ds;                    /* direct sampling in force; never saved */
static uint32_t s_rtl_key;          /* the last RTL-SDR attached, 0 none     */
static int s_hackrf_ppm;            /* as the driver last reported it        */

static bool s_note;
static char s_note_title[20], s_note_body[128];

/* ------------------------------------------------------- stored forms -- */

void ls_rffe_cfg_default(ls_rffe_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->lna_db = -1;
    c->vga_db = -1;
}

#define RFFE_VERSION 1u

uint32_t ls_rffe_pack(const ls_rffe_cfg_t *c)
{
    const uint32_t lna = c->lna_db < 0 ? 0 : (uint32_t)(c->lna_db / 8 + 1);
    const uint32_t vga = c->vga_db < 0 ? 0 : (uint32_t)(c->vga_db / 2 + 1);
    return (c->dagc & 3u) | (c->offset ? 1u << 2 : 0) |
           (c->pin[LS_RFFE_RTL] ? 1u << 3 : 0) | (c->pin[LS_RFFE_HACKRF] ? 1u << 4 : 0) |
           ((c->amp & 3u) << 5) | ((lna & 7u) << 8) | ((vga & 63u) << 11) |
           (RFFE_VERSION << 28);
}

bool ls_rffe_unpack(uint32_t w, ls_rffe_cfg_t *c)
{
    ls_rffe_cfg_default(c);
    if (w >> 28 != RFFE_VERSION) return false;
    bool clean = true;
    const unsigned dagc = w & 3u, amp = (w >> 5) & 3u, lna = (w >> 8) & 7u, vga = (w >> 11) & 63u;
    if (dagc <= 2) c->dagc = (uint8_t)dagc; else clean = false;
    c->offset = (w >> 2) & 1u;
    c->pin[LS_RFFE_RTL] = (w >> 3) & 1u;
    c->pin[LS_RFFE_HACKRF] = (w >> 4) & 1u;
    if (amp <= 2) c->amp = (uint8_t)amp; else clean = false;
    if (lna <= 6) c->lna_db = lna ? (int8_t)((lna - 1) * 8) : -1; else clean = false;
    if (vga <= 32) c->vga_db = vga ? (int8_t)((vga - 1) * 2) : -1; else clean = false;
    return clean;
}

/* FNV-1a over the three strings with a separator, folded to 1..255. Two
   dongles with the same strings share a correction: rtl_eeprom -s gives
   each its own serial. */
uint32_t ls_rffe_dongle_key(const char *m, const char *p, const char *s)
{
    uint32_t h = 2166136261u;
    const char *parts[3] = { m ? m : "", p ? p : "", s ? s : "" };
    for (int i = 0; i < 3; ++i) {
        for (const char *c = parts[i]; *c; ++c) { h ^= (uint8_t)*c; h *= 16777619u; }
        h ^= 0x1f; h *= 16777619u;
    }
    return h % 255u + 1u;
}

bool ls_rffe_ppm_get(uint64_t table, uint32_t key, int *ppm)
{
    if (!key || key > 255) return false;
    for (int i = 0; i < 4; ++i) {
        const uint16_t slot = (uint16_t)(table >> (16 * i));
        if ((slot >> 8) == key) {
            if (ppm) *ppm = (int8_t)(slot & 0xff);
            return true;
        }
    }
    return false;
}

uint64_t ls_rffe_ppm_put(uint64_t table, uint32_t key, int ppm)
{
    if (!key || key > 255) return table;
    if (ppm > LS_RFFE_RTL_PPM_MAX) ppm = LS_RFFE_RTL_PPM_MAX;
    if (ppm < -LS_RFFE_RTL_PPM_MAX) ppm = -LS_RFFE_RTL_PPM_MAX;
    /* Newest first: the others keep their order, the oldest falls off. */
    uint64_t out = (uint64_t)((key << 8) | (uint8_t)(int8_t)ppm);
    int n = 1;
    for (int i = 0; i < 4 && n < 4; ++i) {
        const uint16_t slot = (uint16_t)(table >> (16 * i));
        if (!(slot >> 8) || (slot >> 8) == key) continue;
        out |= (uint64_t)slot << (16 * n++);
    }
    return out;
}

/* ------------------------------------------------------------ names -- */

const char *ls_rffe_endpoint(ls_rffe_dev_t d) { return d < LS_RFFE_DEVS ? ENDPOINT[d] : ""; }
const char *ls_rffe_name(ls_rffe_dev_t d) { return d < LS_RFFE_DEVS ? NAME[d] : "?"; }

bool ls_rffe_from_endpoint(const char *id, ls_rffe_dev_t *out)
{
    for (int d = 0; id && d < LS_RFFE_DEVS; ++d)
        if (!strcmp(id, ENDPOINT[d])) { if (out) *out = (ls_rffe_dev_t)d; return true; }
    return false;
}

bool ls_rffe_from_word(const char *w, ls_rffe_dev_t *out)
{
    if (!w) return false;
    ls_rffe_dev_t d;
    if (!strcmp(w, "rtl") || !strcmp(w, "rtlsdr") || !strcmp(w, "rtl-sdr")) d = LS_RFFE_RTL;
    else if (!strcmp(w, "hackrf") || !strcmp(w, "hrf")) d = LS_RFFE_HACKRF;
    else return false;
    if (out) *out = d;
    return true;
}

/* ------------------------------------------------------------ state -- */

static void load(void)
{
    if (s_cfg_loaded) return;
    ls_rffe_unpack(settings_get_rffe(), &s_cfg);
    s_cfg_loaded = true;
}

static void save_cfg(void)
{
    (void)settings_set_rffe(ls_rffe_pack(&s_cfg));
}

static void notice(const char *title, const char *body)
{
    LOCK();
    snprintf(s_note_title, sizeof(s_note_title), "%s", title);
    snprintf(s_note_body, sizeof(s_note_body), "%s", body);
    s_note = true;
    UNLOCK();
}

bool ls_rffe_take_notice(char *title, size_t tn, char *body, size_t bn)
{
    if (!__atomic_load_n(&s_note, __ATOMIC_ACQUIRE)) return false;
    LOCK();
    const bool had = s_note;
    if (had) {
        if (title && tn) snprintf(title, tn, "%s", s_note_title);
        if (body && bn) snprintf(body, bn, "%s", s_note_body);
        s_note = false;
    }
    UNLOCK();
    return had;
}

bool ls_rffe_has(ls_rffe_dev_t d, ls_radio_fe_ctl_t ctl)
{
    return d < LS_RFFE_DEVS && (unsigned)ctl < LS_RADIO_FE_CTLS && (HAS[d] & LS_RADIO_FE_BIT(ctl));
}

bool ls_rffe_supported(ls_rffe_dev_t d, ls_radio_fe_ctl_t ctl)
{
    if (!ls_rffe_has(d, ctl)) return false;
    ls_radio_endpoint_info_t info;
    if (ls_radio_endpoint_try_get(ENDPOINT[d], &info) != LS_RADIO_OK || !info.present) return false;
    return (info.fe_ctls & LS_RADIO_FE_BIT(ctl)) != 0;
}

const char *ls_rffe_check(ls_radio_fe_ctl_t ctl, int v)
{
    switch (ctl) {
    case LS_RADIO_FE_BIAS_T:
    case LS_RADIO_FE_OFFSET_TUNING: return v == 0 || v == 1 ? NULL : "on or off";
    case LS_RADIO_FE_PPM:           return v >= -LS_RFFE_RTL_PPM_MAX && v <= LS_RFFE_RTL_PPM_MAX ? NULL : "ppm is -100 to 100";
    case LS_RADIO_FE_DIGITAL_AGC:   return v >= 0 && v <= 2 ? NULL : "follow, on or off";
    case LS_RADIO_FE_AMP:           return v >= 0 && v <= 2 ? NULL : "auto, off or on";
    case LS_RADIO_FE_LNA_DB:        return v == -1 || (v >= 0 && v <= 40 && v % 8 == 0) ? NULL : "LNA is auto or 0 to 40 dB in 8s";
    case LS_RADIO_FE_VGA_DB:        return v == -1 || (v >= 0 && v <= 62 && v % 2 == 0) ? NULL : "VGA is auto or 0 to 62 dB in 2s";
    case LS_RADIO_FE_DIRECT_SAMPLING: return v >= 0 && v <= 2 ? NULL : "off, I or Q";
    default: return "no such control";
    }
}

static int rtl_ppm_saved(void)
{
    int ppm = 0;
    return s_rtl_key && ls_rffe_ppm_get(settings_get_rtl_ppm_table(), s_rtl_key, &ppm) ? ppm : 0;
}

int ls_rffe_saved(ls_rffe_dev_t d, ls_radio_fe_ctl_t ctl)
{
    load();
    if (d >= LS_RFFE_DEVS) return 0;
    switch (ctl) {
    case LS_RADIO_FE_BIAS_T:        return s_cfg.pin[d] ? 1 : 0;
    case LS_RADIO_FE_PPM:           return d == LS_RFFE_RTL ? rtl_ppm_saved() : s_hackrf_ppm;
    case LS_RADIO_FE_DIGITAL_AGC:   return s_cfg.dagc;
    case LS_RADIO_FE_OFFSET_TUNING: return s_cfg.offset;
    case LS_RADIO_FE_AMP:           return s_cfg.amp;
    case LS_RADIO_FE_LNA_DB:        return s_cfg.lna_db;
    case LS_RADIO_FE_VGA_DB:        return s_cfg.vga_db;
    default:                        return 0;
    }
}

int ls_rffe_value(ls_rffe_dev_t d, ls_radio_fe_ctl_t ctl)
{
    if (d >= LS_RFFE_DEVS) return 0;
    if (ctl == LS_RADIO_FE_BIAS_T) return __atomic_load_n(&s_bias[d], __ATOMIC_ACQUIRE);
    if (ctl == LS_RADIO_FE_DIRECT_SAMPLING) return d == LS_RFFE_RTL ? s_ds : 0;
    return ls_rffe_saved(d, ctl);
}

void ls_rffe_rtl_attach(uint32_t key) { __atomic_store_n(&s_rtl_key, key, __ATOMIC_RELEASE); }
bool ls_rffe_rtl_known(void) { return __atomic_load_n(&s_rtl_key, __ATOMIC_ACQUIRE) != 0; }

void ls_rffe_note(ls_rffe_dev_t d, ls_radio_fe_ctl_t ctl, int value, bool pinned_on)
{
    if (d >= LS_RFFE_DEVS) return;
    if (ctl == LS_RADIO_FE_BIAS_T) __atomic_store_n(&s_bias[d], value ? 1 : 0, __ATOMIC_RELEASE);
    else if (ctl == LS_RADIO_FE_PPM && d == LS_RFFE_HACKRF) s_hackrf_ppm = value;
    else if (ctl == LS_RADIO_FE_DIRECT_SAMPLING && d == LS_RFFE_RTL) s_ds = value;
    if (ctl == LS_RADIO_FE_BIAS_T && value && pinned_on) {
        char body[128];
        LS_SNPRINTF(t_radio_frontend_s_bias_t_on, body, sizeof(body), "%s bias-T on at plug-in (pinned). %s", NAME[d], LS_RFFE_BIAS_WARNING);
        notice("BIAS-T ON", body);
    }
}

/* ------------------------------------------------------ set & apply -- */

/* Keep what is kept. False when there is nowhere to keep it. */
static bool save(ls_rffe_dev_t d, ls_radio_fe_ctl_t ctl, int v)
{
    load();
    switch (ctl) {
    case LS_RADIO_FE_PPM:
        if (d == LS_RFFE_HACKRF) { s_hackrf_ppm = v; settings_hackrf_ppm_set(v); return true; }
        if (!s_rtl_key) return false;
        return settings_set_rtl_ppm_table(ls_rffe_ppm_put(settings_get_rtl_ppm_table(), s_rtl_key, v));
    case LS_RADIO_FE_DIGITAL_AGC:   s_cfg.dagc = (uint8_t)v; break;
    case LS_RADIO_FE_OFFSET_TUNING: s_cfg.offset = v != 0; break;
    case LS_RADIO_FE_AMP:           s_cfg.amp = (uint8_t)v; break;
    case LS_RADIO_FE_LNA_DB:        s_cfg.lna_db = (int8_t)v; break;
    case LS_RADIO_FE_VGA_DB:        s_cfg.vga_db = (int8_t)v; break;
    default: return true;           /* bias-T and direct sampling: not saved */
    }
    save_cfg();
    return true;
}

static ls_radio_err_t apply(ls_rffe_dev_t d, ls_radio_fe_ctl_t ctl, int v)
{
    int value = v;
    ls_radio_err_t err = ls_radio_endpoint_fe(ENDPOINT[d], ctl, true, &value);
    if (err != LS_RADIO_OK) {
        /* A failed switch-on leaves it as the device has it: off. */
        if (ctl == LS_RADIO_FE_BIAS_T) __atomic_store_n(&s_bias[d], 0, __ATOMIC_RELEASE);
        return err;
    }
    if (ctl == LS_RADIO_FE_BIAS_T) {
        __atomic_store_n(&s_bias[d], v ? 1 : 0, __ATOMIC_RELEASE);
        if (v) {
            char body[128];
            snprintf(body, sizeof(body), "%s: %s", NAME[d], LS_RFFE_BIAS_WARNING);
            notice("BIAS-T ON", body);
        }
    } else if (ctl == LS_RADIO_FE_DIRECT_SAMPLING) {
        s_ds = v;
    } else if (ctl == LS_RADIO_FE_AMP && v == 2) {
        notice("RF AMP ON", LS_TEXT(t_radio_frontend_hackrf_amp_forced_on, "HackRF amp forced on, +11 dB: a strong signal or a transmitter "
                            "nearby can damage the input. Keep it for weak signals."));
    }
    return LS_RADIO_OK;
}

ls_radio_err_t ls_rffe_set(ls_rffe_dev_t d, ls_radio_fe_ctl_t ctl, int v)
{
    if (!ls_rffe_has(d, ctl) || ls_rffe_check(ctl, v)) return LS_RADIO_ERR_INVALID;
    const bool live_only = ctl == LS_RADIO_FE_BIAS_T || ctl == LS_RADIO_FE_DIRECT_SAMPLING;
    ls_radio_endpoint_info_t info;
    const bool present = ls_radio_endpoint_get(ENDPOINT[d], &info) == LS_RADIO_OK && info.present;
    if (!present && live_only) return LS_RADIO_ERR_UNAVAILABLE;
    if (!save(d, ctl, v)) return LS_RADIO_ERR_UNAVAILABLE;
    if (!present) return LS_RADIO_ERR_UNAVAILABLE;
    return apply(d, ctl, v);
}

#ifdef ESP_PLATFORM
typedef struct { uint8_t d, ctl; int16_t value; } req_t;
static QueueHandle_t s_q;
static StaticQueue_t s_q_ctrl;
static uint8_t s_q_store[8 * sizeof(req_t)];

static void worker(void *arg)
{
    (void)arg;
    req_t r;
    for (;;)
        if (xQueueReceive(s_q, &r, portMAX_DELAY) == pdTRUE)
            (void)apply((ls_rffe_dev_t)r.d, (ls_radio_fe_ctl_t)r.ctl, r.value);
}

static bool worker_ready(void)
{
    static volatile int state;      /* 0 none, 1 starting, 2 up */
    int expected = 0;
    if (__atomic_compare_exchange_n(&state, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        s_q = xQueueCreateStatic(8, sizeof(req_t), s_q_store, &s_q_ctrl);
        /* Its stack in PSRAM: it only sends control transfers, which copy
           their data into the USB layer's own buffers. */
        const bool ok = s_q && xTaskCreatePinnedToCoreWithCaps(worker, "rffe", 6144, NULL, 3, NULL, 0,
                                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
        __atomic_store_n(&state, ok ? 2 : 0, __ATOMIC_RELEASE);
        return ok;
    }
    while (__atomic_load_n(&state, __ATOMIC_ACQUIRE) == 1) vTaskDelay(1);
    return __atomic_load_n(&state, __ATOMIC_ACQUIRE) == 2;
}
#endif

bool ls_rffe_request(ls_rffe_dev_t d, ls_radio_fe_ctl_t ctl, int v)
{
    if (!ls_rffe_has(d, ctl) || ls_rffe_check(ctl, v)) return false;
    const bool live_only = ctl == LS_RADIO_FE_BIAS_T || ctl == LS_RADIO_FE_DIRECT_SAMPLING;
    if (live_only && !ls_rffe_supported(d, ctl)) return false;
    if (!save(d, ctl, v)) return false;
    ls_radio_endpoint_info_t info;
    if (ls_radio_endpoint_try_get(ENDPOINT[d], &info) == LS_RADIO_OK && !info.present)
        return true;                /* kept for the next attach */
    if (ctl == LS_RADIO_FE_BIAS_T) __atomic_store_n(&s_bias[d], v ? 1 : 0, __ATOMIC_RELEASE);
#ifdef ESP_PLATFORM
    req_t r = { (uint8_t)d, (uint8_t)ctl, (int16_t)v };
    return worker_ready() && xQueueSend(s_q, &r, 0) == pdTRUE;
#else
    return apply(d, ctl, v) == LS_RADIO_OK;
#endif
}

int ls_rffe_gains(ls_rffe_dev_t d, int *tenths, int max)
{
    return d < LS_RFFE_DEVS ? ls_radio_endpoint_fe_gains(ENDPOINT[d], tenths, max) : 0;
}

/* ------------------------------------------------------------ bias-T -- */

bool ls_radio_bias_t_supported(const char *id)
{
    ls_rffe_dev_t d;
    return ls_rffe_from_endpoint(id, &d) && ls_rffe_supported(d, LS_RADIO_FE_BIAS_T);
}

ls_radio_err_t ls_radio_bias_t_set(const char *id, bool on)
{
    ls_rffe_dev_t d;
    if (!ls_rffe_from_endpoint(id, &d)) return LS_RADIO_ERR_INVALID;
    return ls_rffe_set(d, LS_RADIO_FE_BIAS_T, on ? 1 : 0);
}

ls_radio_err_t ls_radio_bias_t_get(const char *id, bool *on)
{
    ls_rffe_dev_t d;
    if (!on || !ls_rffe_from_endpoint(id, &d)) return LS_RADIO_ERR_INVALID;
    int value = 0;
    ls_radio_err_t err = ls_radio_endpoint_fe(id, LS_RADIO_FE_BIAS_T, false, &value);
    if (err == LS_RADIO_OK) {
        *on = value != 0;
        __atomic_store_n(&s_bias[d], *on ? 1 : 0, __ATOMIC_RELEASE);
    }
    return err;
}

bool ls_radio_bias_t_state(const char *id)
{
    ls_rffe_dev_t d;
    return ls_rffe_from_endpoint(id, &d) && __atomic_load_n(&s_bias[d], __ATOMIC_ACQUIRE);
}

void ls_radio_bias_t_pin(const char *id, bool pinned)
{
    ls_rffe_dev_t d;
    if (!ls_rffe_from_endpoint(id, &d)) return;
    load();
    if (s_cfg.pin[d] == pinned) return;
    s_cfg.pin[d] = pinned;
    save_cfg();
}

bool ls_radio_bias_t_pinned(const char *id)
{
    ls_rffe_dev_t d;
    if (!ls_rffe_from_endpoint(id, &d)) return false;
    load();
    return s_cfg.pin[d];
}

/* ----------------------------------------------------------- console -- */

static const char *errname(ls_radio_err_t e) { return ls_radio_err_name(e); }

static void bias_status(ls_rffe_dev_t d)
{
    ls_radio_endpoint_info_t info;
    const bool present = ls_radio_endpoint_get(ENDPOINT[d], &info) == LS_RADIO_OK && info.present;
    printf("%s bias-T: ", NAME[d]);
    if (!present) {
        printf("not attached%s\n", ls_radio_bias_t_pinned(ENDPOINT[d]) ? "  (pinned on at plug-in)" : "");
        return;
    }
    bool on = false;
    ls_radio_err_t err = ls_radio_bias_t_get(ENDPOINT[d], &on);
    if (err != LS_RADIO_OK) printf("unknown (%s)", errname(err));
    else printf("%s", on ? "ON" : "off");
    if (d == LS_RFFE_RTL) {
        uint8_t gpo = 0, gpoe = 0, gpd = 0;
        if (rtl_adapter_bias_regs && rtl_adapter_bias_regs(&gpo, &gpoe, &gpd) == 0)
            LS_PRINTF(t_radio_frontend_gpio0_gpo_0x_02x, "  [GPIO0: GPO 0x%02x GPOE 0x%02x GPD 0x%02x -> out %s, level %s]",
                   gpo, gpoe, gpd, (gpoe & 1) && !(gpd & 1) ? "enabled" : "disabled",
                   gpo & 1 ? "high" : "low");
    } else {
        LS_FPUTS(t_radio_frontend_hackrf_has_no_read, "  [HackRF has no read-back: last command]", stdout);
    }
    printf("%s\n", ls_radio_bias_t_pinned(ENDPOINT[d]) ? "  pinned on at plug-in" : "");
}

/* The device named, or the one attached (the RTL-SDR first). */
static ls_rffe_dev_t pick(int argc, char **argv, int *arg)
{
    ls_rffe_dev_t d;
    if (argc > *arg && ls_rffe_from_word(argv[*arg], &d)) { ++*arg; return d; }
    ls_radio_endpoint_info_t info;
    for (int i = 0; i < LS_RFFE_DEVS; ++i)
        if (ls_radio_endpoint_get(ENDPOINT[i], &info) == LS_RADIO_OK && info.present)
            return (ls_rffe_dev_t)i;
    return LS_RFFE_RTL;
}

int ls_bias_command(int argc, char **argv)
{
    int a = 1;
    if (argc <= 1 || (argc == 2 && !strcmp(argv[1], "status"))) {
        bias_status(LS_RFFE_RTL);
        bias_status(LS_RFFE_HACKRF);
        return 0;
    }
    const ls_rffe_dev_t d = pick(argc, argv, &a);
    const char *verb = argc > a ? argv[a] : "status";
    if (!strcmp(verb, "status")) { bias_status(d); return 0; }
    if (!strcmp(verb, "pin") || !strcmp(verb, "unpin")) {
        ls_radio_bias_t_pin(ENDPOINT[d], verb[0] == 'p');
        LS_PRINTF(t_radio_frontend_s_bias_t_s, "%s bias-T %s at plug-in\n", NAME[d], verb[0] == 'p' ? "comes on" : "stays off");
        return 0;
    }
    if (strcmp(verb, "on") && strcmp(verb, "off")) {
        printf("usage: bias [rtl|hackrf] on|off|status|pin|unpin\n");
        return 1;
    }
    const bool on = !strcmp(verb, "on");
    ls_radio_err_t err = ls_radio_bias_t_set(ENDPOINT[d], on);
    if (err != LS_RADIO_OK) {
        printf("%s bias-T: %s\n", NAME[d], err == LS_RADIO_ERR_UNAVAILABLE ? "not attached" : errname(err));
        return 1;
    }
    if (on) printf("WARNING: %s\n", LS_RFFE_BIAS_WARNING);
    bias_status(d);
    return 0;
}

static void rffe_show(ls_rffe_dev_t d)
{
    static const char *const DAGC[] = { "follow gain", "on", "off" };
    static const char *const AMP[] = { "by gain", "off", "ON" };
    static const char *const DS[] = { "off", "I", "Q" };
    ls_radio_endpoint_info_t info;
    const bool present = ls_radio_endpoint_get(ENDPOINT[d], &info) == LS_RADIO_OK && info.present;
    printf("%s: %s\n", NAME[d], present ? "attached" : "not attached (saved settings shown)");
    printf("  bias-T %s%s\n", ls_rffe_value(d, LS_RADIO_FE_BIAS_T) ? "ON" : "off",
           ls_radio_bias_t_pinned(ENDPOINT[d]) ? ", pinned on at plug-in" : "");
    printf("  ppm %d%s\n", ls_rffe_value(d, LS_RADIO_FE_PPM),
           d == LS_RFFE_RTL ? (s_rtl_key ? "  (this dongle)" : "  (no dongle seen this boot)") : "");
    if (d == LS_RFFE_RTL) {
        printf("  digital AGC %s\n", DAGC[ls_rffe_value(d, LS_RADIO_FE_DIGITAL_AGC) % 3]);
        LS_PRINTF(t_radio_frontend_offset_tuning_s_s, "  offset tuning %s%s\n", ls_rffe_value(d, LS_RADIO_FE_OFFSET_TUNING) ? "on" : "off",
               present && !ls_rffe_supported(d, LS_RADIO_FE_OFFSET_TUNING) ? "  (not this tuner: low IF, no DC spike)" : "");
        LS_PRINTF(t_radio_frontend_direct_sampling_s_s, "  direct sampling %s%s\n", DS[ls_rffe_value(d, LS_RADIO_FE_DIRECT_SAMPLING) % 3],
               ls_rffe_supported(d, LS_RADIO_FE_DIRECT_SAMPLING) ? "" : "  (not in this build)");
    } else {
        const int lna = ls_rffe_value(d, LS_RADIO_FE_LNA_DB), vga = ls_rffe_value(d, LS_RADIO_FE_VGA_DB);
        printf("  amp %s\n", AMP[ls_rffe_value(d, LS_RADIO_FE_AMP) % 3]);
        if (lna < 0) printf("  LNA by gain\n"); else printf("  LNA %d dB fixed\n", lna);
        if (vga < 0) printf("  VGA by gain\n"); else printf("  VGA %d dB fixed\n", vga);
    }
    if (present) {
        int g[64];
        const int n = ls_rffe_gains(d, g, 64);
        if (n > 0) {
            printf("  tuner gains (%d):", n);
            for (int i = 0; i < n; ++i) printf(" %d.%d", g[i] / 10, abs(g[i] % 10));
            printf(" dB\n");
        }
    }
}

static bool parse_int(const char *s, int *out)
{
    if (!s || !*s) return false;
    char *end = NULL;
    const long v = strtol(s, &end, 10);
    if (*end) return false;
    *out = (int)v;
    return true;
}

int ls_rffe_command(int argc, char **argv)
{
    int a = 1;
    if (argc <= 1) { rffe_show(LS_RFFE_RTL); rffe_show(LS_RFFE_HACKRF); return 0; }
    const ls_rffe_dev_t d = pick(argc, argv, &a);
    if (argc <= a) { rffe_show(d); return 0; }
    const char *what = argv[a];
    const char *word = argc > a + 1 ? argv[a + 1] : "";
    ls_radio_fe_ctl_t ctl;
    int v = 0;
    if (!strcmp(what, "gains")) { rffe_show(d); return 0; }
    else if (!strcmp(what, "ppm")) { ctl = LS_RADIO_FE_PPM; if (!parse_int(word, &v)) goto usage; }
    else if (!strcmp(what, "dagc")) {
        ctl = LS_RADIO_FE_DIGITAL_AGC;
        if (!strcmp(word, "follow") || !strcmp(word, "auto")) v = 0;
        else if (!strcmp(word, "on")) v = 1; else if (!strcmp(word, "off")) v = 2; else goto usage;
    } else if (!strcmp(what, "offset")) {
        ctl = LS_RADIO_FE_OFFSET_TUNING;
        if (!strcmp(word, "on")) v = 1; else if (!strcmp(word, "off")) v = 0; else goto usage;
    } else if (!strcmp(what, "ds")) {
        ctl = LS_RADIO_FE_DIRECT_SAMPLING;
        if (!strcmp(word, "off")) v = 0; else if (!strcmp(word, "i")) v = 1;
        else if (!strcmp(word, "q")) v = 2; else goto usage;
    } else if (!strcmp(what, "amp")) {
        ctl = LS_RADIO_FE_AMP;
        if (!strcmp(word, "auto")) v = 0; else if (!strcmp(word, "off")) v = 1;
        else if (!strcmp(word, "on")) v = 2; else goto usage;
    } else if (!strcmp(what, "lna") || !strcmp(what, "vga")) {
        ctl = what[0] == 'l' ? LS_RADIO_FE_LNA_DB : LS_RADIO_FE_VGA_DB;
        if (!strcmp(word, "auto")) v = -1; else if (!parse_int(word, &v)) goto usage;
    } else goto usage;
    if (!ls_rffe_has(d, ctl)) { printf("%s has no %s\n", NAME[d], what); return 1; }
    const char *why = ls_rffe_check(ctl, v);
    if (why) { printf("%s\n", why); return 1; }
    const ls_radio_err_t err = ls_rffe_set(d, ctl, v);
    if (err == LS_RADIO_ERR_UNAVAILABLE)
        LS_PRINTF(t_radio_frontend_s_not_attached_s, "%s not attached; %s\n", NAME[d],
               ctl == LS_RADIO_FE_DIRECT_SAMPLING ? "nothing changed" :
               (ctl == LS_RADIO_FE_PPM && d == LS_RFFE_RTL && !s_rtl_key) ? "no dongle seen yet, not saved"
                                                                         : "saved for when it is");
    else if (err != LS_RADIO_OK) printf("%s %s: %s\n", NAME[d], what, errname(err));
    else if (ctl == LS_RADIO_FE_AMP && v == 2)
        printf("WARNING: amp forced on (+11 dB); strong nearby signals can damage the HackRF input\n");
    rffe_show(d);
    return err == LS_RADIO_OK || err == LS_RADIO_ERR_UNAVAILABLE ? 0 : 1;
usage:
    printf("usage: rffe [rtl|hackrf] [ppm N | dagc follow|on|off | offset on|off | ds off|i|q |\n"
           "             amp auto|on|off | lna auto|0..40 | vga auto|0..62 | gains]\n");
    return 1;
}
