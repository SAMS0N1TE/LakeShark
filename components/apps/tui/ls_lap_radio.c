#include "ls_lap_context.h"
#include "ls_lap_dsp.h"
#include "ls_lap_features.h"
#include "ls_lap_system.h"
/* See ls_lap_radio.h. The receive session: permission, the request mailbox
 * the worker answers, the bounded I/Q queue and the spectrum feed.
 *
 * The queue is single producer (the backend's capture context) and single
 * consumer (the app, on the screen task). Slots are published by advancing
 * `head` with release order and returned by advancing `tail`; neither side
 * ever waits. When the queue is full the producer drops what it holds, counts
 * it, and flags the next block it does publish with LAP_IQ_GAP.
 *
 * The queue is freed only by the worker, only after the screen task has
 * asked for the session to close (after which it never touches the queue
 * again) and only after the backend's close has stopped the producer. */
#include "ls_lap_radio.h"
#include "ls_lap_radio_io.h"
#include "ls_lap_api.h"
#include "ls_lap_service.h"
#include "dsp/spectrum.h"
#include "ls_lap_rx24.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ls_textpack.h"
#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define BIG_ALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define BIG_FREE(p) heap_caps_free(p)
#define RELAX() vTaskDelay(1)
#else
#define BIG_ALLOC(n) calloc(1, (n))
#define BIG_FREE(p) free(p)
#define RELAX() ((void)0)
#endif
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif

#define SLOTS LS_LAP_RX_SLOTS
#define SLOT_PAIRS LS_LAP_RX_SLOT_PAIRS
#define SPEC_N SPEC_FFT_N
#define SPEC_LINES_PER_S 10u
#define SPEC_AVG 3              /* frames averaged into one published line */
_Static_assert((SLOTS & (SLOTS - 1)) == 0, "slot index wraps by mask");

typedef struct {
    uint64_t first, center;
    uint32_t rate, pairs, flags;
} meta_t;

/* All PSRAM, one allocation per session. */
typedef struct {
    rx24_t *v24;
    uint32_t token;
    atomic_uint head, tail;             /* published / consumed slots */
    atomic_uint dropped_lo, overflows, gaps;
    meta_t meta[SLOTS];
    int16_t iq[SLOTS][SLOT_PAIRS * 2];
    /* producer only */
    uint32_t fill, pending_flags, source_flags, spec_fill, spec_skip;
    uint64_t next_sample, center;
    uint32_t rate;
    bool overflowing, no_iq;
    int16_t spec_stage[SPEC_N * 2];
    /* producer -> worker: one I/Q frame, seqlocked */
    atomic_uint spec_seq;
    int16_t spec_iq[SPEC_N * 2];
    /* worker only */
    spectrum_t fft;
    unsigned spec_seen, spec_frames;
    /* worker -> app: one dB line, seqlocked */
    atomic_uint line_seq;
    float line[SPEC_N];
    /* consumer only */
    uint32_t read_off;
    uint64_t delivered;
} queue_t;

enum { ST_IDLE = 100, ST_OPENING = LAP_RX_OPENING, ST_RUNNING = LAP_RX_RUNNING, ST_CLOSING = 101 };
enum { R_OPEN = 1, R_TUNE = 2, R_GAIN = 4, R_RATE = 8, R_CLOSE = 16, R_LIST = 32 };

static const ls_lap_rx_backend_t *backend;
static void (*wake_io)(void);
static LS_LAP_POOL queue_t *_Atomic p4_q_slots[LS_LAP_CONTEXTS];
#define p4_q (p4_q_slots[ls_lap_context_index()])
               /* set by the worker before RUNNING */
/* Producers inside push or mark: the worker frees a queue it has taken
   away only once this is back to zero, whatever a backend's close left
   running. */
static atomic_int producing;
static LS_LAP_POOL atomic_uint p4_granted_slots[LS_LAP_CONTEXTS];
#define p4_granted (p4_granted_slots[ls_lap_context_index()])

static atomic_int p4_state_slots[LS_LAP_CONTEXTS]  = { [0 ... LS_LAP_CONTEXTS-1] = ST_IDLE };
#define p4_state (p4_state_slots[ls_lap_context_index()])

static LS_LAP_POOL atomic_uint p4_requests_slots[LS_LAP_CONTEXTS];
#define p4_requests (p4_requests_slots[ls_lap_context_index()])

static atomic_flag lock = ATOMIC_FLAG_INIT;
static int serial_seed;
static atomic_int serial_slots[LS_LAP_CONTEXTS];
#define serial serial_slots[ls_lap_context_index()]                       /* screen task */
static LS_LAP_POOL atomic_bool p4_spec_wanted_slots[LS_LAP_CONTEXTS];
#define p4_spec_wanted (p4_spec_wanted_slots[ls_lap_context_index()])
          /* screen task -> worker and producer, advisory */
/* Under `lock`: what the app asked for and what the worker got. */
static LS_LAP_POOL struct {
    int radio;
    lap_rx_config_t cfg;
    uint64_t tune_hz;
    int gain;
    uint32_t rate;
    ls_lap_rx_actual_t actual;
} p4_mail_slots[LS_LAP_CONTEXTS];
#define p4_mail (p4_mail_slots[ls_lap_context_index()])

EXT_RAM_BSS_ATTR static lap_radio_info_t radios[LAP_RADIOS_MAX];
static atomic_int radio_count;
static int p4_worker_radio_slots[LS_LAP_CONTEXTS]  = { [0 ... LS_LAP_CONTEXTS-1] = -1 };
#define p4_worker_radio (p4_worker_radio_slots[ls_lap_context_index()])
            /* the session's radio, worker side */
static LS_LAP_POOL uint32_t p4_open_tries_slots[LS_LAP_CONTEXTS];
#define p4_open_tries (p4_open_tries_slots[ls_lap_context_index()])


static void lk(void) { while (atomic_flag_test_and_set_explicit(&lock, memory_order_acquire)) {} }
static void ulk(void) { atomic_flag_clear_explicit(&lock, memory_order_release); }
static void wake(void) { if (wake_io) wake_io(); }
void ls_lap_radio_set_wake(void (*w)(void)) { wake_io = w; }
static const ls_lap_rx_backend_t *be(void) {
    if (!backend) backend = ls_lap_rx_default_backend();
    return backend;
}
void ls_lap_radio_set_backend(const ls_lap_rx_backend_t *b) { backend = b; }
int ls_lap_radio_snapshot(int index, lap_radio_info_t *out) {
    lk(); int n=atomic_load(&radio_count);
    if(index<0||index>=n){ulk();return LAP_RX_ENODEV;}
    *out=radios[index];ulk();return 0;
}
uint32_t ls_lap_granted(void) { return atomic_load(&p4_granted); }

void ls_lap_radio_caps(lap_caps_t *out) {
    lk();
    out->radio_count = atomic_load(&radio_count);
    for (int i = 0; i < out->radio_count; i++) {
        out->radio_kind[i] = radios[i].kind;
        if (!radios[i].present) continue;
        out->radio_present |= 1u << i;
        out->radio_kinds |= 1u << radios[i].kind;
        if ((LAP_HACKRF_TX && radios[i].kind == LAP_RADIO_KIND_HACKRF) ||
            radios[i].kind == LAP_RADIO_KIND_LR2021 || radios[i].kind == LAP_RADIO_KIND_SX1262 || radios[i].kind==LAP_RADIO_KIND_MIXRF)
            out->radio_tx |= 1u << i;
    }
    ulk();
    atomic_fetch_or(&p4_requests, R_LIST); wake();
}

/* ---- producer side ------------------------------------------------------ */

static void publish(queue_t *b) {
    const unsigned h = atomic_load_explicit(&b->head, memory_order_relaxed);
    b->meta[h & (SLOTS - 1)].pairs = b->fill;
    atomic_store_explicit(&b->head, h + 1, memory_order_release);
    b->fill = 0;
}
static void mark_for(uint32_t token,uint64_t center_hz, uint32_t rate_hz, uint32_t flags) {
    atomic_fetch_add(&producing, 1);
    queue_t *b = atomic_load(&p4_q);
    if (b && (!token||b->token==token)) {
        if (b->fill) publish(b);
        if (center_hz) b->center = center_hz;
        if (rate_hz) b->rate = rate_hz;
        b->source_flags |= flags & (LAP_IQ_REPLAY|LAP_IQ_SYNTHETIC);
        b->pending_flags |= flags;
        rx24_mark(b->v24, flags);
        b->spec_fill = 0;
        if (flags & LAP_IQ_GAP) atomic_fetch_add(&b->gaps, 1);
    }
    atomic_fetch_sub(&producing, 1);
}
static void spec_take(queue_t *b, const int16_t *iq, size_t pairs) {
    if (b->spec_skip) {
        const uint32_t k = pairs < b->spec_skip ? (uint32_t)pairs : b->spec_skip;
        b->spec_skip -= k; iq += 2 * k; pairs -= k;
    }
    while (pairs) {
        uint32_t k = SPEC_N - b->spec_fill;
        if (k > pairs) k = (uint32_t)pairs;
        memcpy(b->spec_stage + 2 * b->spec_fill, iq, 4u * k);
        b->spec_fill += k; iq += 2 * k; pairs -= k;
        if (b->spec_fill < SPEC_N) return;
        /* One frame, handed over seqlocked; the worker skips a torn copy. */
        atomic_fetch_add_explicit(&b->spec_seq, 1, memory_order_acq_rel);
        memcpy(b->spec_iq, b->spec_stage, sizeof(b->spec_iq));
        atomic_fetch_add_explicit(&b->spec_seq, 1, memory_order_release);
        b->spec_fill = 0;
        /* About SPEC_LINES_PER_S * SPEC_AVG frames a second, whatever the rate. */
        const uint32_t every = b->rate / (SPEC_LINES_PER_S * SPEC_AVG);
        b->spec_skip = every > SPEC_N ? every - SPEC_N : 0;
        if (b->spec_skip) {
            const uint32_t s = pairs < b->spec_skip ? (uint32_t)pairs : b->spec_skip;
            b->spec_skip -= s; iq += 2 * s; pairs -= s;
        }
    }
}
static void push(queue_t *b, const int16_t *iq, size_t pairs);
static void gain_for(uint32_t token,int gain) {
    atomic_fetch_add(&producing,1);
    queue_t *b=atomic_load(&p4_q); if (b&&(!token||b->token==token)) rx24_gain(b->v24,gain);
    atomic_fetch_sub(&producing,1);
}
static void push_for(uint32_t token,const int16_t *iq, size_t pairs) {
    atomic_fetch_add(&producing, 1);
    queue_t *b = atomic_load(&p4_q);
    if (b && iq && (!token||b->token==token)) { ls_lap_io_tap(LAP_RECORD_IQ16,iq,pairs*4,b->center,b->rate, (b->pending_flags & LAP_IQ_GAP ? LAP_DATA_GAP:0) | ((b->pending_flags|b->source_flags) & LAP_IQ_REPLAY ? LAP_DATA_REPLAY:0) | ((b->pending_flags|b->source_flags)&LAP_IQ_SYNTHETIC?LAP_DATA_SYNTHETIC:0) | (b->pending_flags&LAP_IQ_RETUNED?LAP_DATA_RETUNED:0)); push(b, iq, pairs); }
    atomic_fetch_sub(&producing, 1);
}
static void push(queue_t *b, const int16_t *iq, size_t pairs) {
    rx24_push(b->v24, iq, pairs, b->next_sample, b->center, b->rate);
    if (p4_spec_wanted) spec_take(b, iq, pairs);
    if (b->no_iq) { b->next_sample += pairs; return; }
    while (pairs) {
        const unsigned h = atomic_load_explicit(&b->head, memory_order_relaxed);
        if (!b->fill) {
            if (h - atomic_load_explicit(&b->tail, memory_order_acquire) >= SLOTS) {
                /* Full: the app is behind. Drop the newest, say so once. */
                atomic_fetch_add(&b->dropped_lo, (unsigned)pairs);
                if (!b->overflowing) { b->overflowing = true; atomic_fetch_add(&b->overflows, 1); }
                if (!(b->pending_flags & LAP_IQ_GAP)) atomic_fetch_add(&b->gaps, 1);
                b->pending_flags |= LAP_IQ_GAP;
                b->next_sample += pairs;
                return;
            }
            b->overflowing = false;
            meta_t *m = &b->meta[h & (SLOTS - 1)];
            m->first = b->next_sample;
            m->center = b->center;
            m->rate = b->rate;
            m->flags = b->pending_flags | b->source_flags;
            b->pending_flags = 0;
        }
        uint32_t k = SLOT_PAIRS - b->fill;
        if (k > pairs) k = (uint32_t)pairs;
        memcpy(b->iq[h & (SLOTS - 1)] + 2 * b->fill, iq, 4u * k);
        b->fill += k; b->next_sample += k; iq += 2 * k; pairs -= k;
        if (b->fill == SLOT_PAIRS) publish(b);
    }
}

/* ---- the app's calls (screen task) -------------------------------------- */

static bool rx_allowed(void) {
    return ls_lap_api_inside() && (atomic_load(&p4_granted) & LAP_CAP_RADIO_RX);
}
static int check(int h) {
    if (!ls_lap_api_inside()) return LAP_RX_EINVAL;
    if (!(atomic_load(&p4_granted) & LAP_CAP_RADIO_RX)) return LAP_RX_EACCES;
    const int st = atomic_load(&p4_state);
    if (h <= 0 || h != serial || st == ST_IDLE || st == ST_CLOSING) return LAP_RX_EINVAL;
    return st;
}
static void ask(unsigned bits) { atomic_fetch_or(&p4_requests, bits); wake(); }

int lap_radio_list(lap_radio_info_t *out, int max) {
    if (!ls_lap_api_inside() || max < 0) return LAP_RX_EINVAL;
    if (!(atomic_load(&p4_granted) & LAP_CAP_RADIO_RX)) return LAP_RX_EACCES;
    if (max > LAP_RADIOS_MAX) max = LAP_RADIOS_MAX;
    if (max && !ls_lap_api_readable(out, (size_t)max * sizeof(*out))) return LAP_RX_EINVAL;
    lk();
    int n = atomic_load(&radio_count);
    if (n > max) n = max;
    if (n) memcpy(out, radios, (size_t)n * sizeof(*out));
    ulk();
    if (!n) ask(R_LIST);
    return n;
}
static bool in_range(const lap_radio_info_t *r, uint64_t hz) { return hz >= r->min_hz && hz <= r->max_hz; }
bool ls_lap_rx_physical_busy(void) {
    for(unsigned i=0;i<LS_LAP_CONTEXTS;i++) if(atomic_load(&p4_state_slots[i])!=ST_IDLE && p4_mail_slots[i].radio>=0) return true;
    return false;
}
int lap_rx_open(int radio, const lap_rx_config_t *cfg) {
    if (!ls_lap_api_inside()) return LAP_RX_EINVAL;
    if (!rx_allowed()) return LAP_RX_EACCES;
    lap_rx_config_t c;
    if (!ls_lap_copy_input(&c, cfg, sizeof(c))) return LAP_RX_EINVAL;
    if (atomic_load(&p4_state) != ST_IDLE) return LAP_RX_EBUSY;   /* one session; a closing one finishes first */
    lk();
    const int n = atomic_load(&radio_count);
    lap_radio_info_t r = radio >= 0 && radio < n ? radios[radio] : (lap_radio_info_t){0};
    ulk();
    if(radio==-1)r=(lap_radio_info_t){.present=true,.features=LAP_RXF_IQ|LAP_RXF_SPECTRUM,.min_hz=1,.max_hz=UINT64_MAX,.min_rate=1,.max_rate=2400000,.gain_min=INT16_MIN,.gain_max=INT16_MAX};
    else if(radio<0||radio>=n)return LAP_RX_ENODEV;
    if(radio>=0 && ls_lap_io_radio_busy())return LAP_RX_EBUSY;
    if (!r.present) return LAP_RX_ENODEV;
    if (!(r.features & LAP_RXF_IQ)) return LAP_RX_EUNSUPPORTED;
    if (r.busy) return LAP_RX_EBUSY;
    if (!c.rate_hz) c.rate_hz = r.min_rate;
    if (!in_range(&r, c.center_hz) || c.rate_hz < r.min_rate || c.rate_hz > r.max_rate ||
        (c.gain_tenths_db != LAP_GAIN_AUTO && (c.gain_tenths_db < r.gain_min || c.gain_tenths_db > r.gain_max)))
        return LAP_RX_ERANGE;
    if ((c.flags & ~(LAP_RX_WIDE | LAP_RX_SPECTRUM_ONLY)) || ((c.flags & LAP_RX_WIDE) && !(r.features & LAP_RXF_WIDE)))
        return LAP_RX_EUNSUPPORTED;
    lk();
    p4_mail.radio = radio;
    p4_mail.cfg = c;
    p4_mail.actual = (ls_lap_rx_actual_t){c.center_hz, c.rate_hz, c.gain_tenths_db};
    ulk();
    serial = serial_seed = serial_seed % 0x3fffffff + 1;
    atomic_store(&p4_state, ST_OPENING);
    ask(R_OPEN);
    return serial;
}
int lap_rx_state(int h) { return check(h); }
int lap_rx_status(int h, lap_rx_status_t *out) {
    const int st = check(h);
    if (st == LAP_RX_EINVAL || st == LAP_RX_EACCES) return st;
    if (!ls_lap_api_readable(out, sizeof(*out))) return LAP_RX_EINVAL;
    memset(out, 0, sizeof(*out));
    out->state = st;
    lk();
    out->radio = p4_mail.radio;
    out->center_hz = p4_mail.actual.center_hz;
    out->rate_hz = p4_mail.actual.rate_hz;
    out->gain_tenths_db = p4_mail.actual.gain_tenths_db;
    ulk();
    out->capacity = SLOTS;
    queue_t *b = st == ST_RUNNING ? atomic_load_explicit(&p4_q, memory_order_acquire) : NULL;
    if (b) {
        out->queued = atomic_load(&b->head) - atomic_load(&b->tail);
        out->delivered = b->delivered;
        out->dropped = atomic_load(&b->dropped_lo);
        out->overflows = atomic_load(&b->overflows);
        out->gaps = atomic_load(&b->gaps);
    }
    return 0;
}
/* A change to a running (or opening) session, checked against its radio. */
static int change(int h, lap_radio_info_t *info) {
    const int st = check(h);
    if (st < 0) return st;
    lk();
    if(p4_mail.radio<0) { ulk(); return LAP_RX_EUNSUPPORTED; }
    *info = radios[p4_mail.radio >= 0 && p4_mail.radio < LAP_RADIOS_MAX ? p4_mail.radio : 0];
    ulk();
    return 0;
}
int lap_rx_tune(int h, uint64_t hz) {
    lap_radio_info_t r;
    int rc = change(h, &r);
    if (rc) return rc;
    if (!in_range(&r, hz)) return LAP_RX_ERANGE;
    lk(); p4_mail.tune_hz = hz; ulk();
    ask(R_TUNE);
    return 0;
}
int lap_rx_gain(int h, int tenths_db) {
    lap_radio_info_t r;
    int rc = change(h, &r);
    if (rc) return rc;
    if (!(r.features & LAP_RXF_GAIN)) return LAP_RX_EUNSUPPORTED;
    if (tenths_db != LAP_GAIN_AUTO && (tenths_db < r.gain_min || tenths_db > r.gain_max)) return LAP_RX_ERANGE;
    lk(); p4_mail.gain = tenths_db; ulk();
    ask(R_GAIN);
    return 0;
}
int lap_rx_rate(int h, uint32_t rate_hz) {
    lap_radio_info_t r;
    int rc = change(h, &r);
    if (rc) return rc;
    if (!(r.features & LAP_RXF_RATE)) return rate_hz == r.min_rate ? 0 : LAP_RX_EUNSUPPORTED;
    if (rate_hz < r.min_rate || rate_hz > r.max_rate) return LAP_RX_ERANGE;
    lk(); p4_mail.rate = rate_hz; ulk();
    ask(R_RATE);
    return 0;
}
int lap_rx_read(int h, int16_t *iq, int max_pairs, lap_iq_meta_t *meta) {
    const int st = check(h);
    if (st < 0) return st;
    if (max_pairs <= 0 || !ls_lap_api_readable(iq, (size_t)max_pairs * 4u) ||
        (meta && !ls_lap_api_readable(meta, sizeof(*meta))))
        return LAP_RX_EINVAL;
    queue_t *b = st == ST_RUNNING ? atomic_load_explicit(&p4_q, memory_order_acquire) : NULL;
    if (!b) return 0;
    const unsigned t = atomic_load_explicit(&b->tail, memory_order_relaxed);
    if (t == atomic_load_explicit(&b->head, memory_order_acquire)) return 0;
    const meta_t *m = &b->meta[t & (SLOTS - 1)];
    uint32_t n = m->pairs - b->read_off;
    if (n > (uint32_t)max_pairs) n = (uint32_t)max_pairs;
    memcpy(iq, b->iq[t & (SLOTS - 1)] + 2 * b->read_off, 4u * n);
    if (meta)
        *meta = (lap_iq_meta_t){m->first + b->read_off, m->center, m->rate, b->read_off ? 0 : m->flags};
    b->read_off += n;
    b->delivered += n;
    if (b->read_off >= m->pairs) {
        b->read_off = 0;
        atomic_store_explicit(&b->tail, t + 1, memory_order_release);
    }
    return (int)n;
}
int lap_rx_spectrum(int h, float *db, int bins, uint32_t *seq) {
    const int st = check(h);
    if (st < 0) return st;
    if (bins < 8 || bins > SPEC_N || !ls_lap_api_readable(db, (size_t)bins * sizeof(float)) ||
        (seq && !ls_lap_api_readable(seq, sizeof(*seq))))
        return LAP_RX_EINVAL;
    if (!p4_spec_wanted) { p4_spec_wanted = true; wake(); }
    queue_t *b = st == ST_RUNNING ? atomic_load_explicit(&p4_q, memory_order_acquire) : NULL;
    if (!b) return 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        const unsigned s0 = atomic_load_explicit(&b->line_seq, memory_order_acquire);
        if (!s0) return 0;
        if (s0 & 1u) continue;
        for (int o = 0; o < bins; o++) {
            const int lo = o * SPEC_N / bins, hi = (o + 1) * SPEC_N / bins;
            float pk = -200.0f;
            for (int i = lo; i < (hi > lo ? hi : lo + 1); i++)
                if (b->line[i] > pk) pk = b->line[i];
            db[o] = pk;
        }
        if (atomic_load_explicit(&b->line_seq, memory_order_acquire) == s0) {
            if (seq) *seq = s0 / 2;
            return bins;
        }
    }
    return 0;
}
void lap_rx_close(int h) {
    /* A session that failed still holds its handle until it is closed. */
    if (!ls_lap_api_inside() || h <= 0 || h != serial) return;
    const int st = atomic_load(&p4_state);
    if (st == ST_IDLE || st == ST_CLOSING) return;
    atomic_store(&p4_state, ST_CLOSING);
    ask(R_CLOSE);
}

/* ---- the console ---------------------------------------------------------- */

void ls_lap_radio_describe(void (*line)(const char *text)) {
    static const char *const STATE[] = {"opening", "running"};
    char t[200];
    lap_radio_info_t r[LAP_RADIOS_MAX];
    lk();
    const int n = atomic_load(&radio_count);
    memcpy(r, radios, sizeof(r));
    const ls_lap_rx_actual_t a = p4_mail.actual;
    const int radio = p4_mail.radio;
    ulk();
    for (int i = 0; i < n; i++) {
        LS_SNPRINTF(t_ls_lap_radio_app_ok_radio_d, t, sizeof(t), "APP OK radio %d %.15s present=%d busy=%d features=0x%02x khz=%lu-%lu rate=%lu-%lu %.31s",
                 i, r[i].name, r[i].present, r[i].busy, r[i].features, (unsigned long)(r[i].min_hz / 1000u),
                 (unsigned long)(r[i].max_hz / 1000u), (unsigned long)r[i].min_rate, (unsigned long)r[i].max_rate, r[i].why);
        line(t);
    }
    const int st = atomic_load(&p4_state);
    queue_t *b = st == ST_RUNNING ? atomic_load(&p4_q) : NULL;
    LS_SNPRINTF(t_ls_lap_radio_app_ok_rx_granted, t, sizeof(t), "APP OK rx granted=0x%x state=%s%s radio=%d center_khz=%lu rate=%lu gain=%d",
             (unsigned)atomic_load(&p4_granted), st == ST_IDLE ? "idle" : st == ST_CLOSING ? "closing" : st >= 0 ? STATE[st] : "error ",
             st < 0 && st != ST_IDLE ? (st == LAP_RX_EBUSY ? "busy" : st == LAP_RX_ENODEV ? "nodev" : "failed") : "",
             radio, (unsigned long)(a.center_hz / 1000u), (unsigned long)a.rate_hz, a.gain_tenths_db);
    line(t);
    if (b) {
        LS_SNPRINTF(t_ls_lap_radio_app_ok_rx_queued, t, sizeof(t), "APP OK rx queued=%u/%u dropped=%u overflows=%u gaps=%u lines=%u spectrum_only=%d",
                 atomic_load(&b->head) - atomic_load(&b->tail), (unsigned)SLOTS, atomic_load(&b->dropped_lo),
                 atomic_load(&b->overflows), atomic_load(&b->gaps), atomic_load(&b->line_seq) / 2, b->no_iq);
        line(t);
    }
}

/* ---- lifetime ----------------------------------------------------------- */

void ls_lap_radio_begin(const lap_app_t *app) {
    ls_lap_radio_end();
    atomic_store(&p4_granted, app ? (app->flags & LS_LAP_CAPS_API26) : 0u);
    ls_lap_io_begin(app);
    ls_lap_events_begin(app);
    ls_lap_audio_begin();
    ls_lap_service_begin(app);           /* API 2.1: whether it may run one */
    if(app) ls_lap_system_ephemeral(app->api_major==2&&app->api_minor>=6);
    if (app) ask(R_LIST);
}
void ls_lap_radio_end(void) {
    ls_lap_dsp_end();
    ls_lap_io_end();
    atomic_store(&p4_granted, 0u);
    p4_spec_wanted = false;
    const int st = atomic_load(&p4_state);
    if (st != ST_IDLE && st != ST_CLOSING) {
        atomic_store(&p4_state, ST_CLOSING);
        ask(R_CLOSE);
    }
    ls_lap_audio_end();
    ls_lap_events_end();
    ls_lap_service_end();
}

/* ---- the worker --------------------------------------------------------- */

static void refresh_list(void) {
    lap_radio_info_t tmp[LAP_RADIOS_MAX];
    memset(tmp, 0, sizeof(tmp));
    int n = be() && be()->list ? be()->list(tmp, LAP_RADIOS_MAX) : 0;
    if (n < 0) n = 0;
    if (n > LAP_RADIOS_MAX) n = LAP_RADIOS_MAX;
    for (int i = 0; i < n; i++) {
        tmp[i].name[sizeof(tmp[i].name) - 1] = 0;
        tmp[i].why[sizeof(tmp[i].why) - 1] = 0;
    }
    lk();
    memcpy(radios, tmp, sizeof(radios));
    atomic_store(&radio_count, n);
    ulk();
    rx24_refresh(be(), n);
}
static void shut(void) {
    ls_lap_dsp_end();
    queue_t *b = atomic_load(&p4_q);
    if (b || atomic_load(&p4_state) != ST_IDLE) {
        if (be() && be()->close && p4_worker_radio >= 0) be()->close();
        p4_worker_radio = -1;
        lk(); atomic_store(&p4_q, NULL); ulk();
        while (atomic_load(&producing)) RELAX();
        if (b) rx24_destroy(b->v24);
        BIG_FREE(b);
    }
    ls_lap_resource_claim(LS_LAP_LEASE_IQ,false);
    atomic_store(&p4_state, ST_IDLE);
    p4_spec_wanted = false;
    ask(R_LIST);                         /* the radio is free again: say so */
}
static void fail(int err) {
    queue_t *b=atomic_load(&p4_q); if (b) rx24_fail(b->v24,err);
    if (be() && be()->close && p4_worker_radio >= 0) be()->close();
    p4_worker_radio = -1;
    atomic_store(&p4_state, err);           /* the app still holds the handle; it closes it */
}
static void open_now(void) {
    lk();
    const int radio = p4_mail.radio;
    const lap_rx_config_t cfg = p4_mail.cfg;
    ulk();
    if(radio>=0 && !ls_lap_resource_claim(LS_LAP_LEASE_IQ,true)) {
        atomic_store(&p4_state,LAP_RX_EBUSY); return;
    }
    queue_t *b = atomic_load(&p4_q);
    if (!b) {
        b = BIG_ALLOC(sizeof(*b));
        if (!b) { atomic_store(&p4_state, LAP_RX_ENOMEM); return; }
        b->v24 = rx24_create();
        if (!b->v24) { BIG_FREE(b); atomic_store(&p4_state, LAP_RX_ENOMEM); return; }
        rx24_gain(b->v24,cfg.gain_tenths_db);
        b->token=(uint32_t)serial;
        b->center = cfg.center_hz;
        b->rate = cfg.rate_hz;
        b->no_iq = (cfg.flags & LAP_RX_SPECTRUM_ONLY) != 0;
        spectrum_context_init(&b->fft);
        atomic_store_explicit(&p4_q, b, memory_order_release);
    }
    ls_lap_rx_actual_t actual = {cfg.center_hz, cfg.rate_hz, cfg.gain_tenths_db};
    p4_worker_radio = radio;
    const int rc = radio==-1 ? 0 : be() && be()->open ? be()->open(radio, &cfg, &actual) : LAP_RX_ENODEV;
    if (rc == LAP_RX_EAGAIN && ++p4_open_tries < 250) {     /* about 5 s at the busy cadence */
        atomic_fetch_or(&p4_requests, R_OPEN);
        return;
    }
    p4_open_tries = 0;
    if (rc == LAP_RX_EAGAIN && be()->close) be()->close();   /* give back what it asked for */
    if (rc < 0) {
        p4_worker_radio = -1;               /* a failed open cleans up after itself */
        atomic_store(&p4_state, rc == LAP_RX_EAGAIN ? LAP_RX_EBUSY : rc);
        return;
    }
    lk(); p4_mail.actual = actual; ulk();
    atomic_store(&p4_state, ST_RUNNING);
}
static void spectrum_work(queue_t *b) {
    const unsigned s = atomic_load_explicit(&b->spec_seq, memory_order_acquire);
    if (!s || (s & 1u) || s == b->spec_seen) return;
    for (int i = 0; i < SPEC_N; i++) {
        b->fft.re[i] = b->spec_iq[2 * i] * (1.0f / 32768.0f) * b->fft.win[i];
        b->fft.im[i] = b->spec_iq[2 * i + 1] * (1.0f / 32768.0f) * b->fft.win[i];
    }
    if (atomic_load_explicit(&b->spec_seq, memory_order_acquire) != s) return;   /* torn: next one */
    b->spec_seen = s;
    spectrum_context_accum(&b->fft);
    if (++b->spec_frames < SPEC_AVG) return;
    b->spec_frames = 0;
    atomic_fetch_add_explicit(&b->line_seq, 1, memory_order_acq_rel);
    spectrum_context_read_db(&b->fft, b->line, SPEC_N);
    atomic_fetch_add_explicit(&b->line_seq, 1, memory_order_release);
    spectrum_context_reset(&b->fft);
}
bool ls_lap_radio_service(void) {
    const bool dsp = ls_lap_dsp_service();
    unsigned bits = atomic_exchange(&p4_requests, 0u);
    const int st = atomic_load(&p4_state);
    if (st == ST_CLOSING || (bits & R_CLOSE)) {
        if (st == ST_CLOSING) shut();
        bits &= ~(unsigned)(R_OPEN | R_TUNE | R_GAIN | R_RATE);
    }
    if ((bits & R_OPEN) && atomic_load(&p4_state) == ST_OPENING) open_now();
    if (atomic_load(&p4_state) == ST_RUNNING && be()) {
        ls_lap_rx_actual_t a;
        lk(); a = p4_mail.actual; const uint64_t hz = p4_mail.tune_hz; const int g = p4_mail.gain; const uint32_t r = p4_mail.rate; ulk();
        int rc = 0;
        if(p4_mail.radio>=0) {
            if ((bits & R_RATE) && be()->rate) rc = be()->rate(r, &a);
            if (rc >= 0 && (bits & R_TUNE) && be()->tune) rc = be()->tune(hz, &a);
            if (rc >= 0 && (bits & R_GAIN) && be()->gain) rc = be()->gain(g, &a);
            if (rc >= 0 && be()->poll) rc = be()->poll(&a);
        }
        if (rc < 0 && rc != LAP_RX_EUNSUPPORTED && rc != LAP_RX_ERANGE) fail(rc);
        else { lk(); p4_mail.actual = a; ulk(); }
        queue_t *b = atomic_load(&p4_q);
        if (b) rx24_work(b->v24, &a);
        if (b && p4_spec_wanted) spectrum_work(b);
    }
    /* While a radio app is open the worker keeps its busy cadence (20 ms), so
       a dongle plugged in shows up in the list within a second. */
    static unsigned passes;
    const bool radio_app = (atomic_load(&p4_granted) & LAP_CAP_RADIO_RX) != 0;
    if ((bits & R_LIST) || (radio_app && !(++passes % 50u))) refresh_list();
    const bool radio_io = ls_lap_io_service();
    const bool audio = ls_lap_audio_service();
    const bool events = ls_lap_events_service();
    const int now = atomic_load(&p4_state);
    return dsp || radio_io || radio_app || audio || events || now == ST_OPENING || now == ST_RUNNING ||
           now == ST_CLOSING || atomic_load(&p4_requests) != 0;
}

/* Radio RX API 2.4; compiled with this session owner on board and bench. */
#include "ls_lap_rx24.inc"

void ls_lap_rx_mark(uint64_t hz,uint32_t rate,uint32_t flags) {
    unsigned old=ls_lap_producer_select(LS_LAP_LEASE_IQ);
    ls_lap_rx_replay_mark(hz,rate,flags); ls_lap_producer_restore(old);
}
void ls_lap_rx_gain_applied(int gain) {
    unsigned old=ls_lap_producer_select(LS_LAP_LEASE_IQ);
    gain_for(0,gain); ls_lap_producer_restore(old);
}
void ls_lap_rx_push(const int16_t *iq,size_t n) {
    unsigned old=ls_lap_producer_select(LS_LAP_LEASE_IQ);
    ls_lap_rx_replay_push(iq,n); ls_lap_producer_restore(old);
}
void ls_lap_rx_replay_mark(uint64_t hz,uint32_t rate,uint32_t flags) { mark_for(0,hz,rate,flags); }
void ls_lap_rx_replay_push(const int16_t *iq,size_t n) { push_for(0,iq,n); }
uint32_t ls_lap_rx_token(void) { return (uint32_t)serial; }
static bool bind_token(uint32_t token) {
    if(!token) return false;
    for(unsigned i=0;i<LS_LAP_CONTEXTS;i++) if((uint32_t)atomic_load(&serial_slots[i])==token && ls_lap_instance_at(i)) {
        ls_lap_instance_bind(ls_lap_instance_at(i)); return true;
    }
    return false;
}
void ls_lap_rx_push_token(uint32_t token,const int16_t *iq,size_t n) {
    ls_lap_instance_t *old=ls_lap_instance_current();
    if(bind_token(token)) push_for(token,iq,n);
    ls_lap_instance_bind(old);
}
void ls_lap_rx_mark_token(uint32_t token,uint64_t hz,uint32_t rate,uint32_t flags) {
    ls_lap_instance_t *old=ls_lap_instance_current();
    if(bind_token(token)) mark_for(token,hz,rate,flags);
    ls_lap_instance_bind(old);
}
void ls_lap_rx_gain_token(uint32_t token,int gain) {
    ls_lap_instance_t *old=ls_lap_instance_current();
    if(bind_token(token)) gain_for(token,gain);
    ls_lap_instance_bind(old);
}

#include "ls_lap_usage.h"
ls_lap_usage_t ls_lap_radio_usage(unsigned s) {
    lk();
    queue_t *b=atomic_load(&p4_q_slots[s]);
    ls_lap_usage_t v={b?sizeof(*b)+sizeof(rx24_t):0,b?1u+(b->v24->channel>0)+(b->v24->decoder>0):0};
    ulk(); return v;
}
