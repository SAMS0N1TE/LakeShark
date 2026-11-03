/* The board's radios behind lap_rx_* (see ls_lap_radio.h): the USB SDRs
 * through the endpoint lease (radio_endpoint.h), the LR2021 through its I/Q
 * session (lr_wf_session.h). Every function here runs on the lap_io worker,
 * except the capture task and the LR2021 consumer, which only convert and
 * push into the app queue.
 *
 * Ownership is the board's usual one. The app's own screen has already
 * parked whatever receiver HOME kept (ls_lap.c, radio_hold); a dongle another
 * job still holds refuses the lease (BUSY), and the LR2021 is taken from
 * MeshCore with the same hold FALLS uses and handed back on close. */
#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
#include "ls_lap_radio.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iq_app_control.h"
#include "lr_iq_spectrum.h"
#include "lr_wf_session.h"
#include "ls_field.h"
#include "ls_lora.h"
#include "ls_mixrf.h"
#include "ls_mesh.h"
#include "radio_endpoint.h"
#include "radio_frontend.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define LR_RATE 125000u              /* narrow and wide both hand over 125k IQ16 */
#define READ_BYTES 16384u            /* one SDR read: 8192 u8 pairs */
#define OWNER "lap"

typedef enum { K_NONE, K_RTL, K_HACKRF, K_LORA } kind_t;
static kind_t kinds[LAP_RADIOS_MAX];     /* worker: index -> radio, from the last list */
static kind_t open_kind;

/* ---- the list ----------------------------------------------------------- */

static int sdr_entry(lap_radio_info_t *r, const char *id, uint8_t kind, const char *name) {
    ls_radio_endpoint_info_t info;
    if (ls_radio_endpoint_try_get(id, &info) != LS_RADIO_OK) return 0;
    memset(r, 0, sizeof(*r));
    snprintf(r->name, sizeof(r->name), "%s", name);
    r->kind = kind;
    r->features = LAP_RXF_IQ | LAP_RXF_SPECTRUM | LAP_RXF_GAIN | LAP_RXF_RATE;
    r->present = info.present;
    r->busy = info.leased && strcmp(info.owner, OWNER);
    r->min_hz = info.frequency_range_count ? info.frequency_ranges[0].min_hz : 0;
    r->max_hz = info.frequency_range_count ? info.frequency_ranges[info.frequency_range_count - 1].max_hz : 0;
    r->min_rate = info.sample_rate_range_count ? (uint32_t)info.sample_rate_ranges[0].min_hz : 0;
    r->max_rate = info.sample_rate_range_count ? (uint32_t)info.sample_rate_ranges[info.sample_rate_range_count - 1].max_hz : 0;
    r->gain_min = 0;
    r->gain_max = kind == LAP_RADIO_KIND_HACKRF ? 1130 : 496;   /* radio_choice.h's limits */
    if (!r->present) snprintf(r->why, sizeof(r->why), "unplugged");
    else if (r->busy) snprintf(r->why, sizeof(r->why), "in use by %.20s", info.owner);
    return 1;
}
static int hw_list(lap_radio_info_t *out, int max) {
    int n = 0;
    memset(kinds, 0, sizeof(kinds));
    if (n < max && sdr_entry(&out[n], LS_RADIO_ENDPOINT_RTL_USB, LAP_RADIO_KIND_RTL, "RTL-SDR")) kinds[n++] = K_RTL;
    if (n < max && sdr_entry(&out[n], LS_RADIO_ENDPOINT_HACKRF_USB, LAP_RADIO_KIND_HACKRF, "HackRF")) kinds[n++] = K_HACKRF;
    if (n < max && ls_lora_fitted()!=LS_LORA_CHIP_NONE) {
        lap_radio_info_t *r = &out[n];
        memset(r, 0, sizeof(*r));
        const uint32_t caps = ls_lora_fitted_caps();
        r->present = true;
        if (ls_lora_fitted() == LS_LORA_CHIP_LR20XX) {
            snprintf(r->name, sizeof(r->name), "LR2021");
            r->kind = LAP_RADIO_KIND_LR2021;
            r->features = LAP_RXF_IQ | LAP_RXF_SPECTRUM | LAP_RXF_WIDE;
            r->min_hz = ls_lora_rx_min_hz(caps);
            r->max_hz = (caps & LS_LORA_CAP_RX_EXT) ? LS_LORA_RX_EXT_MAX_HZ : ls_lora_rx_lf_max_hz(caps);
            r->min_rate = r->max_rate = LR_RATE;
            /* Someone else streaming (FM or P25 on the LR, FALLS) shows as a live spectrum centre. */
            r->busy = lr_spec_center() != 0 && open_kind != K_LORA;
            if (r->busy) snprintf(r->why, sizeof(r->why), "another receiver streams");
        } else {
            snprintf(r->name, sizeof(r->name), "%.15s", ls_lora_fitted_name());
            r->kind = LAP_RADIO_KIND_SX1262;
            snprintf(r->why, sizeof(r->why), "packets only, no I/Q");
        }
        kinds[n++] = K_LORA;
    }
    ls_mixrf_status_t mix;ls_mixrf_snapshot(&mix);
    if(n<max&&mix.cc){lap_radio_info_t *r=&out[n++];memset(r,0,sizeof(*r));r->kind=LAP_RADIO_KIND_MIXRF;r->present=true;r->busy=mix.cc_owner!=LS_MIXRF_OWNER_NONE;r->min_hz=300000000;r->max_hz=928000000;snprintf(r->name,sizeof(r->name),"MIX-RF CC1101");snprintf(r->why,sizeof(r->why),"edge capture; no I/Q");}
    return n;
}

/* ---- the USB SDRs -------------------------------------------------------- */

/* One capture task does everything that touches USB - the lease, configure,
   start, retune, gain, rate, read, stop and release - on an internal stack
   (the lap_io worker's is PSRAM). The worker only starts it, watches its
   phase, and asks it to stop.

   Each task owns one block (cap_t) with its flags and buffers. A task still
   inside USB when the worker gives up on it (1.5 s) is left that block: it
   frees the block and deletes itself when it gets out, and never pushes
   after it was asked to stop, so a new session never shares anything with
   it. */
enum { PH_NONE = 0, PH_STARTING = 1, PH_RUNNING = 2 };   /* or a negative LAP_RX_E* */
enum { CAP_RUNNING = 0, CAP_DONE = 1, CAP_LEFT = 2 };
typedef struct {
    kind_t kind;
    uint32_t token;
    lap_rx_config_t want;
    ls_iq_control_t control; /* stays with a late USB task after close */
    atomic_bool stop;
    atomic_int state;                    /* CAP_*: who frees this block */
    atomic_int phase;
    atomic_uint rate, new_rate;          /* what runs, and a change asked for */
    atomic_int gain;
    atomic_int command_state; /* 0 idle, 1 queued, 2 completed */
    int command_control, command_result;
    int64_t command_value;
    uint64_t command_center;

    uint8_t raw[READ_BYTES];             /* u8 IQ as read */
    int16_t iq[READ_BYTES];              /* the same as int16 */
} cap_t;
static struct {
    TaskHandle_t task;
    cap_t *cap;                          /* PSRAM; the running task's block */
} sdr;

static int sdr_err(ls_radio_err_t e) {
    switch (e) {
    case LS_RADIO_ERR_BUSY: case LS_RADIO_ERR_EXISTS: return LAP_RX_EBUSY;
    case LS_RADIO_ERR_UNAVAILABLE: case LS_RADIO_ERR_DISCONNECTED: return LAP_RX_ENODEV;
    case LS_RADIO_ERR_UNSUPPORTED: return LAP_RX_EUNSUPPORTED;
    case LS_RADIO_ERR_INVALID: return LAP_RX_ERANGE;
    case LS_RADIO_ERR_NO_MEMORY: return LAP_RX_ENOMEM;
    default: return LAP_RX_EIO;
    }
}
/* Capture task: configure (or reconfigure) and start streaming. */
static ls_radio_err_t sdr_start(cap_t *c, ls_radio_session_t *session, uint64_t center, uint32_t rate, int gain) {
    ls_radio_iq_config_t want = {
        .center_hz = center, .sample_rate_hz = rate, .bandwidth_hz = rate,
        .gain_mode = gain == LAP_GAIN_AUTO ? LS_RADIO_GAIN_AUTO : LS_RADIO_GAIN_MANUAL,
        .gain_tenths_db = gain == LAP_GAIN_AUTO ? 0 : gain,
    }, got;
    ls_radio_err_t e = ls_iq_control_configure(&c->control, session, &want, &got);
    if (e == LS_RADIO_OK) e = ls_radio_iq_start(session);
    if (e != LS_RADIO_OK) return e;
    if (atomic_load(&c->stop)) return LS_RADIO_ERR_DISCONNECTED;
    atomic_store(&c->rate, got.sample_rate_hz);
    atomic_store(&c->gain, got.gain_mode == LS_RADIO_GAIN_AUTO ? LAP_GAIN_AUTO : got.gain_tenths_db);
    ls_lap_rx_gain_token(c->token,atomic_load(&c->gain));
    ls_lap_rx_mark_token(c->token,got.center_hz, got.sample_rate_hz, LAP_IQ_GAP | LAP_IQ_RETUNED);
    return LS_RADIO_OK;
}
static void capture(void *arg) {
    cap_t *const c = arg;
    ls_radio_session_t *session = NULL;
    const ls_radio_requirements_t req = {
        .required_caps = LS_RADIO_RX_IQ_U8, .min_hz = c->want.center_hz, .max_hz = c->want.center_hz,
        .sample_rate_hz = c->want.rate_hz, .iq_format = LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
        .preferred_endpoint_id = c->kind == K_RTL ? LS_RADIO_ENDPOINT_RTL_USB : LS_RADIO_ENDPOINT_HACKRF_USB,
    };
    ls_radio_err_t e = ls_radio_acquire(OWNER, &req, &session);
    if (e != LS_RADIO_OK) { session = NULL; goto out; }
    ls_iq_control_reset(&c->control);
    ls_iq_control_set_initial(&c->control, c->want.center_hz,
                              c->want.gain_tenths_db == LAP_GAIN_AUTO ? 0 : c->want.gain_tenths_db, c->want.rate_hz);
    e = sdr_start(c, session, c->want.center_hz, c->want.rate_hz, c->want.gain_tenths_db);
    if (e != LS_RADIO_OK) goto out;
    if (atomic_load(&c->stop)) goto out;
    atomic_store(&c->phase, PH_RUNNING);
    uint64_t center = c->want.center_hz;
    while (!atomic_load(&c->stop)) {
        ls_iq_control_request_t r;
        if (ls_iq_control_take(&c->control, &r)) {
            if (r.flags & LS_IQ_CONTROL_TUNE) {
                if (ls_iq_control_apply_tune(&c->control, session, &r) == LS_RADIO_OK && !atomic_load(&c->stop)) {
                    ls_iq_control_status_t status;
                    ls_iq_control_status(&c->control, &status);
                    center = status.effective_center_hz;
                    ls_lap_rx_mark_token(c->token,center, 0, LAP_IQ_RETUNED);
                }
            }
            if (r.flags & LS_IQ_CONTROL_GAIN) {
                if (ls_iq_control_apply_gain(&c->control, session, &r) == LS_RADIO_OK && !atomic_load(&c->stop)) {
                    ls_iq_control_status_t status;
                    ls_iq_control_status(&c->control,&status);
                    int applied=r.gain_auto ? LAP_GAIN_AUTO : status.effective_gain_tenths_db;
                    atomic_store(&c->gain,applied); ls_lap_rx_gain_token(c->token,applied);
                }
                if (!atomic_load(&c->stop)) ls_lap_rx_mark_token(c->token,0, 0, LAP_IQ_RETUNED);
            }
        }
        if (atomic_load(&c->stop)) break;
        if (atomic_load_explicit(&c->command_state, memory_order_acquire) == 1) {
            const int ctl = c->command_control;
            const int64_t value = c->command_value;
            ls_radio_err_t ce = LS_RADIO_ERR_UNSUPPORTED;
            if (ctl == LAP_RX_CTL_TUNE) {
                uint64_t got = 0;
                ce = ls_radio_iq_retune(session, (uint64_t)value, false, &got);
                if (ce == LS_RADIO_OK) { center = got; c->command_center = got; }
            } else if (ctl == LAP_RX_CTL_GAIN) {
                int got = 0;
                ce = ls_radio_iq_set_gain(session, value == LAP_GAIN_AUTO ? LS_RADIO_GAIN_AUTO : LS_RADIO_GAIN_MANUAL, value == LAP_GAIN_AUTO ? 0 : (int)value, &got);
                if (ce == LS_RADIO_OK && !atomic_load(&c->stop)) { atomic_store(&c->gain, value == LAP_GAIN_AUTO ? LAP_GAIN_AUTO : got); ls_lap_rx_gain_token(c->token,atomic_load(&c->gain)); }
            } else if (ctl == LAP_RX_CTL_RATE) {
                ce = ls_radio_iq_stop(session);
                if (ce == LS_RADIO_OK) ce = sdr_start(c, session, center, (uint32_t)value, atomic_load(&c->gain));
            } else if (ctl < LS_RADIO_FE_CTLS) {
                ce = ls_rffe_set(c->kind == K_RTL ? LS_RFFE_RTL : LS_RFFE_HACKRF, (ls_radio_fe_ctl_t)ctl, (int)value);
            } else if (ctl == LAP_RX_CTL_ANTENNA && value == 0) ce = LS_RADIO_OK;
            if (atomic_load(&c->stop)) break;
            c->command_result = ce == LS_RADIO_OK ? 0 : sdr_err(ce);
            if (ce == LS_RADIO_OK) ls_lap_rx_mark_token(c->token,center, atomic_load(&c->rate), LAP_IQ_RETUNED | LAP_IQ_GAP);
            /* Keep the legacy effective snapshot in sync with successful tune. */
            if (ctl == LAP_RX_CTL_TUNE && ce == LS_RADIO_OK)
                ls_iq_control_note_tune_result(&c->control, center, LS_RADIO_OK, center);
            atomic_store_explicit(&c->command_state, 2, memory_order_release);
        }
        const unsigned rate = atomic_exchange(&c->new_rate, 0u);
        if (rate && rate != atomic_load(&c->rate)) {
            ls_radio_iq_stop(session);
            e = sdr_start(c, session, center, rate, atomic_load(&c->gain));
            if (e != LS_RADIO_OK) break;
        }
        size_t got = 0;
        e = ls_radio_iq_read(session, c->raw, READ_BYTES, 50, &got);
        if (e != LS_RADIO_OK && e != LS_RADIO_ERR_TIMEOUT) break;
        e = LS_RADIO_OK;
        /* Asked to stop while inside the read: the session may be gone. */
        if (atomic_load(&c->stop)) break;
        got &= ~(size_t)1;
        /* Offset binary u8 (both dongles hand it over so) to signed full scale. */
        for (size_t i = 0; i < got; i++) c->iq[i] = (int16_t)(((int)c->raw[i] * 2 - 255) * 128);
        if (got) ls_lap_rx_push_token(c->token,c->iq, got / 2);
    }
out:
    if (session) {
        ls_radio_iq_stop(session);
        ls_radio_release(session);
    }
    if (e != LS_RADIO_OK) atomic_store(&c->phase, sdr_err(e));
    int running = CAP_RUNNING;
    if (atomic_compare_exchange_strong(&c->state, &running, CAP_DONE))
        vTaskSuspend(NULL);              /* the worker deletes it and frees the block */
    /* Left behind: nobody else holds the block any more. */
    heap_caps_free(c);
    vTaskDelete(NULL);
}
static void sdr_close(void) {
    cap_t *c = sdr.cap;
    if (sdr.task) {
        atomic_store(&c->stop, true);
        for (int i = 0; i < 300 && atomic_load(&c->state) == CAP_RUNNING; i++) vTaskDelay(pdMS_TO_TICKS(5));
        int running = CAP_RUNNING;
        if (atomic_compare_exchange_strong(&c->state, &running, CAP_LEFT)) {
            c = NULL;                    /* still inside USB past 1.5 s: it frees its own block */
        } else {
            vTaskDelete(sdr.task);
        }
        sdr.task = NULL;
    }
    heap_caps_free(c);
    sdr.cap = NULL;
}
static int sdr_open(kind_t k, const lap_rx_config_t *cfg, ls_lap_rx_actual_t *actual) {
    if (!sdr.task) {
        cap_t *c = heap_caps_calloc(1, sizeof(cap_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!c) return LAP_RX_ENOMEM;
        c->kind = k;
        c->want = *cfg;
        c->token=ls_lap_rx_token();
        atomic_init(&c->stop, false);
        atomic_init(&c->state, CAP_RUNNING);
        atomic_init(&c->new_rate, 0u);
        atomic_init(&c->phase, PH_STARTING);
        sdr.cap = c;
        /* Internal stack: USB must not run on a PSRAM stack. */
        if (xTaskCreatePinnedToCore(capture, "lap_iq", 4096, c, 5, &sdr.task, 0) != pdPASS) {
            sdr.task = NULL;
            sdr_close();
            return LAP_RX_ENOMEM;
        }
        return LAP_RX_EAGAIN;
    }
    const int ph = atomic_load(&sdr.cap->phase);
    if (ph == PH_STARTING) return LAP_RX_EAGAIN;
    if (ph < 0) { sdr_close(); return ph; }
    *actual = (ls_lap_rx_actual_t){cfg->center_hz, atomic_load(&sdr.cap->rate), atomic_load(&sdr.cap->gain)};
    return 0;
}

/* ---- the LR2021 ----------------------------------------------------------- */

static struct {
    bool hold_asked, started;
    lr_wf_wide_status saved;             /* the wide setting before the app, restored on close */
    uint64_t hz;
} lr;

static void lr_consume(void *ctx, const uint8_t *data, size_t bytes, bool spare) {
    uint32_t token=(uint32_t)(uintptr_t)ctx; (void)spare;
    if (!data || !bytes) { ls_lap_rx_mark_token(token,0,0,LAP_IQ_GAP); return; }
    ls_lap_rx_push_token(token,(const int16_t *)data,bytes/4);
}
static void lr_close(void) {
    if (lr.started) {
        lr_wf_stop();
        lr_wf_set_wide(lr.saved.on, lr.saved.offset_hz);
        lr.started = false;
    }
    if (lr.hold_asked) { ls_mesh_radio_hold(false); lr.hold_asked = false; }
}
static int lr_start(uint64_t hz) {
    ls_lap_rx_mark(hz, LR_RATE, LAP_IQ_GAP | LAP_IQ_RETUNED);
    const esp_err_t e = lr_wf_start_consumer((uint32_t)hz,lr_consume,(void *)(uintptr_t)ls_lap_rx_token());
    if (e == ESP_OK) { lr.hz = hz; return 0; }
    return e == ESP_ERR_INVALID_ARG ? LAP_RX_ERANGE : e == ESP_ERR_NO_MEM ? LAP_RX_ENOMEM
         : e == ESP_ERR_NOT_SUPPORTED ? LAP_RX_EUNSUPPORTED : LAP_RX_EBUSY;
}
static int lr_open(const lap_rx_config_t *cfg, ls_lap_rx_actual_t *actual) {
    if (ls_lora_chip() != LS_LORA_CHIP_LR20XX) return LAP_RX_EUNSUPPORTED;
    if (!ls_lora_packet_rx_hz_ok(ls_lora_caps(), (uint32_t)cfg->center_hz)) return LAP_RX_ERANGE;
    if (!lr.hold_asked) {
        if (ls_field_owned() || ls_lora_scanning() || lr_spec_center()) return LAP_RX_EBUSY;
        ls_mesh_radio_hold(true);
        lr.hold_asked = true;
    }
    if (!ls_mesh_radio_held()) return LAP_RX_EAGAIN;          /* MeshCore lets go within a pass or two */
    lr_wf_wide_get(&lr.saved);
    if (lr_wf_set_wide((cfg->flags & LAP_RX_WIDE) != 0,
                       lr.saved.offset_hz ? lr.saved.offset_hz : LR_WF_WIDE_DEFAULT_HZ) != ESP_OK) {
        lr_close();
        return LAP_RX_EINVAL;
    }
    lr.started = true;
    const int rc = lr_start(cfg->center_hz);
    if (rc < 0) { lr_close(); return rc; }
    *actual = (ls_lap_rx_actual_t){cfg->center_hz, LR_RATE, LAP_GAIN_AUTO};
    return 0;
}

/* ---- the backend ---------------------------------------------------------- */

static int hw_open(int radio, const lap_rx_config_t *cfg, ls_lap_rx_actual_t *actual) {
    const kind_t k = radio >= 0 && radio < LAP_RADIOS_MAX ? kinds[radio] : K_NONE;
    int rc = k == K_RTL || k == K_HACKRF ? sdr_open(k, cfg, actual) : k == K_LORA ? lr_open(cfg, actual) : LAP_RX_ENODEV;
    open_kind = rc >= 0 || rc == LAP_RX_EAGAIN ? k : K_NONE;
    return rc;
}
static int hw_tune(uint64_t hz, ls_lap_rx_actual_t *actual) {
    if (open_kind == K_LORA) {
        if (!ls_lora_packet_rx_hz_ok(ls_lora_caps(), (uint32_t)hz)) return LAP_RX_ERANGE;
        lr_wf_stop();                    /* no old block may be tagged with the new centre */
        const int rc = lr_start(hz);
        if (rc == 0) actual->center_hz = hz;
        return rc;
    }
    if (!sdr.cap) return LAP_RX_ENODEV;
    ls_iq_control_request_tune(&sdr.cap->control, hz, false);   /* the capture task applies it */
    actual->center_hz = hz;
    return 0;
}
static int hw_gain(int tenths, ls_lap_rx_actual_t *actual) {
    if (open_kind == K_LORA) return LAP_RX_EUNSUPPORTED;
    if (!sdr.cap) return LAP_RX_ENODEV;
    ls_iq_control_request_gain_mode(&sdr.cap->control, tenths, tenths == LAP_GAIN_AUTO);
    actual->gain_tenths_db = tenths;
    return 0;
}
static int hw_rate(uint32_t rate, ls_lap_rx_actual_t *actual) {
    (void)actual;
    if (open_kind == K_LORA) return rate == LR_RATE ? 0 : LAP_RX_EUNSUPPORTED;
    if (!sdr.cap) return LAP_RX_EINVAL;
    atomic_store(&sdr.cap->new_rate, rate);   /* the capture task restarts the stream; poll reports it */
    return 0;
}
static int hw_poll(ls_lap_rx_actual_t *actual) {
    if (open_kind == K_LORA) {
        lr_wf_stats st;
        lr_wf_get_stats(&st);
        return st.done && st.error != ESP_OK ? LAP_RX_EIO : 0;
    }
    if (!sdr.cap) return LAP_RX_EIO;
    ls_iq_control_status_t st;
    ls_iq_control_status(&sdr.cap->control, &st);
    if (st.effective_center_known) actual->center_hz = st.effective_center_hz;
    actual->rate_hz = atomic_load(&sdr.cap->rate);
    actual->gain_tenths_db = atomic_load(&sdr.cap->gain);
    const int ph = atomic_load(&sdr.cap->phase);
    return ph < 0 ? ph : atomic_load(&sdr.cap->state) != CAP_RUNNING ? LAP_RX_EIO : 0;
}
static void hw_close(void) {
    if (open_kind == K_LORA) lr_close();
    else if (open_kind != K_NONE) sdr_close();
    open_kind = K_NONE;
}

/* Radio RX API 2.4: exact registry discovery and capture-task completions. */
static int hw_info2(int radio, lap_radio_info2_t *out) {
    kind_t k = radio >= 0 && radio < LAP_RADIOS_MAX ? kinds[radio] : K_NONE;
    memset(out, 0, sizeof(*out)); out->size = sizeof(*out); out->version = 1;
    const char *id = k == K_RTL ? LS_RADIO_ENDPOINT_RTL_USB : k == K_HACKRF ? LS_RADIO_ENDPOINT_HACKRF_USB : NULL;
    if (id) {
        ls_radio_endpoint_info_t i;
        if (ls_radio_endpoint_get(id, &i) != LS_RADIO_OK) return LAP_RX_ENODEV;
        snprintf(out->id, sizeof(out->id), "%s", id);
        snprintf(out->owner, sizeof(out->owner), "%s", i.owner);
        out->present = i.present; out->busy = i.leased && strcmp(i.owner, OWNER);
        out->controls = i.fe_ctls | (1u<<LAP_RX_CTL_TUNE) | (1u<<LAP_RX_CTL_GAIN) | (1u<<LAP_RX_CTL_RATE) | (1u<<LAP_RX_CTL_ANTENNA);
        out->frequency_count = i.frequency_range_count < LAP_RADIO_RANGES_MAX ? i.frequency_range_count : LAP_RADIO_RANGES_MAX;
        out->rate_count = i.sample_rate_range_count < LAP_RADIO_RANGES_MAX ? i.sample_rate_range_count : LAP_RADIO_RANGES_MAX;
        for (int n=0;n<out->frequency_count;n++) out->frequencies[n] = (lap_radio_range_t){i.frequency_ranges[n].min_hz,i.frequency_ranges[n].max_hz};
        for (int n=0;n<out->rate_count;n++) out->rates[n] = (lap_radio_range_t){i.sample_rate_ranges[n].min_hz,i.sample_rate_ranges[n].max_hz};
        int gains[LAP_RADIO_GAINS_MAX]; int n = ls_radio_endpoint_fe_gains(id,gains,LAP_RADIO_GAINS_MAX);
        out->gain_count = n > 0 ? (n < LAP_RADIO_GAINS_MAX ? n : LAP_RADIO_GAINS_MAX) : 0;
        for (n=0;n<out->gain_count;n++) out->gains[n] = gains[n];
        for (n=0;n<LS_RADIO_FE_CTLS;n++) if (out->controls & (1u<<n))
            out->values[n] = ls_rffe_value(k == K_RTL ? LS_RFFE_RTL : LS_RFFE_HACKRF,(ls_radio_fe_ctl_t)n);
        return 0;
    }
    if (k != K_LORA) return LAP_RX_ENODEV;
    snprintf(out->id,sizeof(out->id),"lora.socket"); out->present = ls_lora_present();
    if (ls_lora_chip() != LS_LORA_CHIP_LR20XX) return 0;
    uint32_t caps = ls_lora_caps();
    if (caps & LS_LORA_CAP_RX_EXT) {
        out->frequency_count=1;
        out->frequencies[0]=(lap_radio_range_t){LS_LORA_RX_EXT_MIN_HZ,LS_LORA_RX_EXT_MAX_HZ};
    } else {
        out->frequency_count=1;
        out->frequencies[0]=(lap_radio_range_t){LS_LORA_RX_MIN_HZ,LS_LORA_RX_NARROW_MAX_HZ};
        if (caps & LS_LORA_CAP_BAND_1G5_2G5) {
            out->frequencies[0].max_hz=999999999u;
            out->frequencies[1]=(lap_radio_range_t){1900000000u,2200000000u};
            out->frequencies[2]=(lap_radio_range_t){2400000000u,2500000000u};
            out->frequency_count=3;
        }
    }
    out->rate_count = 1; out->rates[0] = (lap_radio_range_t){LR_RATE,LR_RATE};
    out->controls = (1u<<LAP_RX_CTL_TUNE) | (1u<<LAP_RX_CTL_ANTENNA);
    return 0;
}
static int hw_control(int ctl, int64_t value, ls_lap_rx_actual_t *actual) {
    if (open_kind == K_LORA) {
        if (ctl == LAP_RX_CTL_TUNE) return hw_tune((uint64_t)value,actual);
        if (ctl == LAP_RX_CTL_ANTENNA) return value == 0 ? 0 : LAP_RX_EUNSUPPORTED;
        return LAP_RX_EUNSUPPORTED;
    }
    cap_t *c = sdr.cap;
    if (!c || atomic_load(&c->phase) != PH_RUNNING) return LAP_RX_ENODEV;
    int st = atomic_load_explicit(&c->command_state,memory_order_acquire);
    if (st == 0) {
        c->command_control = ctl; c->command_value = value;
        atomic_store_explicit(&c->command_state,1,memory_order_release);
        return LAP_RX_EAGAIN;
    }
    if (st == 1) return LAP_RX_EAGAIN;
    int rc = c->command_result;
    if (!rc) {
        if (ctl == LAP_RX_CTL_TUNE) actual->center_hz = c->command_center;
        actual->rate_hz = atomic_load(&c->rate);
        actual->gain_tenths_db = atomic_load(&c->gain);
    }
    atomic_store(&c->command_state,0);
    return rc;
}
static const ls_lap_rx_backend_t HW = {hw_list, hw_open, hw_tune, hw_gain, hw_rate, hw_poll, hw_close, hw_info2, hw_control};
const ls_lap_rx_backend_t *ls_lap_rx_default_backend(void) { return &HW; }
#endif
