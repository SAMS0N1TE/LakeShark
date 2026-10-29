#include "esp_attr.h"
#include "lr_wf_session.h"
#include "lr_iq_spectrum.h"
#include "lr2021_iq_queue.h"
#include "lr_ddc.h"
#include "ls_lora.h"
#include "ls_lora_iq.h"
#include "ls_lora_lr20xx.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "ls_cold.h"
#ifdef ESP_PLATFORM
#include "esp_cpu.h"
#define WF_CYCLES() esp_cpu_get_cycle_count()
#else
#define WF_CYCLES() 0u
#endif

/* Wide hand-off: the capture task only copies each published block (1 KiB,
 * 512 IQ8 pairs, ~1125 a second at 576k) into a PSRAM ring; the owner task
 * on core 1 expands, down-converts and feeds. 128 slots hold 114 ms, against
 * the narrow queue's 20 ms, because a 4 KiB check batch lands four at once. */
#define WIDE_SLOTS       128u
#define WIDE_SLOT_BYTES  1024u
#define WIDE_SLOT_PAIRS  (WIDE_SLOT_BYTES / 2)
#define WIDE_CHUNK_PAIRS 256u
/* lr_ddc_out_max(512 pairs at 576k) is 113; leave room. */
#define WIDE_OUT_PAIRS   160u
/* Two check batches behind still counts as keeping up (narrow allows one block). */
#define WIDE_SPARE_SLOTS 8u
#define WIDE_RATE_HZ     576000u
#define WIDE_STATS_PAIRS WIDE_RATE_HZ  /* publish cycle averages about once a second */
_Static_assert((WIDE_SLOTS & (WIDE_SLOTS - 1)) == 0, "ring index wraps by mask");

typedef struct {
    atomic_uint write_index, read_index;
    struct { uint64_t first_pair; uint32_t bytes; } meta[WIDE_SLOTS];
    uint8_t data[WIDE_SLOTS][WIDE_SLOT_BYTES];
} wide_t;
/* The owner's per-sample state. Internal RAM measured no faster than PSRAM
   for the DDC (compute bound), so it stays in PSRAM; lr_ddc_t is ~22 KB,
   most of it the coefficient table. */
typedef struct {
    lr_ddc_t ddc;
    int16_t in[2 * WIDE_SLOT_PAIRS];
    int16_t out[2 * WIDE_OUT_PAIRS];
} wide_dsp_t;

typedef struct {
    lr2021_iq_queue queue;
    atomic_bool stop, done, capture_stop, capture_done;
    lr20xx_stream_result_t result;
    esp_err_t error, capture_error;
    uint32_t hz;
    lr_wf_consumer_t consume;
    void *ctx;
    atomic_uint lost_pairs, gaps;
    /* This run's route, fixed while its capture task lives. */
    bool run_wide;
    int32_t run_offset;
    lr20xx_iq_config_t iq;
    wide_t *wide;           /* PSRAM, from the first wide run to stop */
    wide_dsp_t *dsp;        /* PSRAM, same lifetime */
    bool ddc_ready;         /* dsp->ddc designed for WIDE_RATE_HZ at its offset */
    /* lr_wf_retune: the channel asked for (0: none) and when; the owner
       moves retune_t0 to retune_from and times the first block after. */
    atomic_uint retune_hz;
    int64_t retune_t0, retune_from;
} session_t;
static session_t *session;
static lr_wf_tap_t s_tap;
void lr_wf_set_tap(lr_wf_tap_t tap) { __atomic_store_n(&s_tap, tap, __ATOMIC_RELEASE); }
static inline void tap_block(const uint8_t *data, size_t bytes, uint32_t hz)
{
    const lr_wf_tap_t tap = __atomic_load_n(&s_tap, __ATOMIC_ACQUIRE);
    if (tap) tap(data, bytes, hz);
}

static atomic_bool wide_on, wide_force;
/* What a run takes: the setting, or an owner's pin (lr_wf_set_wide_force). */
static inline bool wide_want(void) { return atomic_load(&wide_on) || atomic_load(&wide_force); }
bool lr_wf_wide_on(void) { return atomic_load(&wide_on); }
void lr_wf_set_wide_force(bool on) { atomic_store(&wide_force, on); }
/* TIME MACHINE's copy of what the consumer gets (lr_wf_set_capture_tap). */
static _Atomic(lr_wf_capture_tap_t) s_capture_tap;
void lr_wf_set_capture_tap(lr_wf_capture_tap_t tap) { atomic_store(&s_capture_tap, tap); }
static inline void tap_out(uint32_t hz, const uint8_t *d, size_t bytes)
{
    const lr_wf_capture_tap_t t = atomic_load_explicit(&s_capture_tap, memory_order_acquire);
    if (t) t(hz, d, bytes);
}
static atomic_int wide_offset = LR_WF_WIDE_DEFAULT_HZ;
_Static_assert(LR_WF_WIDE_DEFAULT_HZ * 4 == WIDE_RATE_HZ, "default offset is lr_ddc's in/4 path");
static struct {
    atomic_bool active, wide;
    atomic_uint channel_hz, lo_hz, rate_hz;
    atomic_uint ddc_mcycles, expand_mcycles;
    atomic_uint queue_high_water, queue_drops, fifo_overflows, fifo_full;
    atomic_uint runs, switches, in_pairs, out_pairs, ddc_dropped;
    atomic_uint bad_at_us, bad_drain_gap_us, bad_wake_late_us, bad_drain_us, bad_peak_fifo;
    atomic_uint bad_level_us, bad_read_us, bad_publish_us, bad_agc_us, bad_busy_us;
    atomic_uint retunes, retune_us, retune_max_us;
} st;

static void wide_clear_counters(void)
{
    atomic_store(&st.ddc_mcycles, 0); atomic_store(&st.expand_mcycles, 0);
    atomic_store(&st.queue_high_water, 0); atomic_store(&st.queue_drops, 0);
    atomic_store(&st.fifo_overflows, 0); atomic_store(&st.fifo_full, 0);
    atomic_store(&st.runs, 0); atomic_store(&st.switches, 0);
    atomic_store(&st.in_pairs, 0); atomic_store(&st.out_pairs, 0);
    atomic_store(&st.ddc_dropped, 0);
    atomic_store(&st.bad_at_us, 0); atomic_store(&st.bad_drain_gap_us, 0);
    atomic_store(&st.bad_wake_late_us, 0); atomic_store(&st.bad_drain_us, 0);
    atomic_store(&st.bad_peak_fifo, 0);
    atomic_store(&st.bad_level_us, 0); atomic_store(&st.bad_read_us, 0);
    atomic_store(&st.bad_publish_us, 0); atomic_store(&st.bad_agc_us, 0); atomic_store(&st.bad_busy_us, 0);
    atomic_store(&st.retunes, 0); atomic_store(&st.retune_us, 0); atomic_store(&st.retune_max_us, 0);
}
esp_err_t lr_wf_set_wide(bool on, int32_t offset_hz)
{
    const int32_t mag = offset_hz < 0 ? -offset_hz : offset_hz;
    if (mag < LR_WF_WIDE_OFFSET_MIN_HZ || mag > LR_WF_WIDE_OFFSET_MAX_HZ)
        return ESP_ERR_INVALID_ARG;
    atomic_store(&wide_offset, offset_hz);
    atomic_store(&wide_on, on);
    wide_clear_counters();
    return ESP_OK;
}
void lr_wf_wide_get(lr_wf_wide_status *out)
{
    *out = (lr_wf_wide_status){
        .on = atomic_load(&wide_on), .offset_hz = atomic_load(&wide_offset),
        .active = atomic_load(&st.active), .wide = atomic_load(&st.wide),
        .channel_hz = atomic_load(&st.channel_hz), .lo_hz = atomic_load(&st.lo_hz),
        .rate_hz = atomic_load(&st.rate_hz),
        .ddc_mcycles = atomic_load(&st.ddc_mcycles),
        .expand_mcycles = atomic_load(&st.expand_mcycles),
        .queue_slots = WIDE_SLOTS, .queue_high_water = atomic_load(&st.queue_high_water),
        .queue_drops = atomic_load(&st.queue_drops),
        .fifo_overflows = atomic_load(&st.fifo_overflows), .fifo_full = atomic_load(&st.fifo_full),
        .runs = atomic_load(&st.runs), .switches = atomic_load(&st.switches),
        .in_pairs = atomic_load(&st.in_pairs), .out_pairs = atomic_load(&st.out_pairs),
        .ddc_dropped = atomic_load(&st.ddc_dropped),
        .bad_at_us = atomic_load(&st.bad_at_us), .bad_drain_gap_us = atomic_load(&st.bad_drain_gap_us),
        .bad_wake_late_us = atomic_load(&st.bad_wake_late_us), .bad_drain_us = atomic_load(&st.bad_drain_us),
        .bad_peak_fifo = atomic_load(&st.bad_peak_fifo),
        .bad_level_us = atomic_load(&st.bad_level_us), .bad_read_us = atomic_load(&st.bad_read_us),
        .bad_publish_us = atomic_load(&st.bad_publish_us), .bad_agc_us = atomic_load(&st.bad_agc_us),
        .bad_busy_us = atomic_load(&st.bad_busy_us),
        .retunes = atomic_load(&st.retunes), .retune_us = atomic_load(&st.retune_us),
        .retune_max_us = atomic_load(&st.retune_max_us),
    };
}
/* The first checked block of a run: the tune is confirmed, and a retune's
   cost is known. */
static void confirm(session_t *s, bool *confirmed)
{
    lr_spec_begin(s->hz); *confirmed = true;
    if (!s->retune_from) return;
    const uint32_t us = (uint32_t)(esp_timer_get_time() - s->retune_from);
    s->retune_from = 0;
    atomic_store(&st.retune_us, us);
    if (us > atomic_load(&st.retune_max_us)) atomic_store(&st.retune_max_us, us);
}
/* The setting differs from what this run streams. */
static bool switch_wanted(const session_t *s)
{
    const bool on = wide_want();
    return on != s->run_wide || (on && atomic_load(&wide_offset) != s->run_offset);
}

static bool IRAM_ATTR collect(void *arg, const uint8_t *data, size_t bytes, uint64_t first)
{
    session_t *s = arg;
    if (atomic_load(&s->capture_stop)) return false;
    if (lr2021_iq_queue_push(&s->queue, data, bytes, first, 1)) return true;
    atomic_fetch_add(&s->lost_pairs, bytes / 4);
    atomic_fetch_add(&s->gaps, 1);
    return false; /* producer stops; next session starts a fresh generation */
}
/* Copy only: no DSP on the capture core. A full ring stops the producer. */
static bool IRAM_ATTR collect_wide(void *arg, const uint8_t *data, size_t bytes, uint64_t first,
                                   uint32_t rate, lr20xx_iq_format_t format)
{
    session_t *s = arg;
    wide_t *q = s->wide;
    if (atomic_load(&s->capture_stop)) return false;
    const unsigned w = atomic_load_explicit(&q->write_index, memory_order_relaxed);
    const unsigned r = atomic_load_explicit(&q->read_index, memory_order_acquire);
    if (rate == WIDE_RATE_HZ && format == LR20XX_IQ8 && bytes && bytes <= WIDE_SLOT_BYTES &&
        !(bytes % 2) && w - r < WIDE_SLOTS) {
        const unsigned slot = w & (WIDE_SLOTS - 1);
        q->meta[slot].first_pair = first; q->meta[slot].bytes = bytes;
        memcpy(q->data[slot], data, bytes);
        atomic_store_explicit(&q->write_index, w + 1, memory_order_release);
        if (w + 1 - r > atomic_load(&st.queue_high_water)) atomic_store(&st.queue_high_water, w + 1 - r);
        return true;
    }
    atomic_fetch_add(&s->lost_pairs, bytes / 2);
    atomic_fetch_add(&s->gaps, 1);
    atomic_fetch_add(&st.queue_drops, 1);
    return false;
}
static void capture(void *arg)
{
    session_t *s = arg;
    s->capture_error = s->run_wide ? lr20xx_stream_live(&s->iq, collect_wide, s, &s->result)
                                   : lr20xx_stream_source2_live(collect, s, &s->result);
    atomic_store_explicit(&s->capture_done, true, memory_order_release);
    vTaskDelete(NULL);
}

enum { RUN_STOP, RUN_SWITCH, RUN_FAIL, RUN_RETUNE };

static int pump_narrow(session_t *s, bool *confirmed)
{
    uint64_t expected = 0;
    int64_t deadline = esp_timer_get_time() + 1000000;
    while (!atomic_load(&s->stop) && !atomic_load_explicit(&s->capture_done, memory_order_acquire)) {
        if (atomic_load(&s->retune_hz)) return RUN_RETUNE;
        if (switch_wanted(s)) return RUN_SWITCH;
        const lr2021_iq_block *b = lr2021_iq_queue_peek(&s->queue);
        if (!b) {
            if (esp_timer_get_time() > deadline) { s->error = ESP_ERR_TIMEOUT; return RUN_FAIL; }
            vTaskDelay(1); continue;
        }
        if (b->generation != 1 || b->first_pair != expected) {
            atomic_fetch_add(&s->gaps, 1);
            atomic_fetch_add(&s->lost_pairs, (b->first_pair > expected ? (uint32_t)(b->first_pair-expected) : 0));
            s->error = ESP_ERR_INVALID_RESPONSE; return RUN_FAIL;
        }
        if (!*confirmed) confirm(s, confirmed);
        bool spare = atomic_load(&s->queue.write_index) - atomic_load(&s->queue.read_index) <= 1;
        if (s->consume) s->consume(s->ctx, b->data, b->bytes, spare);
        tap_block(b->data, b->bytes, s->hz);
        tap_out(s->hz, b->data, b->bytes);
        lr_spec_feed(b->data, b->bytes, b->first_pair, b->generation,
                     (uint32_t)(esp_timer_get_time() / 1000),
                     atomic_load(&s->queue.write_index) - atomic_load(&s->queue.read_index) <= 1);
        expected += b->bytes / 4;
        lr2021_iq_queue_release(&s->queue);
        deadline = esp_timer_get_time() + 1000000;
    }
    return RUN_STOP;
}

/* Wide: IQ8 576k -> int16 -> lr_ddc -> 125k IQ16, delivered exactly as the
 * narrow blocks are. The spectrum counts output pairs as its own stream. */
static int pump_wide(session_t *s, bool *confirmed)
{
    wide_t *q = s->wide;
    uint64_t expected = 0, out_first = 0;
    uint64_t ddc_cycles = 0, expand_cycles = 0;
    uint32_t window = 0;
    int64_t deadline = esp_timer_get_time() + 1000000;
    while (!atomic_load(&s->stop) && !atomic_load_explicit(&s->capture_done, memory_order_acquire)) {
        if (atomic_load(&s->retune_hz)) return RUN_RETUNE;
        if (switch_wanted(s)) return RUN_SWITCH;
        const unsigned r = atomic_load_explicit(&q->read_index, memory_order_relaxed);
        const unsigned w = atomic_load_explicit(&q->write_index, memory_order_acquire);
        if (w == r) {
            if (esp_timer_get_time() > deadline) { s->error = ESP_ERR_TIMEOUT; return RUN_FAIL; }
            vTaskDelay(1); continue;
        }
        const unsigned slot = r & (WIDE_SLOTS - 1);
        if (q->meta[slot].first_pair != expected) {
            atomic_fetch_add(&s->gaps, 1);
            atomic_fetch_add(&s->lost_pairs, (q->meta[slot].first_pair > expected ?
                (uint32_t)(q->meta[slot].first_pair - expected) : 0));
            s->error = ESP_ERR_INVALID_RESPONSE; return RUN_FAIL;
        }
        if (!*confirmed) confirm(s, confirmed);
        const bool spare = w - r <= WIDE_SPARE_SLOTS;
        const size_t pairs = q->meta[slot].bytes / 2;
        const uint32_t c0 = WF_CYCLES();
        wide_dsp_t *dsp = s->dsp;
        lr20xx_iq_expand8(q->data[slot], dsp->in, pairs);
        const uint32_t c1 = WF_CYCLES();
        const uint32_t dropped = dsp->ddc.dropped;
        const size_t n = lr_ddc_process(&dsp->ddc, dsp->in, pairs, dsp->out, WIDE_OUT_PAIRS);
        const uint32_t c2 = WF_CYCLES();
        /* WIDE SPAN view: the whole route, before the down-converter. One
           atomic load when nobody watches; a window every 25 ms when one does. */
        lr_span_feed(dsp->in, pairs, s->iq.frequency_hz, s->hz, WIDE_RATE_HZ,
                     (uint32_t)(esp_timer_get_time() / 1000), spare);
        expand_cycles += c1 - c0; ddc_cycles += c2 - c1;
        if (dsp->ddc.dropped != dropped) atomic_fetch_add(&st.ddc_dropped, dsp->ddc.dropped - dropped);
        /* Release before feeding: the samples now live in dsp->out. */
        atomic_store_explicit(&q->read_index, r + 1, memory_order_release);
        for (size_t off = 0; off < n; off += WIDE_CHUNK_PAIRS) {
            const size_t k = n - off < WIDE_CHUNK_PAIRS ? n - off : WIDE_CHUNK_PAIRS;
            const uint8_t *d = (const uint8_t *)(dsp->out + 2 * off);
            if (s->consume) s->consume(s->ctx, d, k * 4, spare);
            tap_block(d, k * 4, s->hz);
            tap_out(s->hz, d, k * 4);
            lr_spec_feed(d, k * 4, out_first, 1, (uint32_t)(esp_timer_get_time() / 1000), spare);
            out_first += k;
        }
        expected += pairs;
        atomic_fetch_add(&st.in_pairs, (unsigned)pairs);
        atomic_fetch_add(&st.out_pairs, (unsigned)n);
        window += (uint32_t)pairs;
        if (window >= WIDE_STATS_PAIRS) {
            atomic_store(&st.ddc_mcycles, (unsigned)(ddc_cycles * 1000 / window));
            atomic_store(&st.expand_mcycles, (unsigned)(expand_cycles * 1000 / window));
            ddc_cycles = expand_cycles = 0; window = 0;
        }
        deadline = esp_timer_get_time() + 1000000;
    }
    return RUN_STOP;
}

/* Choose this run's route from the setting. Wide tunes the LO offset below
   the channel and points the down-converter back at it. */
static esp_err_t prepare_run(session_t *s)
{
    s->run_wide = wide_want();
    s->run_offset = atomic_load(&wide_offset);
    if (!s->run_wide) return ESP_OK;
    const uint32_t lo = s->hz - (uint32_t)s->run_offset;
    if (!ls_lora_packet_rx_hz_ok(ls_lora_caps(), lo)) return ESP_ERR_INVALID_ARG;
    if (!s->wide) {
        s->wide = heap_caps_malloc(sizeof(*s->wide), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s->wide) return ESP_ERR_NO_MEM;
    }
    if (!s->dsp) {
        s->dsp = heap_caps_malloc(sizeof(*s->dsp), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s->dsp) return ESP_ERR_NO_MEM;
    }
    atomic_store(&s->wide->write_index, 0); atomic_store(&s->wide->read_index, 0);
    /* A hop at the same offset keeps the filter (its design is the slow
       part, a few thousand Kaiser taps) and only clears the stream state. */
    if (s->ddc_ready && s->dsp->ddc.offset_hz == s->run_offset) lr_ddc_reset(&s->dsp->ddc);
    else {
        s->ddc_ready = false;
        if (!lr_ddc_init(&s->dsp->ddc, WIDE_RATE_HZ, s->run_offset, LR_DDC_OUT_HZ) ||
            lr_ddc_out_max(&s->dsp->ddc, WIDE_SLOT_PAIRS) > WIDE_OUT_PAIRS)
            return ESP_ERR_INVALID_ARG;
        s->ddc_ready = true;
    }
    s->iq = (lr20xx_iq_config_t){
        .route = LR20XX_IQ_SOURCE0_576, .frequency_hz = lo, .gain = 13,
        .path = lr20xx_path_for_hz(lo), .host_gain = true,
    };
    return ESP_OK;
}

static void owner(void *arg)
{
    session_t *s = arg;
    ls_lora_cfg_t saved;
    const ls_lora_cfg_t *previous = ls_lora_cfg();
    if (previous) saved = *previous; else ls_lora_cfg_default(&saved);
    ls_lora_cfg_t cfg = saved;
    cfg.freq_hz = s->hz; cfg.sf = 7; cfg.bw_hz = 31250; cfg.invert_iq = false;
    s->error = ls_lora_configure(&cfg);
    if (s->error == ESP_OK) s->error = lr20xx_set_agc_gain(0);
    if (s->error == ESP_OK) s->error = ls_lora_receive();
    bool confirmed = false;
    uint32_t modem_hz = s->hz;   /* where the modem is programmed; source 2 listens there */
    if (s->error != ESP_OK) goto restore;
    for (;;) {
        s->error = prepare_run(s);
        if (s->error != ESP_OK) goto restore;
        /* After a hop, narrow needs the modem on the new channel; wide set
           its own LO and left the modem where it was. */
        if (!s->run_wide && modem_hz != s->hz) {
            cfg.freq_hz = s->hz;
            s->error = ls_lora_configure(&cfg);
            if (s->error == ESP_OK) s->error = lr20xx_set_agc_gain(0);
            if (s->error == ESP_OK) s->error = ls_lora_receive();
            if (s->error != ESP_OK) goto restore;
            modem_hz = s->hz;
        }
        atomic_store(&s->capture_stop, false);
        atomic_store(&s->capture_done, false);
        /* Core 0 even when wide: core 1 has ls_tui (prio 4) under the owner's
           down-conversion, and capture there starved the UI into its 60 s
           stall abort. */
        if (xTaskCreatePinnedToCore(capture, "wf_iq_rx", 4096, s,
                                   configMAX_PRIORITIES - 1, NULL, 0) != pdPASS) {
            atomic_store(&s->capture_done, true);
            s->error = ESP_ERR_NO_MEM; goto restore;
        }
        atomic_store(&st.active, true); atomic_store(&st.wide, s->run_wide);
        atomic_store(&st.channel_hz, s->hz);
        atomic_store(&st.lo_hz, s->run_wide ? s->iq.frequency_hz : s->hz);
        atomic_store(&st.rate_hz, s->run_wide ? WIDE_RATE_HZ : LR_DDC_OUT_HZ);
        if (s->run_wide) atomic_fetch_add(&st.runs, 1);
        const int why = s->run_wide ? pump_wide(s, &confirmed) : pump_narrow(s, &confirmed);
        atomic_store(&s->capture_stop, true);
        while (!atomic_load_explicit(&s->capture_done, memory_order_acquire)) vTaskDelay(1);
        atomic_store(&st.active, false);
        if (s->result.overflows) atomic_fetch_add(&st.fifo_overflows, s->result.overflows);
        if (s->capture_error == ESP_ERR_INVALID_SIZE) atomic_fetch_add(&st.fifo_full, 1);
        if (s->result.overflows || s->capture_error == ESP_ERR_INVALID_SIZE) {
            const lr20xx_stream_result_t *r = &s->result;
            atomic_store(&st.bad_at_us, (unsigned)(r->first_fifo_error_us > 0 ? r->first_fifo_error_us : r->elapsed_us));
            atomic_store(&st.bad_drain_gap_us, (unsigned)r->max_drain_gap_us);
            atomic_store(&st.bad_wake_late_us, (unsigned)r->max_wake_latency_us);
            atomic_store(&st.bad_drain_us, (unsigned)r->max_drain_us);
            atomic_store(&st.bad_peak_fifo, r->max_fifo_level);
            atomic_store(&st.bad_level_us, (unsigned)r->max_level_us);
            atomic_store(&st.bad_read_us, (unsigned)r->max_read_us);
            atomic_store(&st.bad_publish_us, (unsigned)r->max_publish_us);
            atomic_store(&st.bad_agc_us, (unsigned)r->max_agc_us);
            atomic_store(&st.bad_busy_us, (unsigned)r->max_busy_us);
        }
        /* Account for checked blocks abandoned when capture ended; a switch
           drops them on purpose. */
        const bool moved = why == RUN_SWITCH || why == RUN_RETUNE;
        const lr2021_iq_block *left;
        while ((left = lr2021_iq_queue_peek(&s->queue))) {
            if (!moved) atomic_fetch_add(&s->lost_pairs, left->bytes/4);
            lr2021_iq_queue_release(&s->queue);
        }
        if (s->run_wide && !moved) {
            const unsigned w = atomic_load(&s->wide->write_index), r = atomic_load(&s->wide->read_index);
            for (unsigned i = r; i != w; ++i)
                atomic_fetch_add(&s->lost_pairs, s->wide->meta[i & (WIDE_SLOTS - 1)].bytes / 2);
        }
        if (s->run_wide) lr_span_end();
        if (s->consume) s->consume(s->ctx, NULL, 0, false);
        tap_out(s->hz, NULL, 0);
        if (moved && s->capture_error == ESP_OK && s->result.recovery_error == ESP_OK) {
            /* The new route confirms the tune again with its own first block. */
            if (confirmed) { lr_spec_end(); confirmed = false; }
            if (why == RUN_RETUNE) {
                s->retune_from = s->retune_t0;
                s->hz = atomic_exchange(&s->retune_hz, 0);
                atomic_fetch_add(&st.retunes, 1);
            } else atomic_fetch_add(&st.switches, 1);
            continue;
        }
        if (!atomic_load(&s->stop) && !atomic_load(&s->gaps)) atomic_fetch_add(&s->gaps, 1);
        if (s->capture_error != ESP_OK) s->error = s->capture_error;
        if (s->result.recovery_error != ESP_OK) s->error = s->result.recovery_error;
        if (!atomic_load(&s->stop) && s->error == ESP_OK) s->error = ESP_ERR_INVALID_STATE;
        break;
    }
restore:
    if (confirmed) lr_spec_end();
    esp_err_t restored = ls_lora_configure(&saved);
    if (restored != ESP_OK) s->error = restored;
    atomic_store_explicit(&s->done, true, memory_order_release);
    vTaskDelete(NULL);
}
/* start, stop and the stats readers run on different tasks (RF LAB, the
   waterfall, FM, native apps): one lock keeps a session from being freed
   while another task reads or replaces it. A flag and a 1-tick sleep, not a
   FreeRTOS mutex, so the host tests' task shims still stand in. Never taken
   by the owner or capture tasks, so a stop waiting on done under it cannot
   deadlock. */
static atomic_bool s_session_busy;
static void session_lock(void)
{
    while (atomic_exchange_explicit(&s_session_busy, true, memory_order_acquire)) vTaskDelay(1);
}
static void session_unlock(void) { atomic_store_explicit(&s_session_busy, false, memory_order_release); }

esp_err_t lr_wf_retune(uint32_t hz)
{
    if (!hz || !ls_lora_packet_rx_hz_ok(ls_lora_caps(), hz)) return ESP_ERR_INVALID_ARG;
    if (wide_want() &&
        !ls_lora_packet_rx_hz_ok(ls_lora_caps(), hz - (uint32_t)atomic_load(&wide_offset)))
        return ESP_ERR_INVALID_ARG;
    /* Under the session lock, so a stop cannot free the session mid-write. */
    session_lock();
    session_t *s = session;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s && !atomic_load_explicit(&s->done, memory_order_acquire)) {
        s->retune_t0 = esp_timer_get_time();
        atomic_store(&s->retune_hz, hz);
        err = ESP_OK;
    }
    session_unlock();
    return err;
}
static void get_stats_locked(lr_wf_stats *out)
{
    *out = (lr_wf_stats){0};
    if (!session) return;
    out->lost_pairs = atomic_load(&session->lost_pairs);
    out->gaps = atomic_load(&session->gaps);
    out->done = atomic_load_explicit(&session->done, memory_order_acquire);
    out->error = out->done ? session->error : ESP_OK;
}
static void stop_locked(lr_wf_stats *out)
{
    if (out) *out = (lr_wf_stats){0};
    if (!session) return;
    atomic_store(&session->stop, true);
    while (!atomic_load_explicit(&session->done, memory_order_acquire)) vTaskDelay(1);
    if (out) get_stats_locked(out);
    heap_caps_free(session->wide);
    heap_caps_free(session->dsp);
    heap_caps_free(session); session = NULL;
}
void lr_wf_stop(void) { lr_wf_stop_stats(NULL); }
void lr_wf_stop_stats(lr_wf_stats *out)
{
    session_lock();
    stop_locked(out);
    session_unlock();
}
esp_err_t lr_wf_start(uint32_t hz) { return lr_wf_start_consumer(hz, NULL, NULL); }
void lr_wf_get_stats(lr_wf_stats *out)
{
    session_lock();
    get_stats_locked(out);
    session_unlock();
}
static esp_err_t start_locked(uint32_t hz, lr_wf_consumer_t consume, void *ctx);
LS_COLD esp_err_t lr_wf_start_consumer(uint32_t hz, lr_wf_consumer_t consume, void *ctx)
{
    session_lock();
    const esp_err_t e = start_locked(hz, consume, ctx);
    session_unlock();
    return e;
}
static esp_err_t start_locked(uint32_t hz, lr_wf_consumer_t consume, void *ctx)
{
    if (session && (session->consume != consume || session->ctx != ctx))
        return ESP_ERR_INVALID_STATE;
    if (session && session->hz != hz) stop_locked(NULL);
    if (session) return atomic_load_explicit(&session->done, memory_order_acquire)
                         ? session->error : ESP_OK;
    if (!ls_lora_packet_rx_hz_ok(ls_lora_caps(), hz)) return ESP_ERR_INVALID_ARG;
    if (wide_want() &&
        !ls_lora_packet_rx_hz_ok(ls_lora_caps(), hz - (uint32_t)atomic_load(&wide_offset)))
        return ESP_ERR_INVALID_ARG;
    if (ls_lora_chip() != LS_LORA_CHIP_LR20XX) return ESP_ERR_NOT_SUPPORTED;
    if (lr20xx_stream_source2_active() || ls_lora_fsk_active() || ls_lora_scanning())
        return ESP_ERR_INVALID_STATE;
    session_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s) return ESP_ERR_NO_MEM;
    atomic_init(&s->stop, false); atomic_init(&s->done, false);
    atomic_init(&s->capture_stop, false); atomic_init(&s->capture_done, true);
    s->hz = hz; s->consume = consume; s->ctx = ctx;
    atomic_init(&s->lost_pairs, 0); atomic_init(&s->gaps, 0); atomic_init(&s->retune_hz, 0);
    if (!lr2021_iq_queue_init(&s->queue) ||
        xTaskCreatePinnedToCore(owner, "wf_iq", 6144, s, 10, NULL, 1) != pdPASS) {
        heap_caps_free(s); return ESP_ERR_NO_MEM;
    }
    session = s;
    return ESP_OK;
}
