/* See rec_ism.h. The decoding itself is lr433_dec.c's; nothing here knows a
   device. The front end follows REC's own slicer (app_rec.c): magnitude
   against a floor tracked only while no carrier is up, on and off
   thresholds apart for hysteresis, runs below a minimum folded into the one
   before. */

#include "rec_ism.h"
#include "ls_decoded.h"
#pragma weak ls_decoded_publish

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

/* ------------------------------------------------------------- decoder -- */

/* Lay the edges out as decisions at `rate`, from the first edge on. The
   boundaries come from the running total, so rounding never accumulates. */
static int render(const int32_t *e, int n, uint32_t rate, uint8_t *bits, int cap_bits)
{
    memset(bits, 0, (size_t)(cap_bits + 7) / 8);
    uint64_t t = 0;
    int pos = 0;
    for (int i = 0; i < n && pos < cap_bits; i++) {
        uint64_t d = e[i] < 0 ? (uint64_t)(-(int64_t)e[i]) : (uint64_t)e[i];
        if (e[i] < 0 && d > REC_ISM_GAP_CLAMP_US) d = REC_ISM_GAP_CLAMP_US;
        t += d;
        int end = (int)((t * rate + 500000u) / 1000000u);
        if (end > cap_bits) end = cap_bits;
        if (e[i] > 0)
            for (int b = pos; b < end; b++) bits[b >> 3] |= (uint8_t)(0x80u >> (b & 7));
        if (end > pos) pos = end;
    }
    return pos;
}

static inline unsigned bit_at(const uint8_t *b, int i) { return (b[i >> 3] >> (7 - (i & 7))) & 1u; }

static bool same(const lr433_msg_t *a, const lr433_msg_t *b)
{
    return a->proto == b->proto && a->channel == b->channel && !strcmp(a->id, b->id) &&
           a->consumption == b->consumption;
}

/* Keep out[from..from+got) that are not already among out[0..from). */
static int keep_new(lr433_msg_t *out, int from, int got)
{
    int k = from;
    for (int i = from; i < from + got; i++) {
        bool dup = false;
        for (int j = 0; j < k && !dup; j++) dup = same(&out[j], &out[i]);
        if (!dup) { if (k != i) out[k] = out[i]; k++; }
    }
    return k - from;
}

/* The ERT detector, 1010100 as Manchester chips, in time order. */
#define ERT_DETECTOR 0x2665u
#define ERT_DETECTOR_CHIPS 14
#define ERT_MATCHES 8

static int ert(const int32_t *edges, int n, rec_ism_work_t *w, lr433_msg_t *out, int max)
{
    const int chips = render(edges, n, REC_ISM_ERT_RATE, w->bits, (int)REC_ISM_BITS_BYTES * 8);
    uint32_t win = 0;
    int k = 0, tried = 0;
    for (int i = 0; i < chips && tried < ERT_MATCHES && k < max; i++) {
        win = ((win << 1) | bit_at(w->bits, i)) & ((1u << ERT_DETECTOR_CHIPS) - 1u);
        if (i < ERT_DETECTOR_CHIPS - 1 || win != ERT_DETECTOR) continue;
        tried++;
        const int start = i + 1;
        int len = chips - start;
        if (len > (int)sizeof(w->ert) * 8) len = (int)sizeof(w->ert) * 8;
        if (len < 96) break;
        memset(w->ert, 0, sizeof(w->ert));
        for (int b = 0; b < len; b++)
            if (bit_at(w->bits, start + b)) w->ert[b >> 3] |= (uint8_t)(0x80u >> (b & 7));
        const lr433_capture_t cap = {
            .kind = LR433_IN_ERT, .rate = REC_ISM_ERT_RATE,
            .lead = ERT_DETECTOR, .lead_bits = ERT_DETECTOR_CHIPS,
            .data = w->ert, .bits = len,
        };
        k += keep_new(out, k, lr433_decode(&cap, &w->lr, out + k, max - k));
    }
    return k;
}

int rec_ism_decode_edges(const int32_t *edges, int n, bool fsk,
                         rec_ism_work_t *w, lr433_msg_t *out, int max)
{
    if (!edges || n < 2 || !w || !out || max <= 0) return 0;
    const int bits = render(edges, n, REC_ISM_RATE, w->bits, (int)REC_ISM_BITS_BYTES * 8);
    const lr433_capture_t cap = {
        .kind = fsk ? LR433_IN_FSK : LR433_IN_OOK, .rate = REC_ISM_RATE,
        .data = w->bits, .bits = bits,
    };
    int k = lr433_decode(&cap, &w->lr, out, max);
    if (fsk || k >= max) return k;
    lr433_msg_t *more = out + k;
    const int got = ert(edges, n, w, more, max - k);
    return k + keep_new(out, k, got);
}

void rec_ism_describe(const lr433_msg_t *m, char *out, size_t n)
{
    if (!out || !n) return;
    if (!m) { out[0] = 0; return; }
    char id[24], v[32] = "";
    if (m->channel < 0) snprintf(id, sizeof(id), "%s", m->id);
    else if (m->channel >= 'A') snprintf(id, sizeof(id), "%s %c", m->id, m->channel);
    else snprintf(id, sizeof(id), "%s %d", m->id, m->channel);
    switch (lr433_proto_class(m->proto)) {
    case LR433_METER:
        if (m->consumption >= 0) snprintf(v, sizeof(v), " %lld", (long long)m->consumption);
        break;
    case LR433_TPMS:
        /* A tyre sensor's ID only: its readings are shown for MY ID in LR433. */
        break;
    default: {
        size_t at = 0;
        if (!isnan(m->temp_c)) at += (size_t)snprintf(v + at, sizeof(v) - at, " %.1fC", (double)m->temp_c);
        if (!isnan(m->humidity) && at < sizeof(v))
            at += (size_t)snprintf(v + at, sizeof(v) - at, " %.0f%%", (double)m->humidity);
        if (!at && (!isnan(m->wind_ms) || !isnan(m->rain_mm))) snprintf(v, sizeof(v), " wind/rain");
        break;
    }
    }
    snprintf(out, n, "%s %s%s%s", lr433_proto_label(m->proto), id, v, m->battery_ok == 0 ? " BAT" : "");
}

/* ---------------------------------------------------------- front end -- */

#define FLOOR_SHIFT 12
#define MIN_SNR 10

void rec_ism_front_init(rec_ism_front_t *f, uint32_t fs, int32_t *ook, int32_t *fsk,
                        int cap, rec_ism_burst_fn fn, void *ctx)
{
    memset(f, 0, sizeof(*f));
    f->fs = fs ? fs : 1;
    f->gap_us = 20000;
    f->min_us = 12;
    f->min_edges = 8;
    f->buf[0] = ook;
    f->buf[1] = fsk;
    f->cap = cap;
    f->fn = fn;
    f->ctx = ctx;
    f->floor_acc = 8 << FLOOR_SHIFT;
    f->floor = 8;
    rec_fsk_init(&f->fsk, (float)f->fs);
}

void rec_ism_front_reset(rec_ism_front_t *f)
{
    f->active = false;
    f->carrier = false;
    f->n[0] = f->n[1] = 0;
    rec_fsk_init(&f->fsk, (float)f->fs);
}

/* Microseconds from the burst's running total of samples, so a width
   rounded down here is made up by the next one: rounding each run on its
   own drifts a 1300-edge IDM message by several ERT chips. */
static uint32_t to_us(rec_ism_front_t *f, int k, uint32_t samples)
{
    f->t_samples[k] += samples;
    const uint64_t end = (f->t_samples[k] * 1000000u + f->fs / 2) / f->fs;
    const uint32_t us = (uint32_t)(end - f->t_us[k]);
    f->t_us[k] = end;
    return us;
}

static void push(rec_ism_front_t *f, int k, bool level, uint32_t samples)
{
    const uint32_t us = to_us(f, k, samples);
    if (!us) return;
    int32_t *e = f->buf[k];
    int *n = &f->n[k];
    if (*n > 0 && (us < f->min_us || (e[*n - 1] > 0) == level)) {
        e[*n - 1] += e[*n - 1] > 0 ? (int32_t)us : -(int32_t)us;
        return;
    }
    if (*n == 0 && !level && k == 0) return;
    if (*n < f->cap) e[(*n)++] = level ? (int32_t)us : -(int32_t)us;
}

static void finish(rec_ism_front_t *f)
{
    for (int k = 0; k < 2; k++) push(f, k, f->lvl[k], f->run[k]);
    f->active = false;
    if (f->n[0] >= f->min_edges || f->n[1] >= f->min_edges) {
        f->bursts++;
        if (f->fn) f->fn(f->ctx, f->buf[0], f->n[0], f->buf[1], f->n[1], f->peak);
    }
    f->n[0] = f->n[1] = 0;
}

void rec_ism_front_u8(rec_ism_front_t *f, const uint8_t *iq, size_t bytes)
{
    if (!f || !iq || !f->buf[0] || !f->buf[1] || f->cap <= 0) return;
    const uint32_t gap = (uint32_t)(((uint64_t)f->gap_us * f->fs) / 1000000u);
    for (size_t i = 0; i + 1 < bytes; i += 2) {
        const int si = (int)iq[i] - 127, sq = (int)iq[i + 1] - 127;
        const int mag = (si < 0 ? -si : si) + (sq < 0 ? -sq : sq);
        /* Two samples averaged: both edges move by the same half sample. */
        f->env = (mag + f->prev_mag) / 2;
        f->prev_mag = mag;
        if (!f->carrier) {
            f->floor_acc += f->env - (f->floor_acc >> FLOOR_SHIFT);
            f->floor = f->floor_acc >> FLOOR_SHIFT;
            if (f->floor < 2) f->floor = 2;
        }
        int on = f->floor * 4, off = f->floor * 2;
        if (on < f->floor + MIN_SNR) on = f->floor + MIN_SNR;
        if (off < f->floor + MIN_SNR / 2) off = f->floor + MIN_SNR / 2;
        /* Inside a burst the slicer sits halfway between the floor and the
           carrier's level, so a pulse is as long as it was sent: thresholds
           near the floor stretch every mark by the envelope's fall, which at
           ERT's 30 us chips is a third of a chip. */
        if (f->active) {
            f->level -= f->level >> 10;
            if (f->env > f->level) f->level = f->env;
            const int mid = (f->level + f->floor) / 2, h = (f->level - f->floor) / 8;
            if (mid - h > off) { on = mid + h; off = mid - h; }
        }
        f->carrier = f->carrier ? f->env > off : f->env > on;
        const bool lv[2] = { f->carrier, rec_fsk_step(&f->fsk, si, sq, f->carrier) };
        if (!f->active) {
            if (!f->carrier) continue;
            f->active = true;
            f->n[0] = f->n[1] = 0;
            f->t_samples[0] = f->t_samples[1] = f->t_us[0] = f->t_us[1] = 0;
            f->peak = f->level = f->env;
            f->quiet = 0;
            for (int k = 0; k < 2; k++) { f->lvl[k] = lv[k]; f->run[k] = 0; }
        }
        for (int k = 0; k < 2; k++) {
            if (lv[k] == f->lvl[k]) { f->run[k]++; continue; }
            push(f, k, f->lvl[k], f->run[k]);
            f->lvl[k] = lv[k];
            f->run[k] = 1;
        }
        if (f->env > f->peak) f->peak = f->env;
        f->quiet = f->carrier ? 0 : f->quiet + 1;
        if (f->quiet >= gap || f->n[0] >= f->cap || f->n[1] >= f->cap) finish(f);
    }
}

/* --------------------------------------------------------------- sink -- */

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static struct { rec_ism_sink_fn fn; void *ctx; } s_sinks[REC_ISM_SINKS];

/* The last few published, for the duplicate window. */
#define RECENT 6
#define DUP_US 1000000
static struct { lr433_msg_t m; int64_t at; } s_recent[RECENT];
static int s_recent_next;
static uint32_t s_published, s_duplicates;

bool rec_ism_subscribe(rec_ism_sink_fn fn, void *ctx)
{
    if (!fn) return false;
    bool ok = false;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < REC_ISM_SINKS && !ok; i++)
        if (s_sinks[i].fn == fn && s_sinks[i].ctx == ctx) ok = true;
    for (int i = 0; i < REC_ISM_SINKS && !ok; i++)
        if (!s_sinks[i].fn) { s_sinks[i].fn = fn; s_sinks[i].ctx = ctx; ok = true; }
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

void rec_ism_unsubscribe(rec_ism_sink_fn fn, void *ctx)
{
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < REC_ISM_SINKS; i++)
        if (s_sinks[i].fn == fn && s_sinks[i].ctx == ctx) s_sinks[i].fn = NULL;
    portEXIT_CRITICAL(&s_lock);
}

void rec_ism_publish(const rec_ism_msg_t *msg, int64_t now_us)
{
    if (!msg) return;
    bool dup = false;
    struct { rec_ism_sink_fn fn; void *ctx; } sinks[REC_ISM_SINKS];
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < RECENT && !dup; i++)
        dup = s_recent[i].at && now_us - s_recent[i].at < DUP_US && same(&s_recent[i].m, &msg->m);
    if (dup) {
        s_duplicates++;
    } else {
        s_recent[s_recent_next].m = msg->m;
        s_recent[s_recent_next].at = now_us ? now_us : 1;
        s_recent_next = (s_recent_next + 1) % RECENT;
        s_published++;
    }
    memcpy(sinks, s_sinks, sizeof(sinks));
    portEXIT_CRITICAL(&s_lock);
    if (dup) return;
    for (int i = 0; i < REC_ISM_SINKS; i++)
        if (sinks[i].fn) sinks[i].fn(msg, sinks[i].ctx);
    /* Onto the decoded timeline once per device message, whichever radio
       heard it. Weak: host builds of the decoders link no timeline. */
    if (ls_decoded_publish) {
        ls_decoded_t ev = { .kind = LS_DECODED_SUBGHZ, .at_us = now_us, .freq_hz = msg->hz };
        snprintf(ev.from, sizeof(ev.from), "%s", msg->m.model);
        snprintf(ev.text, sizeof(ev.text), "%s %s", msg->m.model, msg->m.id);
        ls_decoded_publish(&ev);
    }
}

void rec_ism_counts(uint32_t *published, uint32_t *duplicates)
{
    portENTER_CRITICAL(&s_lock);
    if (published) *published = s_published;
    if (duplicates) *duplicates = s_duplicates;
    portEXIT_CRITICAL(&s_lock);
}
