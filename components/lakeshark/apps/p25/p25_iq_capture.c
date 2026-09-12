#include "p25_iq_capture.h"
#include "esp_heap_caps.h"
#include <stdatomic.h>
#include <string.h>

typedef struct {
    p25_iq_capture_status_t status;
    unsigned gap_baseline;
    uint8_t iq[];
} capture_t;

/* the LCD has only a few KiB of internal RAM while P25 runs.
 * Payload and metadata share ONE PSRAM allocation. These control words
 * introduce no worker/stack; neither RX nor console spins on ownership. */
static capture_t *s_capture;
static atomic_uint s_guard;
static atomic_uint s_phase;
static atomic_uint s_reason;
static atomic_uint s_session;
static atomic_uint s_running;
static atomic_uint s_gaps;
static atomic_uint s_interrupt;

static bool take(void)
{
    unsigned expected = 0;
    return atomic_compare_exchange_strong_explicit(&s_guard, &expected, 1,
        memory_order_acquire, memory_order_relaxed);
}

static void release(void)
{
    atomic_store_explicit(&s_guard, 0, memory_order_release);
}

static bool collecting(unsigned phase)
{
    return phase == P25_IQ_CAPTURE_ARMED || phase == P25_IQ_CAPTURE_ACTIVE;
}

static void result(p25_iq_capture_phase_t phase, p25_iq_capture_result_t reason)
{
    if (s_capture) {
        s_capture->status.phase = phase;
        s_capture->status.reason = reason;
    }
    atomic_store_explicit(&s_reason, reason, memory_order_relaxed);
    atomic_store_explicit(&s_phase, phase, memory_order_release);
}

static void expire(uint32_t now_ms)
{
    if (!s_capture || !collecting(s_capture->status.phase)) return;
    unsigned interrupted = atomic_load_explicit(&s_interrupt, memory_order_acquire);
    if (interrupted)
        result(interrupted == P25_IQ_CAPTURE_GAP ? P25_IQ_CAPTURE_FAILED :
               P25_IQ_CAPTURE_ABORTED, (p25_iq_capture_result_t)interrupted);
    else if (!atomic_load_explicit(&s_running, memory_order_acquire) ||
        s_capture->status.session != atomic_load_explicit(&s_session, memory_order_acquire))
        result(P25_IQ_CAPTURE_ABORTED, P25_IQ_CAPTURE_SESSION_CHANGED);
    else if ((uint32_t)(now_ms - s_capture->status.armed_ms) >= P25_IQ_CAPTURE_TIMEOUT_MS)
        result(P25_IQ_CAPTURE_FAILED, P25_IQ_CAPTURE_TIMEOUT);
}

bool p25_iq_capture_collecting(void)
{
    return collecting(atomic_load_explicit(&s_phase, memory_order_acquire));
}

void p25_iq_capture_interrupt(p25_iq_capture_result_t reason)
{
    if (!p25_iq_capture_collecting()) return;
    atomic_store_explicit(&s_interrupt, reason, memory_order_release);
    if (take()) { if (s_capture) expire(s_capture->status.armed_ms); release(); }
}

void p25_iq_capture_receiver(bool running)
{
    atomic_fetch_add_explicit(&s_session, 1, memory_order_acq_rel);
    atomic_store_explicit(&s_running, running, memory_order_release);
    if (take()) {
        if (s_capture && collecting(s_capture->status.phase))
            result(P25_IQ_CAPTURE_ABORTED, P25_IQ_CAPTURE_SESSION_CHANGED);
        release();
    }
    /* A concurrent console owner reconciles the session on its next call.
     * No callback waits for a parked RX task to acknowledge a free. */
}

p25_iq_capture_result_t p25_iq_capture_start(unsigned blocks, uint32_t now_ms)
{
    if (!blocks || blocks > P25_IQ_CAPTURE_MAX_BLOCKS) return P25_IQ_CAPTURE_ARGUMENT;
    if (!take()) return P25_IQ_CAPTURE_BUSY;
    if (s_capture) { release(); return P25_IQ_CAPTURE_BUSY; }
    if (!atomic_load_explicit(&s_running, memory_order_acquire)) {
        result(P25_IQ_CAPTURE_FAILED, P25_IQ_CAPTURE_NOT_RUNNING);
        release();
        return P25_IQ_CAPTURE_NOT_RUNNING;
    }
    atomic_store_explicit(&s_interrupt, 0, memory_order_release);
    size_t bytes = blocks * P25_IQ_CAPTURE_BLOCK_BYTES;
    capture_t *capture = heap_caps_malloc(sizeof(*capture) + bytes,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!capture) {
        result(P25_IQ_CAPTURE_FAILED, P25_IQ_CAPTURE_NO_MEMORY);
        release();
        return P25_IQ_CAPTURE_NO_MEMORY;
    }
    memset(capture, 0, sizeof(*capture));
    capture->status.session = atomic_load_explicit(&s_session, memory_order_acquire);
    capture->status.requested_bytes = (uint32_t)bytes;
    capture->status.armed_ms = now_ms;
    s_capture = capture;
    result(P25_IQ_CAPTURE_ARMED, P25_IQ_CAPTURE_OK);
    expire(now_ms);
    p25_iq_capture_result_t error = s_capture->status.reason;
    release();
    return error;
}

void p25_iq_capture_feed(const uint8_t *iq, size_t bytes,
                         const p25_iq_capture_meta_t *meta)
{
    unsigned phase = atomic_load_explicit(&s_phase, memory_order_acquire);
    if (!collecting(phase)) return;
    if (!take()) {
        if (phase == P25_IQ_CAPTURE_ACTIVE)
            atomic_fetch_add_explicit(&s_gaps, 1, memory_order_relaxed);
        return;
    }
    if (!s_capture || !collecting(s_capture->status.phase)) { release(); return; }
    if (!iq || !meta || bytes != P25_IQ_CAPTURE_BLOCK_BYTES) {
        result(P25_IQ_CAPTURE_FAILED, P25_IQ_CAPTURE_ARGUMENT);
        release();
        return;
    }
    expire(meta->now_ms);
    if (!collecting(s_capture->status.phase)) { release(); return; }
    p25_iq_capture_status_t *status = &s_capture->status;
    if (status->phase == P25_IQ_CAPTURE_ARMED) {
        status->receiver = *meta;
        status->first_ms = meta->now_ms;
        s_capture->gap_baseline = atomic_load_explicit(&s_gaps, memory_order_relaxed);
        result(P25_IQ_CAPTURE_ACTIVE, P25_IQ_CAPTURE_OK);
    } else if (meta->tune_generation != status->receiver.tune_generation ||
               meta->frequency_hz != status->receiver.frequency_hz) {
        result(P25_IQ_CAPTURE_ABORTED, P25_IQ_CAPTURE_TUNE_CHANGED);
    } else if (meta->sample_rate_hz != status->receiver.sample_rate_hz ||
               meta->bandwidth_hz != status->receiver.bandwidth_hz ||
               meta->gain_tenths != status->receiver.gain_tenths ||
               meta->demod_mode != status->receiver.demod_mode ||
               meta->demod_gain != status->receiver.demod_gain) {
        result(P25_IQ_CAPTURE_ABORTED, P25_IQ_CAPTURE_CONFIG_CHANGED);
    }
    if (collecting(status->phase) && s_capture->gap_baseline !=
        atomic_load_explicit(&s_gaps, memory_order_relaxed))
        result(P25_IQ_CAPTURE_FAILED, P25_IQ_CAPTURE_GAP);
    if (collecting(status->phase)) {
        memcpy(s_capture->iq + status->captured_bytes, iq, bytes);
        status->captured_bytes += (uint32_t)bytes;
        status->last_ms = meta->now_ms;
        /* A stop can publish its epoch while this final copy owns the guard.
         * Reconcile before DONE, otherwise a parked mid-block receiver could
         * leave a supposedly complete capture with a hidden session change. */
        expire(meta->now_ms);
        if (collecting(status->phase) && status->captured_bytes == status->requested_bytes)
            result(P25_IQ_CAPTURE_DONE, P25_IQ_CAPTURE_OK);
    }
    release();
}

p25_iq_capture_result_t p25_iq_capture_cancel(void)
{
    if (!take()) return P25_IQ_CAPTURE_BUSY;
    if (s_capture && collecting(s_capture->status.phase))
        result(P25_IQ_CAPTURE_ABORTED, P25_IQ_CAPTURE_CANCELLED);
    release();
    return P25_IQ_CAPTURE_OK;
}

p25_iq_capture_result_t p25_iq_capture_free(void)
{
    if (!take()) return P25_IQ_CAPTURE_BUSY;
    /* Owning the same guard as memcpy is the writer acknowledgement. RX
     * cannot hold or load a buffer pointer across this ownership boundary. */
    capture_t *old = s_capture;
    s_capture = NULL;
    result(P25_IQ_CAPTURE_EMPTY, P25_IQ_CAPTURE_OK);
    if (old) heap_caps_free(old);
    release();
    return P25_IQ_CAPTURE_OK;
}

bool p25_iq_capture_status(p25_iq_capture_status_t *out, uint32_t now_ms)
{
    if (!out || !take()) return false;
    expire(now_ms);
    if (s_capture) *out = s_capture->status;
    else {
        memset(out, 0, sizeof(*out));
        out->phase = atomic_load_explicit(&s_phase, memory_order_relaxed);
        out->reason = atomic_load_explicit(&s_reason, memory_order_relaxed);
    }
    release();
    return true;
}

p25_iq_capture_result_t p25_iq_capture_read(uint32_t offset, uint8_t *out,
    size_t capacity, size_t *read_bytes, uint32_t now_ms)
{
    if (!out || !read_bytes || !capacity || capacity > P25_IQ_CAPTURE_READ_MAX)
        return P25_IQ_CAPTURE_ARGUMENT;
    *read_bytes = 0;
    if (!take()) return P25_IQ_CAPTURE_BUSY;
    expire(now_ms);
    if (!s_capture || collecting(s_capture->status.phase)) {
        release();
        return P25_IQ_CAPTURE_BUSY;
    }
    if (offset > s_capture->status.captured_bytes) {
        release();
        return P25_IQ_CAPTURE_ARGUMENT;
    }
    size_t count = s_capture->status.captured_bytes - offset;
    if (count > capacity) count = capacity;
    memcpy(out, s_capture->iq + offset, count);
    *read_bytes = count;
    release();
    return P25_IQ_CAPTURE_OK;
}

const char *p25_iq_capture_phase_name(p25_iq_capture_phase_t phase)
{
    const char *names[] = {"empty", "armed", "active", "done", "aborted", "failed"};
    return (unsigned)phase < sizeof(names) / sizeof(names[0]) ? names[phase] : "unknown";
}

const char *p25_iq_capture_result_name(p25_iq_capture_result_t error)
{
    const char *names[] = {"ok", "busy", "argument", "no-memory", "not-running",
        "cancelled", "session-changed", "tune-changed", "config-changed", "timeout", "gap"};
    return (unsigned)error < sizeof(names) / sizeof(names[0]) ? names[error] : "unknown";
}
