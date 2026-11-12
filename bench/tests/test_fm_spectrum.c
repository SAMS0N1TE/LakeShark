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

/* Zoom: a tone 3 kHz above a view centre 40 kHz off the dongle's centre. */
static uint8_t s_zoom_iq[8192 * 2];
static void zoom_fill(double tone_hz, double amp, double blocker_hz, double blocker_amp)
{
    for (int i = 0; i < 8192; ++i) {
        const double a = 6.283185307179586 * tone_hz * i / 256000.0;
        const double b = 6.283185307179586 * blocker_hz * i / 256000.0;
        const double re = amp * cos(a) + blocker_amp * cos(b), im = amp * sin(a) + blocker_amp * sin(b);
        s_zoom_iq[2 * i] = (uint8_t)lrint(127.5 + re);
        s_zoom_iq[2 * i + 1] = (uint8_t)lrint(127.5 + im);
    }
}
static int peak_of(const float *bins)
{
    int peak = 0;
    for (int i = 1; i < FM_SPECTRUM_BINS; ++i) if (bins[i] > bins[peak]) peak = i;
    return peak;
}

LS_CASE(zoom_narrows_the_span_around_the_view_and_keeps_the_tone_where_it_is)
{
    float bins[FM_SPECTRUM_BINS];
    fm_spectrum_snapshot_t meta;
    spectrum_init();
    fm_spectrum_enable(true);
    zoom_fill(43000, 60, 0, 0);
    LS_CHECK(fm_spectrum_zoom(16, 100040000));
    fm_spectrum_feed(s_zoom_iq, sizeof(s_zoom_iq), 100000000, 256000, 2000);
    LS_CHECK(fm_spectrum_read(bins, FM_SPECTRUM_BINS, 2000, &meta));
    LS_EQ_UINT(100040000, meta.center_hz);
    LS_EQ_UINT(16000, meta.span_hz);
    /* 16 kHz over 256 bins is 62.5 Hz a bin; +3 kHz is 48 bins right of 128. */
    const int peak = peak_of(bins);
    LS_CHECK(peak >= 175 && peak <= 177);
    LS_CHECK(bins[peak] > 0.75f);
    /* Sharp: a few bins away it has fallen a long way. */
    LS_CHECK(bins[peak] - bins[peak + 8] > 0.25f);
    fm_spectrum_zoom(1, 0);
    fm_spectrum_enable(false);
}

LS_CASE(zoom_keeps_a_strong_signal_outside_the_view_out_of_it)
{
    float bins[FM_SPECTRUM_BINS];
    fm_spectrum_snapshot_t meta;
    spectrum_init();
    fm_spectrum_enable(true);
    /* Nothing in the 16 kHz view; a carrier 30 kHz off the view centre. */
    zoom_fill(0, 0, 70000, 100);
    LS_CHECK(fm_spectrum_zoom(16, 100040000));
    fm_spectrum_feed(s_zoom_iq, sizeof(s_zoom_iq), 100000000, 256000, 3000);
    LS_CHECK(fm_spectrum_read(bins, FM_SPECTRUM_BINS, 3000, &meta));
    float top = 0;
    for (int i = 0; i < FM_SPECTRUM_BINS; ++i) if (bins[i] > top) top = bins[i];
    /* Full scale reads near 1.0 (0 dBFS); an alias would sit within 30 dB of it. */
    LS_CHECK(top < 0.6f);
    fm_spectrum_zoom(1, 0);
    fm_spectrum_enable(false);
}

LS_CASE(zoom_view_falls_back_to_the_dongle_centre_after_a_long_retune)
{
    float bins[FM_SPECTRUM_BINS];
    fm_spectrum_snapshot_t meta;
    spectrum_init();
    fm_spectrum_enable(true);
    zoom_fill(1000, 60, 0, 0);
    LS_CHECK(fm_spectrum_zoom(4, 100040000));
    fm_spectrum_feed(s_zoom_iq, sizeof(s_zoom_iq), 162400000, 256000, 4000);
    LS_CHECK(fm_spectrum_read(bins, FM_SPECTRUM_BINS, 4000, &meta));
    LS_EQ_UINT(162400000, meta.center_hz);
    LS_EQ_UINT(64000, meta.span_hz);
    LS_CHECK(!fm_spectrum_zoom(3, 0));
    fm_spectrum_zoom(1, 0);
    fm_spectrum_enable(false);
}

/* The droop is put back: a tone near the edge of a zoomed view reads as
   strong as the same tone at its centre. */
static float tone_level(double off_hz)
{
    float bins[FM_SPECTRUM_BINS];
    fm_spectrum_snapshot_t meta;
    static unsigned t = 9000;
    zoom_fill(40000 + off_hz, 60, 0, 0);
    fm_spectrum_invalidate();
    fm_spectrum_feed(s_zoom_iq, sizeof(s_zoom_iq), 100000000, 256000, t += 1000);
    LS_CHECK(fm_spectrum_read(bins, FM_SPECTRUM_BINS, t, &meta));
    return bins[peak_of(bins)] * (FM_SPECTRUM_TOP_DB - FM_SPECTRUM_FLOOR_DB);
}
LS_CASE(zoom_reads_a_tone_at_the_edge_as_strong_as_at_the_centre)
{
    spectrum_init();
    fm_spectrum_enable(true);
    LS_CHECK(fm_spectrum_zoom(8, 100040000));
    const float centre = tone_level(0), edge = tone_level(14000);   /* 32 kHz view */
    LS_CHECK_MSG(fabsf(centre - edge) < 0.6f, "centre %.2f dB, edge %.2f dB", centre, edge);
    fm_spectrum_zoom(1, 0);
    fm_spectrum_enable(false);
}
