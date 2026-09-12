#include "ls_test.h"
#include "fm_spectrum.h"
#include "spectrum.h"
#include <math.h>

LS_CASE(live_spectrum_tracks_tone_and_expires_after_stop_or_retune)
{
    uint8_t iq[SPEC_FFT_N * 2];
    float bins[FM_SPECTRUM_BINS];
    fm_spectrum_snapshot_t meta;
    for (int i = 0; i < SPEC_FFT_N; ++i) {
        double phase = 6.283185307179586 * 32 * i / SPEC_FFT_N;
        iq[2 * i] = (uint8_t)(127.5 + 80 * cos(phase));
        iq[2 * i + 1] = (uint8_t)(127.5 + 80 * sin(phase));
    }
    spectrum_init();
    fm_spectrum_enable(true);
    fm_spectrum_feed(iq, sizeof(iq), 100000000, 256000, 1000);
    LS_CHECK(fm_spectrum_read(bins, FM_SPECTRUM_BINS, 1001, &meta));
    LS_EQ_UINT(100000000, meta.center_hz);
    int peak = 0;
    for (int i = 1; i < FM_SPECTRUM_BINS; ++i)
        if (bins[i] > bins[peak]) peak = i;
    LS_EQ_INT(144, peak);
    LS_CHECK(bins[peak] > 0.9f);
    uint32_t sequence = meta.sequence;
    fm_spectrum_feed(iq, sizeof(iq), 100000000, 256000, 1010);
    LS_CHECK(fm_spectrum_read(bins, FM_SPECTRUM_BINS, 1010, &meta));
    LS_EQ_UINT(sequence, meta.sequence);
    LS_CHECK(!fm_spectrum_read(bins, FM_SPECTRUM_BINS, 1501, &meta));
    fm_spectrum_invalidate();
    LS_CHECK(!fm_spectrum_read(bins, FM_SPECTRUM_BINS, 1002, &meta));
    fm_spectrum_feed(iq, sizeof(iq), 101000000, 256000, 1011);
    LS_CHECK(fm_spectrum_read(bins, FM_SPECTRUM_BINS, 1011, &meta));
    LS_EQ_UINT(101000000, meta.center_hz);
    fm_spectrum_enable(false);
    LS_CHECK(!fm_spectrum_read(bins, FM_SPECTRUM_BINS, 1012, &meta));
}
