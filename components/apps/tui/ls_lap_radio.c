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
#include "ls_lap_api.h"
#include "ls_lap_service.h"
#include "dsp/spectrum.h"
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
    atomic_uint head, tail;             /* published / consumed slots */
    atomic_uint dropped_lo, overflows, gaps;
    meta_t meta[SLOTS];
    int16_t iq[SLOTS][SLOT_PAIRS * 2];
    /* producer only */
    uint32_t fill, pending_flags, spec_fill, spec_skip;
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
static queue_t *_Atomic q;               /* set by the worker before RUNNING */
/* Producers inside push or mark: the worker frees a queue it has taken
   away only once this is back to zero, whatever a backend's close left
   running. */
static atomic_int producing;
static atomic_uint granted;
static atomic_int state = ST_IDLE;
static atomic_uint requests;
static atomic_flag lock = ATOMIC_FLAG_INIT;
static int serial;                       /* screen task */
static atomic_bool spec_wanted;          /* screen task -> worker and producer, advisory */
/* Under `lock`: what the app asked for and what the worker got. */
static struct {
    int radio;
    lap_rx_config_t cfg;
    uint64_t tune_hz;
    int gain;
    uint32_t rate;
    ls_lap_rx_actual_t actual;
} mail;
EXT_RAM_BSS_ATTR static lap_radio_info_t radios[LAP_RADIOS_MAX];
static atomic_int radio_count;
static int worker_radio = -1;            /* the session's radio, worker side */
static uint32_t open_tries;

static void lk(void) { while (atomic_flag_test_and_set_explicit(&lock, memory_order_acquire)) {} }
static void ulk(void) { atomic_flag_clear_explicit(&lock, memory_order_release); }
static void wake(void) { if (wake_io) wake_io(); }
void ls_lap_radio_set_wake(void (*w)(void)) { wake_io = w; }
static const ls_lap_rx_backend_t *be(void) {
    if (!backend) backend = ls_lap_rx_default_backend();
    return backend;
}
void ls_lap_radio_set_backend(const ls_lap_rx_backend_t *b) { backend = b; }
uint32_t ls_lap_granted(void) { return atomic_load(&granted); }

/* ---- producer side ------------------------------------------------------ */

static void publish(queue_t *b) {
    const unsigned h = atomic_load_explicit(&b->head, memory_order_relaxed);
    b->meta[h & (SLOTS - 1)].pairs = b->fill;
    atomic_store_explicit(&b->head, h + 1, memory_order_release);
    b->fill = 0;
}
void ls_lap_rx_mark(uint64_t center_hz, uint32_t rate_hz, uint32_t flags) {
    atomic_fetch_add(&producing, 1);
    queue_t *b = atomic_load(&q);
    if (b) {
        if (b->fill) publish(b);
        if (center_hz) b->center = center_hz;
        if (rate_hz) b->rate = rate_hz;
        b->pending_flags |= flags;
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
void ls_lap_rx_push(const int16_t *iq, size_t pairs) {
    atomic_fetch_add(&producing, 1);
    queue_t *b = atomic_load(&q);
    if (b && iq) push(b, iq, pairs);
    atomic_fetch_sub(&producing, 1);
}
static void push(queue_t *b, const int16_t *iq, size_t pairs) {
    if (spec_wanted) spec_take(b, iq, pairs);
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
            m->flags = b->pending_flags;
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
    return ls_lap_api_inside() && (atomic_load(&granted) & LAP_CAP_RADIO_RX);
}
static int check(int h) {
    if (!ls_lap_api_inside()) return LAP_RX_EINVAL;
    if (!(atomic_load(&granted) & LAP_CAP_RADIO_RX)) return LAP_RX_EACCES;
    const int st = atomic_load(&state);
    if (h <= 0 || h != serial || st == ST_IDLE || st == ST_CLOSING) return LAP_RX_EINVAL;
    return st;
}
static void ask(unsigned bits) { atomic_fetch_or(&requests, bits); wake(); }

int lap_radio_list(lap_radio_info_t *out, int max) {
    if (!ls_lap_api_inside() || max < 0) return LAP_RX_EINVAL;
    if (!(atomic_load(&granted) & LAP_CAP_RADIO_RX)) return LAP_RX_EACCES;
    if (max > LAP_RADIOS_MAX) max = LAP_RADIOS_MAX;
    if (max && !ls_lap_api_owns(out, (size_t)max * sizeof(*out))) return LAP_RX_EINVAL;
    lk();
    int n = atomic_load(&radio_count);
    if (n > max) n = max;
    if (n) memcpy(out, radios, (size_t)n * sizeof(*out));
    ulk();
    if (!n) ask(R_LIST);
    return n;
}
static bool in_range(const lap_radio_info_t *r, uint64_t hz) { return hz >= r->min_hz && hz <= r->max_hz; }
int lap_rx_open(int radio, const lap_rx_config_t *cfg) {
    if (!ls_lap_api_inside()) return LAP_RX_EINVAL;
    if (!rx_allowed()) return LAP_RX_EACCES;
    if (!ls_lap_api_owns(cfg, sizeof(*cfg))) return LAP_RX_EINVAL;
    if (atomic_load(&state) != ST_IDLE) return LAP_RX_EBUSY;   /* one session; a closing one finishes first */
    lk();
    const int n = atomic_load(&radio_count);
    lap_radio_info_t r = radio >= 0 && radio < n ? radios[radio] : (lap_radio_info_t){0};
    ulk();
    if (radio < 0 || radio >= n) return LAP_RX_ENODEV;
    if (!r.present) return LAP_RX_ENODEV;
    if (!(r.features & LAP_RXF_IQ)) return LAP_RX_EUNSUPPORTED;
    if (r.busy) return LAP_RX_EBUSY;
    lap_rx_config_t c = *cfg;
    if (!c.rate_hz) c.rate_hz = r.min_rate;
    if (!in_range(&r, c.center_hz) || c.rate_hz < r.min_rate || c.rate_hz > r.max_rate ||
        (c.gain_tenths_db != LAP_GAIN_AUTO && (c.gain_tenths_db < r.gain_min || c.gain_tenths_db > r.gain_max)))
        return LAP_RX_ERANGE;
    if ((c.flags & ~(LAP_RX_WIDE | LAP_RX_SPECTRUM_ONLY)) || ((c.flags & LAP_RX_WIDE) && !(r.features & LAP_RXF_WIDE)))
        return LAP_RX_EUNSUPPORTED;
    lk();
    mail.radio = radio;
    mail.cfg = c;
    mail.actual = (ls_lap_rx_actual_t){c.center_hz, c.rate_hz, c.gain_tenths_db};
    ulk();
    serial = serial % 0x3fffffff + 1;
    atomic_store(&state, ST_OPENING);
    ask(R_OPEN);
    return serial;
}
int lap_rx_state(int h) { return check(h); }
int lap_rx_status(int h, lap_rx_status_t *out) {
    const int st = check(h);
    if (st == LAP_RX_EINVAL || st == LAP_RX_EACCES) return st;
    if (!ls_lap_api_owns(out, sizeof(*out))) return LAP_RX_EINVAL;
    memset(out, 0, sizeof(*out));
    out->state = st;
    lk();
    out->radio = mail.radio;
    out->center_hz = mail.actual.center_hz;
    out->rate_hz = mail.actual.rate_hz;
    out->gain_tenths_db = mail.actual.gain_tenths_db;
    ulk();
    out->capacity = SLOTS;
    queue_t *b = st == ST_RUNNING ? atomic_load_explicit(&q, memory_order_acquire) : NULL;
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
    *info = radios[mail.radio >= 0 && mail.radio < LAP_RADIOS_MAX ? mail.radio : 0];
    ulk();
    return 0;
}
int lap_rx_tune(int h, uint64_t hz) {
    lap_radio_info_t r;
    int rc = change(h, &r);
    if (rc) return rc;
    if (!in_range(&r, hz)) return LAP_RX_ERANGE;
    lk(); mail.tune_hz = hz; ulk();
    ask(R_TUNE);
    return 0;
}
int lap_rx_gain(int h, int tenths_db) {
    lap_radio_info_t r;
    int rc = change(h, &r);
    if (rc) return rc;
    if (!(r.features & LAP_RXF_GAIN)) return LAP_RX_EUNSUPPORTED;
    if (tenths_db != LAP_GAIN_AUTO && (tenths_db < r.gain_min || tenths_db > r.gain_max)) return LAP_RX_ERANGE;
    lk(); mail.gain = tenths_db; ulk();
    ask(R_GAIN);
    return 0;
}
int lap_rx_rate(int h, uint32_t rate_hz) {
    lap_radio_info_t r;
    int rc = change(h, &r);
    if (rc) return rc;
    if (!(r.features & LAP_RXF_RATE)) return rate_hz == r.min_rate ? 0 : LAP_RX_EUNSUPPORTED;
    if (rate_hz < r.min_rate || rate_hz > r.max_rate) return LAP_RX_ERANGE;
    lk(); mail.rate = rate_hz; ulk();
    ask(R_RATE);
    return 0;
}
int lap_rx_read(int h, int16_t *iq, int max_pairs, lap_iq_meta_t *meta) {
    const int st = check(h);
    if (st < 0) return st;
    if (max_pairs <= 0 || !ls_lap_api_owns(iq, (size_t)max_pairs * 4u) ||
        (meta && !ls_lap_api_owns(meta, sizeof(*meta))))
        return LAP_RX_EINVAL;
    queue_t *b = st == ST_RUNNING ? atomic_load_explicit(&q, memory_order_acquire) : NULL;
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
    if (bins < 8 || bins > SPEC_N || !ls_lap_api_owns(db, (size_t)bins * sizeof(float)) ||
        (seq && !ls_lap_api_owns(seq, sizeof(*seq))))
        return LAP_RX_EINVAL;
    if (!spec_wanted) { spec_wanted = true; wake(); }
    queue_t *b = st == ST_RUNNING ? atomic_load_explicit(&q, memory_order_acquire) : NULL;
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
    const int st = atomic_load(&state);
    if (st == ST_IDLE || st == ST_CLOSING) return;
    atomic_store(&state, ST_CLOSING);
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
    const ls_lap_rx_actual_t a = mail.actual;
    const int radio = mail.radio;
    ulk();
    for (int i = 0; i < n; i++) {
        LS_SNPRINTF(t_ls_lap_radio_app_ok_radio_d, t, sizeof(t), "APP OK radio %d %.15s present=%d busy=%d features=0x%02x khz=%lu-%lu rate=%lu-%lu %.31s",
                 i, r[i].name, r[i].present, r[i].busy, r[i].features, (unsigned long)(r[i].min_hz / 1000u),
                 (unsigned long)(r[i].max_hz / 1000u), (unsigned long)r[i].min_rate, (unsigned long)r[i].max_rate, r[i].why);
        line(t);
    }
    const int st = atomic_load(&state);
    queue_t *b = st == ST_RUNNING ? atomic_load(&q) : NULL;
    LS_SNPRINTF(t_ls_lap_radio_app_ok_rx_granted, t, sizeof(t), "APP OK rx granted=0x%x state=%s%s radio=%d center_khz=%lu rate=%lu gain=%d",
             (unsigned)atomic_load(&granted), st == ST_IDLE ? "idle" : st == ST_CLOSING ? "closing" : st >= 0 ? STATE[st] : "error ",
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
    atomic_store(&granted, app ? (app->flags & LS_LAP_CAPS_API2) : 0u);
    ls_lap_events_begin(app);
    ls_lap_audio_begin();
    ls_lap_service_begin(app);           /* API 2.1: whether it may run one */
    if (app) ask(R_LIST);
}
void ls_lap_radio_end(void) {
    atomic_store(&granted, 0u);
    spec_wanted = false;
    const int st = atomic_load(&state);
    if (st != ST_IDLE && st != ST_CLOSING) {
        atomic_store(&state, ST_CLOSING);
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
}
static void shut(void) {
    queue_t *b = atomic_load(&q);
    if (b || atomic_load(&state) != ST_IDLE) {
        if (be() && be()->close && worker_radio >= 0) be()->close();
        worker_radio = -1;
        atomic_store(&q, NULL);
        while (atomic_load(&producing)) RELAX();
        BIG_FREE(b);
    }
    atomic_store(&state, ST_IDLE);
    spec_wanted = false;
    ask(R_LIST);                         /* the radio is free again: say so */
}
static void fail(int err) {
    if (be() && be()->close && worker_radio >= 0) be()->close();
    worker_radio = -1;
    atomic_store(&state, err);           /* the app still holds the handle; it closes it */
}
static void open_now(void) {
    lk();
    const int radio = mail.radio;
    const lap_rx_config_t cfg = mail.cfg;
    ulk();
    queue_t *b = atomic_load(&q);
    if (!b) {
        b = BIG_ALLOC(sizeof(*b));
        if (!b) { atomic_store(&state, LAP_RX_ENOMEM); return; }
        b->center = cfg.center_hz;
        b->rate = cfg.rate_hz;
        b->no_iq = (cfg.flags & LAP_RX_SPECTRUM_ONLY) != 0;
        spectrum_context_init(&b->fft);
        atomic_store_explicit(&q, b, memory_order_release);
    }
    ls_lap_rx_actual_t actual = {cfg.center_hz, cfg.rate_hz, cfg.gain_tenths_db};
    worker_radio = radio;
    const int rc = be() && be()->open ? be()->open(radio, &cfg, &actual) : LAP_RX_ENODEV;
    if (rc == LAP_RX_EAGAIN && ++open_tries < 250) {     /* about 5 s at the busy cadence */
        atomic_fetch_or(&requests, R_OPEN);
        return;
    }
    open_tries = 0;
    if (rc == LAP_RX_EAGAIN && be()->close) be()->close();   /* give back what it asked for */
    if (rc < 0) {
        worker_radio = -1;               /* a failed open cleans up after itself */
        atomic_store(&state, rc == LAP_RX_EAGAIN ? LAP_RX_EBUSY : rc);
        return;
    }
    lk(); mail.actual = actual; ulk();
    atomic_store(&state, ST_RUNNING);
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
    unsigned bits = atomic_exchange(&requests, 0u);
    const int st = atomic_load(&state);
    if (st == ST_CLOSING || (bits & R_CLOSE)) {
        if (st == ST_CLOSING) shut();
        bits &= ~(unsigned)(R_OPEN | R_TUNE | R_GAIN | R_RATE);
    }
    if ((bits & R_OPEN) && atomic_load(&state) == ST_OPENING) open_now();
    if (atomic_load(&state) == ST_RUNNING && be()) {
        ls_lap_rx_actual_t a;
        lk(); a = mail.actual; const uint64_t hz = mail.tune_hz; const int g = mail.gain; const uint32_t r = mail.rate; ulk();
        int rc = 0;
        if ((bits & R_RATE) && be()->rate) rc = be()->rate(r, &a);
        if (rc >= 0 && (bits & R_TUNE) && be()->tune) rc = be()->tune(hz, &a);
        if (rc >= 0 && (bits & R_GAIN) && be()->gain) rc = be()->gain(g, &a);
        if (rc >= 0 && be()->poll) rc = be()->poll(&a);
        if (rc < 0 && rc != LAP_RX_EUNSUPPORTED && rc != LAP_RX_ERANGE) fail(rc);
        else { lk(); mail.actual = a; ulk(); }
        queue_t *b = atomic_load(&q);
        if (b && spec_wanted) spectrum_work(b);
    }
    /* While a radio app is open the worker keeps its busy cadence (20 ms), so
       a dongle plugged in shows up in the list within a second. */
    static unsigned passes;
    const bool radio_app = (atomic_load(&granted) & LAP_CAP_RADIO_RX) != 0;
    if ((bits & R_LIST) || (radio_app && !(++passes % 50u))) refresh_list();
    const bool audio = ls_lap_audio_service();
    const bool events = ls_lap_events_service();
    const int now = atomic_load(&state);
    return radio_app || audio || events || now == ST_OPENING || now == ST_RUNNING ||
           now == ST_CLOSING || atomic_load(&requests) != 0;
}
