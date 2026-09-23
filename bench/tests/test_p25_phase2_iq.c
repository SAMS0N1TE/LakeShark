/* Phase II from IQ, not from recovered symbols.
 *
 * The public mixed Phase I/II recording (docs/P25_PHASE2.md) is recovered
 * dibits, so everything checked against it so far started after the
 * demodulator. This takes a stretch of it, decodes those dibits directly as
 * the reference, then modulates the same dibits as 6000-baud pi/4-DQPSK IQ
 * (lsm_gen) and runs them through the production front end - dsp_pipeline in
 * Phase II mode, p25_p2_slice, the decoder - and asks for the same voice.
 * That is the half of Phase II that has never seen a signal.
 *
 * The recording is not redistributed: set LS_P25P2_CAPTURE to its path
 * (bench/tools/check_p25p2_sample.py fetches it and does). Without it this
 * says so and passes.
 */
#include "dsp_pipeline.h"
#include "ls_test.h"
#include "lsm_gen.h"
#include "p25_p2_slice.h"
#include "p25_phase2.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sys_log(unsigned char color, const char *fmt, ...) { (void)color; (void)fmt; }

#define WACN 0x92715
#define SYSID 0x1f6
#define NAC 0x01a
#define SPAN_START   150000     /* the recording's Phase II voice starts ~120000 */
#define SPAN_SYMBOLS 120000     /* 20 s of it */

typedef struct { int16_t *pcm; size_t n, cap; } pcm_sink_t;

static void sink(const int16_t *pcm, size_t count, void *context)
{
    pcm_sink_t *s = context;
    if (s->n + count > s->cap) return;
    memcpy(s->pcm + s->n, pcm, count * sizeof(int16_t));
    s->n += count;
}

static uint8_t *load(size_t *n)
{
    const char *path = getenv("LS_P25P2_CAPTURE");
    if (!path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *d = malloc(SPAN_SYMBOLS);
    *n = d && fseek(f, SPAN_START, SEEK_SET) == 0 ? fread(d, 1, SPAN_SYMBOLS, f) : 0;
    fclose(f);
    return d;
}

static p25p2_status_t decode_dibits(const uint8_t *d, size_t n, pcm_sink_t *out)
{
    p25p2_decoder_t *dec = p25p2_create(sink, out);
    LS_CHECK(dec != NULL);
    p25p2_status_t st = {0};
    if (!dec) return st;
    LS_CHECK(p25p2_configure(dec, WACN, SYSID, NAC, 0));
    p25p2_push(dec, d, n);
    p25p2_status(dec, &st);
    p25p2_destroy(dec);
    return st;
}

/* The same dibits as the receiver would see them off the air. */
static p25p2_status_t decode_iq(const uint8_t *d, size_t n, const lsm_channel_t *ch,
                                pcm_sink_t *out)
{
    static const int LEVELS[4] = { 1, 3, -1, -3 };
    const int iq_bytes = (int)n * 2 * (DSP_SAMPLE_RATE * DSP_PRE_DECIM / 6000);
    int *tx = malloc(n * sizeof(int));
    uint8_t *iq = malloc((size_t)iq_bytes);
    p25p2_status_t st = {0};
    LS_CHECK(tx && iq);
    if (!tx || !iq) { free(tx); free(iq); return st; }
    for (size_t i = 0; i < n; i++) tx[i] = LEVELS[d[i] & 3];
    ls_rng_t rng;
    ls_rng_seed(&rng, 2025);
    LS_EQ_INT(lsm_render_iq(tx, (int)n, ch, &rng, iq, iq_bytes), iq_bytes);

    p25p2_decoder_t *dec = p25p2_create(sink, out);
    LS_CHECK(dec && p25p2_configure(dec, WACN, SYSID, NAC, 0));
    /* p25_p2_rx, as the runtime sets the demodulator up for Phase II */
    static dsp_state_t dsp;
    dsp_init(&dsp);
    dsp.phase2 = true;
    dsp_set_gain(&dsp, fabsf(dsp.demod_gain));
    dsp_set_mode(&dsp, DEMOD_CQPSK);
    dsp_reset_cqpsk_loops(&dsp);
    static int16_t audio[8192];
    static uint8_t sym[1024];
    for (int pos = 0; pos < iq_bytes; pos += 16384) {
        int len = iq_bytes - pos < 16384 ? iq_bytes - pos : 16384;
        int count = dsp_process_iq(&dsp, iq + pos, len, audio, 8192);
        int k = p25_p2_slice(audio, count, dsp.demod_gain, sym, 1024);
        if (dec) p25p2_push(dec, sym, (size_t)k);
    }
    if (dec) { p25p2_status(dec, &st); p25p2_destroy(dec); }
    free(tx);
    free(iq);
    return st;
}

LS_CASE(phase2_voice_survives_the_trip_through_iq_and_the_front_end)
{
    size_t n = 0;
    uint8_t *d = load(&n);
    if (!d) {
        ls_note("LS_P25P2_CAPTURE not set - run bench/tools/check_p25p2_sample.py; skipped");
        return;
    }
    LS_EQ_INT((int)n, SPAN_SYMBOLS);

    static int16_t ref_pcm[SPAN_SYMBOLS * 2], iq_pcm[SPAN_SYMBOLS * 2];
    pcm_sink_t ref = { ref_pcm, 0, sizeof(ref_pcm) / 2 };
    p25p2_status_t want = decode_dibits(d, n, &ref);
    ls_note("reference from dibits: voice %u audio %u bursts %u", (unsigned)want.voice_frames,
            (unsigned)want.audio_frames, (unsigned)want.bursts);
    LS_CHECK(want.voice_frames > 300);

    /* Before the CQPSK FLL (dsp_pipeline.c) nothing past +-375 Hz decoded -
       0 voice frames at +-400. An RTL's 1 ppm is 850 Hz at 850 MHz. */
    const struct { const char *name; float off, noise, echo; double min_share; } cases[] = {
        { "clean",       0.0f,    0.0f,  0.0f, 1.00 },
        { "+300 Hz",     300.0f,  0.0f,  0.0f, 0.98 },
        { "-300 Hz",    -300.0f,  0.0f,  0.0f, 0.98 },
        { "+600 Hz",     600.0f,  0.0f,  0.0f, 0.98 },
        { "-600 Hz",    -600.0f,  0.0f,  0.0f, 0.98 },
        { "+900 Hz",     900.0f,  0.0f,  0.0f, 0.98 },
        { "-900 Hz",    -900.0f,  0.0f,  0.0f, 0.98 },
        { "+1200 Hz",   1200.0f,  0.0f,  0.0f, 0.98 },
        { "-1200 Hz",  -1200.0f,  0.0f,  0.0f, 0.98 },
        { "+1600 Hz",   1600.0f,  0.0f,  0.0f, 0.98 },
        { "-1600 Hz",  -1600.0f,  0.0f,  0.0f, 0.98 },
        { "noise 0.15",  0.0f,    0.15f, 0.0f, 0.98 },
        { "noise 0.30",  0.0f,    0.30f, 0.0f, 0.95 },
        { "900 Hz+noise", 900.0f, 0.15f, 0.0f, 0.95 },
        { "echo 0.3",    0.0f,    0.02f, 0.3f, 0.90 },
    };
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        lsm_channel_t ch;
        lsm_channel_clean(&ch);
        ch.freq_off_hz = cases[c].off;
        ch.noise_sd = cases[c].noise;
        ch.echo_amp = cases[c].echo;
        ch.echo_delay_sym = 1.0f;
        pcm_sink_t got_pcm = { iq_pcm, 0, sizeof(iq_pcm) / 2 };
        p25p2_status_t got = decode_iq(d, n, &ch, &got_pcm);
        ls_note("%-10s from IQ: voice %u audio %u bursts %u flips %u mistuned %u", cases[c].name,
                (unsigned)got.voice_frames, (unsigned)got.audio_frames, (unsigned)got.bursts,
                (unsigned)got.polarity_flips, (unsigned)got.mistuned_syncs);
        LS_CHECK(got.voice_frames >= cases[c].min_share * want.voice_frames);
        LS_CHECK(got.audio_frames >= cases[c].min_share * want.audio_frames);
        if (c == 0) {
            /* No channel at all: the audio must be the reference, sample for sample. */
            LS_EQ_INT((int)got_pcm.n, (int)ref.n);
            LS_CHECK(got_pcm.n == ref.n && !memcmp(iq_pcm, ref_pcm, ref.n * sizeof(int16_t)));
        }
    }
    free(d);
}
