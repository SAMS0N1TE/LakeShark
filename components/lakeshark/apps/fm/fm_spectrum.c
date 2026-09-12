#include "fm_spectrum.h"
#include "spectrum.h"
#include "esp_attr.h"
#include <string.h>

static EXT_RAM_BSS_ATTR float s_work[FM_SPECTRUM_BINS];
static EXT_RAM_BSS_ATTR float s_bins[FM_SPECTRUM_BINS];
static bool s_enabled;
static uint32_t s_epoch, s_published_epoch, s_lock;
static fm_spectrum_snapshot_t s_meta;

void fm_spectrum_enable(bool enabled)
{
    if (__atomic_exchange_n(&s_enabled, enabled, __ATOMIC_ACQ_REL) != enabled)
        fm_spectrum_invalidate();
}

void fm_spectrum_invalidate(void)
{
    __atomic_add_fetch(&s_epoch, 1, __ATOMIC_ACQ_REL);
}

/* The FM receive worker is the only writer and owns the shared FFT. */
void fm_spectrum_feed(const uint8_t *iq, int len, uint32_t center_hz,
                      uint32_t span_hz, uint32_t now_ms)
{
    if (!__atomic_load_n(&s_enabled, __ATOMIC_ACQUIRE) || !iq ||
        len < SPEC_FFT_N * 2 || !center_hz || !span_hz) return;
    const uint32_t epoch = __atomic_load_n(&s_epoch, __ATOMIC_ACQUIRE);
    if (s_meta.sequence && s_published_epoch == epoch &&
        s_meta.center_hz == center_hz && s_meta.span_hz == span_hz &&
        (uint32_t)(now_ms - s_meta.captured_ms) < FM_SPECTRUM_PERIOD_MS) return;
    spectrum_reset();
    spectrum_accum(iq, len);
    if (!spectrum_read_db(s_work, FM_SPECTRUM_BINS)) return;
    spectrum_reset();
    for (int i = 0; i < FM_SPECTRUM_BINS; ++i) {
        float v = (s_work[i] - FM_SPECTRUM_FLOOR_DB) /
                  (FM_SPECTRUM_TOP_DB - FM_SPECTRUM_FLOOR_DB);
        s_work[i] = v < 0 ? 0 : v > 1 ? 1 : v;
    }
    __atomic_add_fetch(&s_lock, 1, __ATOMIC_ACQ_REL);
    memcpy(s_bins, s_work, sizeof(s_bins));
    s_meta.center_hz = center_hz;
    s_meta.span_hz = span_hz;
    s_meta.captured_ms = now_ms;
    ++s_meta.sequence;
    s_published_epoch = epoch;
    __atomic_add_fetch(&s_lock, 1, __ATOMIC_RELEASE);
}

bool fm_spectrum_read(float *bins, int n, uint32_t now_ms,
                      fm_spectrum_snapshot_t *snapshot)
{
    if (!bins || n != FM_SPECTRUM_BINS ||
        !__atomic_load_n(&s_enabled, __ATOMIC_ACQUIRE)) return false;
    const uint32_t epoch = __atomic_load_n(&s_epoch, __ATOMIC_ACQUIRE);
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint32_t before = __atomic_load_n(&s_lock, __ATOMIC_ACQUIRE);
        if (before & 1) continue;
        fm_spectrum_snapshot_t meta = s_meta;
        const uint32_t published = s_published_epoch;
        memcpy(bins, s_bins, sizeof(s_bins));
        const uint32_t after = __atomic_load_n(&s_lock, __ATOMIC_ACQUIRE);
        if (before != after || (after & 1)) continue;
        if (!meta.sequence || published != epoch ||
            (uint32_t)(now_ms - meta.captured_ms) > 500) return false;
        if (snapshot) *snapshot = meta;
        return true;
    }
    return false;
}
