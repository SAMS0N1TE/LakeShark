/* bounded P25 spectrum publication over the IQ stream the decoder
 * already owns.  No function in this file can acquire, sweep or tune a radio. */

#include "p25_spectrum.h"

#include <stddef.h>
#include <string.h>

#include "esp_attr.h"
#include "spectrum.h"

#if P25_SPECTRUM_BINS != SPEC_FFT_N
#error "P25 publication bins must match the shared FFT"
#endif

/* Both arrays are linear, read-mostly publications. Keeping their 4 KiB in
 * PSRAM avoids spending scarce internal RAM; spectrum.c owns the 4 KiB hot
 * butterfly workspace in internal RAM. */
static EXT_RAM_BSS_ATTR float s_db[P25_SPECTRUM_BINS];
static EXT_RAM_BSS_ATTR float s_pub[P25_SPECTRUM_BINS];

static volatile bool     s_enabled;
static volatile uint32_t s_epoch;
static volatile uint32_t s_pub_seq;
static volatile bool     s_ready;
static uint32_t          s_rx_epoch;
static uint32_t          s_pub_epoch;
static uint32_t          s_center_hz;
static uint32_t          s_span_hz;
/* The tuner leaks its local oscillator into the mix, so the centre bin carries
   a spur that is not in the air and drew a permanent line down the middle of
   the waterfall. The band sweep masks a whole guard band either side of centre
   (LS_SWEEP_DC_GUARD_HZ) and this view cannot: the channel being watched IS at
   centre, so a guard band would hide the signal rather than the artifact.
   Interpolating across the spur instead is a trade, and the width is where the
   trade is made. Measured skirt of a pure DC tone through this FFT, as a
   fraction of full scale: centre 1.00, +-2 0.265, +-3 0.134, +-4 0.050, +-5 0.
   Anchoring at +-3 knocks the line down to about an eighth of full scale while
   touching five bins, roughly 2.3 kHz of a 12.5 kHz channel at the 240 kHz
   span. Going wider would flatten the spur completely and take the middle of a
   real C4FM carrier with it, which is the worse failure for a view whose job
   is showing where the energy is. The demodulator is unaffected either way:
   it runs its own DC blocker (dsp_pipeline.c). */
#define P25_SPECTRUM_DC_NOTCH_BINS 2

static void notch_dc(float *db, int bins)
{
    const int centre = bins / 2;
    const int lo = centre - P25_SPECTRUM_DC_NOTCH_BINS - 1;
    const int hi = centre + P25_SPECTRUM_DC_NOTCH_BINS + 1;
    if (!db || lo < 0 || hi >= bins) return;
    const float a = db[lo], b = db[hi];
    const float step = (b - a) / (float)(hi - lo);
    for (int i = lo + 1; i < hi; i++) db[i] = a + step * (float)(i - lo);
}

static uint32_t          s_filter_hz;
static unsigned          s_stride_blocks;
static p25_spectrum_snapshot_t s_snapshot;

void p25_spectrum_init(void)
{
    spectrum_init();
    spectrum_reset();
    memset(s_pub, 0, sizeof(s_pub));
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_enabled       = false;
    s_ready         = false;
    s_rx_epoch      = 0;
    s_pub_epoch     = 0;
    s_center_hz     = 0;
    s_span_hz       = 0;
    s_filter_hz     = 0;
    s_stride_blocks = 0;
    __atomic_store_n(&s_pub_seq, 0u, __ATOMIC_RELEASE);
    __atomic_fetch_add(&s_epoch, 1u, __ATOMIC_ACQ_REL);
}

void p25_spectrum_enable(bool enabled)
{
    bool old = __atomic_exchange_n(&s_enabled, enabled, __ATOMIC_ACQ_REL);
    if (old == enabled) return;
    /* Epoch invalidation leaves the RX task as the only publication writer.
     * A UI-side seqlock write here could overlap an FFT publication and make
     * an even sequence visible while the buffer was still changing. */
    __atomic_fetch_add(&s_epoch, 1u, __ATOMIC_ACQ_REL);
}

bool p25_spectrum_enabled(void)
{
    return __atomic_load_n(&s_enabled, __ATOMIC_ACQUIRE);
}

void p25_spectrum_invalidate(void)
{
    __atomic_fetch_add(&s_epoch, 1u, __ATOMIC_ACQ_REL);
}

p25_spectrum_feed_result_t p25_spectrum_feed_iq(
    p25_spectrum_owner_t owner, const uint8_t *iq, int len,
    uint32_t center_hz, uint32_t span_hz, uint32_t filter_hz,
    bool following_voice, uint32_t now_ms)
{
    if (!p25_spectrum_enabled()) return P25_SPECTRUM_FEED_DISABLED;
    if (owner != P25_SPECTRUM_OWNER_RX)
        return P25_SPECTRUM_FEED_NOT_OWNER;
    if (!iq || len < P25_SPECTRUM_BINS * 2 || center_hz == 0 || span_hz == 0)
        return P25_SPECTRUM_FEED_INVALID;

    uint32_t epoch = __atomic_load_n(&s_epoch, __ATOMIC_ACQUIRE);
    if (s_rx_epoch != epoch || s_center_hz != center_hz ||
        s_span_hz != span_hz || s_filter_hz != filter_hz) {
        /* Only this RX owner touches the shared accumulator. A retune or a
         * newly visible view starts with a clean FFT generation. */
        spectrum_reset();
        s_rx_epoch      = epoch;
        s_center_hz     = center_hz;
        s_span_hz       = span_hz;
        s_filter_hz     = filter_hz;
        s_stride_blocks = 0;
    }

    if (++s_stride_blocks < P25_SPECTRUM_BLOCK_STRIDE)
        return P25_SPECTRUM_FEED_DEFERRED;
    s_stride_blocks = 0;

    spectrum_accum(iq, len);
    if (!spectrum_read_db(s_db, P25_SPECTRUM_BINS)) {
        spectrum_reset();
        return P25_SPECTRUM_FEED_INVALID;
    }
    spectrum_reset();
    notch_dc(s_db, P25_SPECTRUM_BINS);

    float range = P25_SPECTRUM_TOP_DB - P25_SPECTRUM_FLOOR_DB;
    __atomic_fetch_add(&s_pub_seq, 1u, __ATOMIC_ACQ_REL); /* odd: writing */
    for (int i = 0; i < P25_SPECTRUM_BINS; i++) {
        float value = (s_db[i] - P25_SPECTRUM_FLOOR_DB) / range;
        if (value < 0.0f) value = 0.0f;
        if (value > 1.0f) value = 1.0f;
        s_pub[i] = value;
    }
    s_snapshot.center_hz       = center_hz;
    s_snapshot.span_hz         = span_hz;
    s_snapshot.filter_hz       = filter_hz;
    s_snapshot.captured_ms     = now_ms;
    s_snapshot.sequence++;
    s_snapshot.fft_bins        = P25_SPECTRUM_BINS;
    s_snapshot.blocks_per_fft  = P25_SPECTRUM_BLOCK_STRIDE;
    s_snapshot.following_voice = following_voice;
    s_pub_epoch = epoch;
    s_ready = true;
    __atomic_fetch_add(&s_pub_seq, 1u, __ATOMIC_RELEASE); /* even: stable */
    return P25_SPECTRUM_FEED_PUBLISHED;
}

bool p25_spectrum_read(float *out, int n, uint32_t now_ms,
                       uint32_t max_age_ms,
                       p25_spectrum_snapshot_t *snapshot)
{
    if (!out || n < 1 || !p25_spectrum_enabled()) return false;
    uint32_t epoch = __atomic_load_n(&s_epoch, __ATOMIC_ACQUIRE);

    for (int attempt = 0; attempt < 3; attempt++) {
        uint32_t before = __atomic_load_n(&s_pub_seq, __ATOMIC_ACQUIRE);
        if ((before & 1u) || !s_ready || s_pub_epoch != epoch) return false;
        p25_spectrum_snapshot_t meta = s_snapshot;
        if ((uint32_t)(now_ms - meta.captured_ms) > max_age_ms) return false;

        for (int o = 0; o < n; o++) {
            int lo = (int)(((int64_t)o * P25_SPECTRUM_BINS) / n);
            int hi = (int)(((int64_t)(o + 1) * P25_SPECTRUM_BINS) / n);
            if (hi <= lo) hi = lo + 1;
            if (hi > P25_SPECTRUM_BINS) hi = P25_SPECTRUM_BINS;
            float peak = 0.0f;
            for (int i = lo; i < hi; i++)
                if (s_pub[i] > peak) peak = s_pub[i];
            out[o] = peak;
        }

        uint32_t after = __atomic_load_n(&s_pub_seq, __ATOMIC_ACQUIRE);
        if (before == after && !(after & 1u)) {
            if (snapshot) *snapshot = meta;
            return true;
        }
    }
    return false;
}

bool p25_spectrum_tap_hz(int x, int width, uint32_t center_hz,
                         uint32_t span_hz, uint32_t tuner_min_hz,
                         uint32_t tuner_max_hz, uint32_t *out_hz)
{
    if (!out_hz || width < 2 || x < 0 || x >= width || span_hz == 0 ||
        tuner_min_hz > tuner_max_hz)
        return false;

    int64_t low = (int64_t)center_hz - (int64_t)span_hz / 2;
    int64_t hz = low + ((int64_t)span_hz * x) / (width - 1);
    if (hz < (int64_t)tuner_min_hz || hz > (int64_t)tuner_max_hz ||
        hz < 0 || hz > UINT32_MAX)
        return false;
    *out_hz = (uint32_t)hz;
    return true;
}
