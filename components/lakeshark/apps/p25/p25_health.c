#include "p25_health.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define P25_HEALTH_EXT_BSS EXT_RAM_BSS_ATTR
#else
#define P25_HEALTH_EXT_BSS
#endif

typedef struct {
    volatile uint32_t lock;
    volatile uint32_t sequence;
    bool have_raw;
    bool reset_pending;
    p25_health_raw_t raw;
    p25_health_raw_t baseline;
    uint32_t last_net_generation;
    uint32_t last_rfss_generation;
    uint32_t last_sccb_generation;
    uint32_t last_neighbor_generation;
    uint32_t net_seen_ms;
    uint32_t rfss_seen_ms;
    uint32_t sccb_seen_ms;
    uint32_t neighbor_seen_ms;
    p25_health_snapshot_t snapshot;
} p25_health_store_t;

static P25_HEALTH_EXT_BSS p25_health_store_t s_store;

static void writer_lock(void)
{
    uint32_t expected;
    do {
        expected = 0;
    } while (!__atomic_compare_exchange_n(&s_store.lock, &expected, 1, false,
                                           __ATOMIC_ACQUIRE,
                                           __ATOMIC_RELAXED));
}

static void writer_unlock(void)
{
    __atomic_store_n(&s_store.lock, 0, __ATOMIC_RELEASE);
}

static uint32_t since(uint32_t value, uint32_t baseline)
{
    /* Some legacy sources are explicitly zeroed by RESET. Treat that as a new
     * epoch instead of reporting an unsigned-wrap-sized error count. */
    return value >= baseline ? value - baseline : value;
}

static uint16_t since16(uint16_t value, uint16_t baseline)
{
    return value >= baseline ? (uint16_t)(value - baseline) : value;
}

static size_t append(char *out, size_t size, size_t used,
                     const char *fmt, ...)
{
    if (!out || size == 0 || used >= size) return used;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + used, size - used, fmt, ap);
    va_end(ap);
    if (n < 0) return used;
    size_t wanted = (size_t)n;
    return wanted >= size - used ? size - 1 : used + wanted;
}

void p25_health_init(void)
{
    writer_lock();
    uint32_t next = s_store.sequence + 2U;
    memset(&s_store.raw, 0, sizeof(s_store.raw));
    memset(&s_store.baseline, 0, sizeof(s_store.baseline));
    memset(&s_store.snapshot, 0, sizeof(s_store.snapshot));
    s_store.have_raw = false;
    s_store.reset_pending = false;
    s_store.last_net_generation = 0;
    s_store.last_rfss_generation = 0;
    s_store.last_sccb_generation = 0;
    s_store.last_neighbor_generation = 0;
    s_store.net_seen_ms = 0;
    s_store.rfss_seen_ms = 0;
    s_store.sccb_seen_ms = 0;
    s_store.neighbor_seen_ms = 0;
    __atomic_store_n(&s_store.sequence, next, __ATOMIC_RELEASE);
    writer_unlock();
}

void p25_health_publish(const p25_health_raw_t *raw, uint32_t now_ms)
{
    if (!raw) return;
    writer_lock();
    uint32_t sequence = __atomic_load_n(&s_store.sequence, __ATOMIC_RELAXED);
    __atomic_store_n(&s_store.sequence, sequence + 1U, __ATOMIC_RELEASE);

    if (raw->net_valid &&
        (!s_store.have_raw || raw->net_generation != s_store.last_net_generation))
        s_store.net_seen_ms = now_ms;
    if (raw->rfss_valid &&
        (!s_store.have_raw || raw->rfss_generation != s_store.last_rfss_generation))
        s_store.rfss_seen_ms = now_ms;
    if (raw->sccb_valid &&
        (!s_store.have_raw || raw->sccb_generation != s_store.last_sccb_generation))
        s_store.sccb_seen_ms = now_ms;
    if (raw->neighbor_count &&
        (!s_store.have_raw ||
         raw->neighbor_generation != s_store.last_neighbor_generation))
        s_store.neighbor_seen_ms = now_ms;

    s_store.last_net_generation = raw->net_generation;
    s_store.last_rfss_generation = raw->rfss_generation;
    s_store.last_sccb_generation = raw->sccb_generation;
    s_store.last_neighbor_generation = raw->neighbor_generation;
    s_store.raw = *raw;
    s_store.have_raw = true;
    if (s_store.reset_pending) {
        /* RESET may happen between source updates. Rebase on the first complete
         * post-request input so old decoder-owned TSBK/opcode totals cannot
         * reappear on the next LCD refresh. */
        s_store.baseline = *raw;
        s_store.reset_pending = false;
    }

    p25_health_snapshot_t *s = &s_store.snapshot;
    memset(s, 0, sizeof(*s));
    s->sequence = sequence + 2U;
    s->captured_ms = now_ms;
    s->net_seen_ms = s_store.net_seen_ms;
    s->rfss_seen_ms = s_store.rfss_seen_ms;
    s->sccb_seen_ms = s_store.sccb_seen_ms;
    s->neighbor_seen_ms = s_store.neighbor_seen_ms;
    s->net_valid = raw->net_valid;
    s->rfss_valid = raw->rfss_valid;
    s->sccb_valid = raw->sccb_valid;
    s->wacn = raw->wacn;
    s->sysid = raw->sysid;
    s->rfss_sysid = raw->rfss_sysid;
    s->rfss = raw->rfss;
    s->site = raw->site;
    s->control_channel = raw->control_channel;
    s->control_hz = raw->control_hz;
    s->sccb_rfss = raw->sccb_rfss;
    s->sccb_site = raw->sccb_site;
    s->sccb_channel_1 = raw->sccb_channel_1;
    s->sccb_channel_2 = raw->sccb_channel_2;
    s->neighbor_count = raw->neighbor_count;
    memcpy(s->neighbors, raw->neighbors, sizeof(s->neighbors));

    s->nid_valid = since(raw->nid_valid, s_store.baseline.nid_valid);
    s->nid_invalid = since(raw->nid_invalid, s_store.baseline.nid_invalid);
    s->tsbk_valid = since(raw->tsbk_valid, s_store.baseline.tsbk_valid);
    s->tsbk_invalid = since(raw->tsbk_invalid, s_store.baseline.tsbk_invalid);
    s->tsbk_vendor = since(raw->tsbk_vendor, s_store.baseline.tsbk_vendor);
    for (size_t i = 0; i < P25_HEALTH_OPCODE_COUNT; ++i) {
        s->unhandled[i] = since16(raw->unhandled[i],
                                  s_store.baseline.unhandled[i]);
        if (s->unhandled[i]) {
            s->unhandled_distinct++;
            s->unhandled_total += s->unhandled[i];
        }
    }

    memcpy(s->effective_demod, raw->effective_demod,
           sizeof(s->effective_demod));
    s->effective_demod[sizeof(s->effective_demod) - 1U] = 0;
    s->acquisition = raw->acquisition;
    s->c4fm_nids = raw->c4fm_nids;
    s->c4fm_tsbks = raw->c4fm_tsbks;
    s->cqpsk_nids = raw->cqpsk_nids;
    s->cqpsk_tsbks = raw->cqpsk_tsbks;
    s->reacquires = since(raw->reacquires, s_store.baseline.reacquires);
    s->timing_error_valid = raw->timing_error_valid;
    s->timing_error_samples = raw->timing_error_samples;
    s->carrier_error_valid = raw->carrier_error_valid;
    s->carrier_error_hz = raw->carrier_error_hz;

    s->rf_level_permille = raw->rf_level_permille;
    s->usb_read_errors = since(raw->usb_read_errors,
                               s_store.baseline.usb_read_errors);
    s->usb_dropped_bytes = raw->usb_dropped_bytes >=
                                   s_store.baseline.usb_dropped_bytes
                               ? raw->usb_dropped_bytes -
                                     s_store.baseline.usb_dropped_bytes
                               : raw->usb_dropped_bytes;
    s->ring_fill = raw->ring_fill;
    s->ring_size = raw->ring_size;
    s->audio_drops = since(raw->audio_drops, s_store.baseline.audio_drops);
    s->audio_underruns = since(raw->audio_underruns,
                               s_store.baseline.audio_underruns);
    s->buffers_ok = raw->buffers_ok;
    s->cpu_valid = raw->cpu_valid;
    s->cpu_core0_pct = raw->cpu_core0_pct;
    s->cpu_core1_pct = raw->cpu_core1_pct;
    s->heap_internal_free = raw->heap_internal_free;
    s->heap_internal_largest = raw->heap_internal_largest;
    s->heap_psram_free = raw->heap_psram_free;

    s->phase2_grants = since(raw->phase2_grants,
                             s_store.baseline.phase2_grants);
    s->phase2_talkgroup = raw->phase2_talkgroup;
    s->phase2_frequency_hz = raw->phase2_frequency_hz;
    s->phase2_slot = raw->phase2_slot;
    s->phase2_slots_per_carrier = raw->phase2_slots_per_carrier;

    __atomic_store_n(&s_store.sequence, sequence + 2U, __ATOMIC_RELEASE);
    writer_unlock();
}

bool p25_health_read(p25_health_snapshot_t *out)
{
    if (!out) return false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint32_t before = __atomic_load_n(&s_store.sequence, __ATOMIC_ACQUIRE);
        if (before & 1U) continue;
        p25_health_snapshot_t copy = s_store.snapshot;
        uint32_t after = __atomic_load_n(&s_store.sequence, __ATOMIC_ACQUIRE);
        if (before == after && !(after & 1U)) {
            if (copy.sequence == 0) return false;
            *out = copy;
            return true;
        }
    }
    return false;
}

void p25_health_reset_counters(void)
{
    writer_lock();
    uint32_t sequence = __atomic_load_n(&s_store.sequence, __ATOMIC_RELAXED);
    __atomic_store_n(&s_store.sequence, sequence + 1U, __ATOMIC_RELEASE);
    if (s_store.have_raw) {
        s_store.baseline = s_store.raw;
        s_store.reset_pending = true;
        p25_health_snapshot_t *s = &s_store.snapshot;
        s->nid_valid = s->nid_invalid = 0;
        s->tsbk_valid = s->tsbk_invalid = s->tsbk_vendor = 0;
        s->unhandled_total = 0;
        s->unhandled_distinct = 0;
        memset(s->unhandled, 0, sizeof(s->unhandled));
        s->reacquires = 0;
        s->usb_read_errors = 0;
        s->usb_dropped_bytes = 0;
        s->audio_drops = 0;
        s->audio_underruns = 0;
        s->phase2_grants = 0;
        s->sequence = sequence + 2U;
    }
    __atomic_store_n(&s_store.sequence, sequence + 2U, __ATOMIC_RELEASE);
    writer_unlock();
}

p25_health_identity_state_t p25_health_identity_state(
    const p25_health_snapshot_t *s, uint32_t now_ms)
{
    if (!s || (!s->net_valid && !s->rfss_valid))
        return P25_HEALTH_ID_UNKNOWN;
    if (s->net_valid && s->rfss_valid && s->sysid != s->rfss_sysid)
        return P25_HEALTH_ID_INVALID;
    if ((s->net_valid &&
         (uint32_t)(now_ms - s->net_seen_ms) >
             P25_HEALTH_IDENTITY_STALE_MS) ||
        (s->rfss_valid &&
         (uint32_t)(now_ms - s->rfss_seen_ms) >
             P25_HEALTH_IDENTITY_STALE_MS))
        return P25_HEALTH_ID_STALE;
    return P25_HEALTH_ID_CURRENT;
}

size_t p25_health_format_identity(const p25_health_snapshot_t *s,
                                  uint32_t now_ms, char *out,
                                  size_t out_size)
{
    if (!out || out_size == 0) return 0;
    out[0] = 0;
    p25_health_identity_state_t state =
        p25_health_identity_state(s, now_ms);
    if (!s) return append(out, out_size, 0, "SYSTEM snapshot unavailable");
    if (state == P25_HEALTH_ID_UNKNOWN) {
        size_t used = append(out, out_size, 0,
                             "SYSTEM unknown   RFSS/SITE unknown\n");
        if (s->control_hz)
            return append(out, out_size, used,
                          "CONTROL %lu.%04lu MHz   NEIGH %u",
                          (unsigned long)(s->control_hz / 1000000ULL),
                          (unsigned long)((s->control_hz / 100ULL) % 10000ULL),
                          (unsigned)s->neighbor_count);
        return append(out, out_size, used, "CONTROL unknown   NEIGH %u",
                      (unsigned)s->neighbor_count);
    }

    const char *tag = state == P25_HEALTH_ID_CURRENT ? "VALID" :
                      state == P25_HEALTH_ID_STALE ? "STALE" : "INVALID";
    size_t used = 0;
    if (s->net_valid)
        used = append(out, out_size, used, "SYSTEM %s WACN $%05lX SYS $%03X",
                      tag, (unsigned long)s->wacn, (unsigned)s->sysid);
    else
        used = append(out, out_size, used, "SYSTEM %s WACN/SYS unknown", tag);
    if (s->rfss_valid)
        used = append(out, out_size, used, "   RFSS %u SITE %u",
                      (unsigned)s->rfss, (unsigned)s->site);
    else
        used = append(out, out_size, used, "   RFSS/SITE unknown");

    if (s->control_hz)
        used = append(out, out_size, used,
                      "\nCONTROL %lu.%04lu MHz CH $%04X   NEIGH %u",
                      (unsigned long)(s->control_hz / 1000000ULL),
                      (unsigned long)((s->control_hz / 100ULL) % 10000ULL),
                      (unsigned)s->control_channel,
                      (unsigned)s->neighbor_count);
    else
        used = append(out, out_size, used,
                      "\nCONTROL unknown CH $%04X   NEIGH %u",
                      (unsigned)s->control_channel,
                      (unsigned)s->neighbor_count);

    if (s->sccb_valid)
        used = append(out, out_size, used,
                      "   ALT%s %u/%u CH $%04X/$%04X",
                      (uint32_t)(now_ms - s->sccb_seen_ms) >
                              P25_HEALTH_IDENTITY_STALE_MS ? " STALE" : "",
                      (unsigned)s->sccb_rfss, (unsigned)s->sccb_site,
                      (unsigned)s->sccb_channel_1,
                      (unsigned)s->sccb_channel_2);
    if (s->neighbor_count && s->neighbors[0].frequency_hz)
        used = append(out, out_size, used,
                      "\nNEIGHBOR%s %u/%u %lu.%04lu MHz",
                      (uint32_t)(now_ms - s->neighbor_seen_ms) >
                              P25_HEALTH_IDENTITY_STALE_MS ? " STALE" : "",
                      (unsigned)s->neighbors[0].rfss,
                      (unsigned)s->neighbors[0].site,
                      (unsigned long)(s->neighbors[0].frequency_hz / 1000000ULL),
                      (unsigned long)((s->neighbors[0].frequency_hz / 100ULL) %
                                      10000ULL));
    return used;
}

size_t p25_health_format_signal(const p25_health_snapshot_t *s,
                                char *out, size_t out_size)
{
    if (!out || out_size == 0) return 0;
    out[0] = 0;
    if (!s) return append(out, out_size, 0, "HEALTH unavailable");

    static const char *const acq[] = { "MANUAL", "HUNT", "LOCK", "NO LOCK" };
    unsigned ai = (unsigned)s->acquisition;
    const char *acq_name = ai < sizeof(acq) / sizeof(acq[0]) ? acq[ai] : "UNKNOWN";
    size_t used = append(out, out_size, 0,
        "NID valid %lu invalid %lu   TSBK valid %lu invalid %lu vendor %lu\n"
        "DEMOD %s %s   C4FM NID/TSBK %lu/%lu   CQPSK %lu/%lu   retry %lu",
        (unsigned long)s->nid_valid, (unsigned long)s->nid_invalid,
        (unsigned long)s->tsbk_valid, (unsigned long)s->tsbk_invalid,
        (unsigned long)s->tsbk_vendor,
        s->effective_demod[0] ? s->effective_demod : "unknown", acq_name,
        (unsigned long)s->c4fm_nids, (unsigned long)s->c4fm_tsbks,
        (unsigned long)s->cqpsk_nids, (unsigned long)s->cqpsk_tsbks,
        (unsigned long)s->reacquires);

    used = append(out, out_size, used, "\nLOOPS timing ");
    if (s->timing_error_valid)
        used = append(out, out_size, used, "%+.3f sample", (double)s->timing_error_samples);
    else
        used = append(out, out_size, used, "n/a");
    used = append(out, out_size, used, "   carrier ");
    if (s->carrier_error_valid)
        used = append(out, out_size, used, "%+.1f Hz", (double)s->carrier_error_hz);
    else
        used = append(out, out_size, used, "n/a");

    used = append(out, out_size, used,
        "\nRF normalized %u.%u%%   USB errors %lu dropped %llu B\n"
        "BUFFER %u/%u   AUDIO drop %lu underrun %lu   DSD buffers %s",
        (unsigned)(s->rf_level_permille / 10U),
        (unsigned)(s->rf_level_permille % 10U),
        (unsigned long)s->usb_read_errors,
        (unsigned long long)s->usb_dropped_bytes,
        (unsigned)s->ring_fill, (unsigned)s->ring_size,
        (unsigned long)s->audio_drops,
        (unsigned long)s->audio_underruns,
        s->buffers_ok ? "OK" : "INVALID");

    if (s->cpu_valid)
        used = append(out, out_size, used,
                      "\nCPU busy C0 %u%% C1 %u%%",
                      (unsigned)s->cpu_core0_pct,
                      (unsigned)s->cpu_core1_pct);
    else
        used = append(out, out_size, used, "\nCPU busy unavailable");
    used = append(out, out_size, used,
                  "   HEAP int %luK/%luK largest  PSRAM %luK",
                  (unsigned long)(s->heap_internal_free / 1024U),
                  (unsigned long)(s->heap_internal_largest / 1024U),
                  (unsigned long)(s->heap_psram_free / 1024U));

    used = append(out, out_size, used, "\nOP UNKNOWN %lu in %u:",
                  (unsigned long)s->unhandled_total,
                  (unsigned)s->unhandled_distinct);
    unsigned shown = 0;
    for (unsigned op = 0; op < P25_HEALTH_OPCODE_COUNT && shown < 6; ++op) {
        if (!s->unhandled[op]) continue;
        used = append(out, out_size, used, " $%02X=%u", op,
                      (unsigned)s->unhandled[op]);
        shown++;
    }
    if (s->unhandled_distinct > shown)
        used = append(out, out_size, used, " +%u more",
                      (unsigned)(s->unhandled_distinct - shown));
    if (s->phase2_grants)
        used = append(out, out_size, used,
                      "\nPHASE 2 voice UNSUPPORTED x%lu TG %u slot %u/%u",
                      (unsigned long)s->phase2_grants,
                      (unsigned)s->phase2_talkgroup,
                      (unsigned)s->phase2_slot,
                      (unsigned)s->phase2_slots_per_carrier);
    return used;
}
