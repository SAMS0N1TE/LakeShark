/* P25 Phase 1 voice, end to end, on generated calls.
 *
 * bench/fixtures/p25_call_gen.c builds a real call - HDU, LDUs carrying
 * known IMBE frames, TDU - as C4FM IQ, and this runs it through the
 * production receive path: dsp_pipeline, the sync hunt, processFrame and the
 * HDU/LDU/TDU processors, the ESS gate, and p25_voice_hold exactly as
 * app_p25.c drives it. The vocoder is replaced by a recorder, so what these
 * cases check is bit-exact: which IMBE frames reached the speaker, in what
 * order, and which were held, released or dropped.
 *
 * Every case here is a way voice has actually failed on the air (see
 * docs/P25_VOICE.md), and each asserts the number of frames heard, because
 * "it still decodes" was true every time voice was choppy.
 */
#include "ls_test.h"
#include "dsd.h"
#include "dsp_pipeline.h"
#include "imbe_shim.h"
#include "p25_call_gen.h"
#include "p25_voice_hold.h"
#include <stdlib.h>
#include <string.h>

int autoscan_bch_ok_flag;
int dsd_bch_fail_counter;
void sys_log(unsigned char color, const char *fmt, ...) { (void)color; (void)fmt; }
void audio_beep_request(int kind) { (void)kind; }

/* ------------------------------------------------ the vocoder, recorded */

#define MAX_FRAMES 512
static uint8_t s_decoded[MAX_FRAMES][P25_CALL_IMBE_BYTES];
static int s_n_decoded;

void imbe_shim_init(void) {}
bool imbe_shim_try_decode_88(const uint8_t *in, int16_t *out)
{
    if (s_n_decoded < MAX_FRAMES) memcpy(s_decoded[s_n_decoded], in, P25_CALL_IMBE_BYTES);
    s_n_decoded++;
    for (int i = 0; i < 160; i++) out[i] = 1000;
    return true;
}
void imbe_shim_decode_88(const uint8_t *in, int16_t *out) { (void)imbe_shim_try_decode_88(in, out); }

/* ------------------------------------------------------------- receiver */

static struct {
    dsp_state_t dsp;
    dsd_sample_ring_t ring;
    dsd_opts opts;
    dsd_state state;
    int dibits[10000];
    short audio[2000];
    float audio_float[2000];
    short pcm[2000];
    mbe_parms cur, prev, enhanced;
    p25_voice_hold_t hold;
    int16_t out[P25_HOLD_MAX_SAMPLES + 2000];
    const uint8_t *iq;
    size_t iq_bytes, offset;
} rx;

/* LS_P25_DUMP=<prefix>: the demodulator's output (int16, 48 kHz) and the
   transmitted symbols, for looking at the eye by hand. Off by default. */
static FILE *s_dump;

void dsd_yield(void)
{
    if (rx.offset >= rx.iq_bytes) { exitflag = 1; dsd_abort = 1; return; }
    int16_t demod[4096];
    size_t n = rx.iq_bytes - rx.offset;
    if (n > 2048) n = 2048;
    int got = dsp_process_iq(&rx.dsp, rx.iq + rx.offset, (int)n, demod, 4096);
    rx.offset += n;
    if (s_dump && got > 0) fwrite(demod, sizeof(int16_t), (size_t)got, s_dump);
    for (int i = 0; i < got; i++) {
        int next = (rx.ring.write_idx + 1) % DSD_SAMPLE_RING_SIZE;
        if (next == rx.ring.read_idx) break;
        rx.ring.buf[rx.ring.write_idx] = demod[i];
        rx.ring.write_idx = next;
    }
}

static double now_s(void)
{
    int queued = (rx.ring.write_idx - rx.ring.read_idx + DSD_SAMPLE_RING_SIZE) % DSD_SAMPLE_RING_SIZE;
    return (double)rx.offset / 480000.0 - queued / 48000.0;
}

typedef struct {
    int hdu, ldu1, ldu2, tdu;
    int played;             /* IMBE frames the speaker got */
    int decoded;            /* IMBE frames the vocoder got */
    double first_play_s;    /* air time of the frame that first reached it */
    int first_play_frame;   /* ...as a frame index: 0 HDU or first LDU1 */
    uint32_t held, released, discarded;
    int tg;
    int hdr_fixed, hdr_critical, imbe_bit_fixes;   /* FEC work: the margin left */
} result_t;

static void receive(const uint8_t *iq, int bytes, result_t *r)
{
    memset(&rx, 0, sizeof(rx));
    memset(r, 0, sizeof(*r));
    r->first_play_s = -1;
    s_n_decoded = 0;
    rx.state.dibit_buf = rx.dibits;
    rx.state.audio_out_buf = rx.audio;
    rx.state.audio_out_float_buf = rx.audio_float;
    rx.state.cur_mp = &rx.cur; rx.state.prev_mp = &rx.prev; rx.state.prev_mp_enhanced = &rx.enhanced;
    initState(&rx.state);
    initOpts(&rx.opts);
    rx.opts.mod_qpsk = 0; rx.opts.mod_gfsk = 0;
    rx.opts.ring = &rx.ring;
    rx.state.pcm_out_buf = rx.pcm;
    rx.state.pcm_out_size = 2000;
    dsp_init(&rx.dsp);
    dsp_set_mode(&rx.dsp, DEMOD_C4FM);
    dsp_set_gain(&rx.dsp, -9000.0f);
    p25_voice_hold_reset(&rx.hold);
    rx.iq = iq; rx.iq_bytes = (size_t)bytes;
    exitflag = 0; dsd_abort = 0;

    while (!exitflag) {
        int sync = getFrameSync(&rx.opts, &rx.state);
        double t = now_s();
        if (sync < 0 || exitflag) { p25_voice_hold_tick(&rx.hold, (uint32_t)(t * 1000)); continue; }
        rx.state.pcm_out_write = 0;
        rx.state.pcm_out_unproven = 0;
        processFrame(&rx.opts, &rx.state);
        if (!rx.state.p25_frame_valid) { p25_voice_hold_tick(&rx.hold, (uint32_t)(t * 1000)); continue; }
        uint8_t duid = rx.state.p25_frame_duid;
        if (duid == 0) r->hdu++;
        if (duid == 5) r->ldu1++;
        if (duid == 10) r->ldu2++;
        if (duid == 3 || duid == 15) r->tdu++;
        /* app_p25.c, the decoder task, as written */
        if (duid == 0 || duid == 3 || duid == 15) p25_voice_hold_end_call(&rx.hold);
        int n = p25_voice_hold_frame(&rx.hold, rx.pcm, rx.state.pcm_out_write,
            rx.state.pcm_out_unproven, rx.state.p25_ess_valid, rx.state.p25_algid,
            (uint32_t)rx.state.lasttg, (uint32_t)(t * 1000), rx.out,
            (int)(sizeof(rx.out) / sizeof(rx.out[0])));
        if (n > 0 && r->first_play_s < 0) r->first_play_s = t;
        r->played += n / 160;
        if (rx.state.lasttg) r->tg = rx.state.lasttg;
    }
    r->decoded = s_n_decoded;
    r->held = rx.hold.held_frames;
    r->released = rx.hold.released_frames;
    r->discarded = rx.hold.discarded_frames;
    r->hdr_fixed = rx.state.debug_header_errors;
    r->hdr_critical = rx.state.debug_header_critical_errors;
    r->imbe_bit_fixes = rx.state.debug_audio_errors;
}

/* ------------------------------------------------------------- the calls */

#define CALL_PAIRS 6
#define CALL_FRAMES (CALL_PAIRS * 18)
static uint8_t s_voice[CALL_FRAMES][P25_CALL_IMBE_BYTES];

static void make_voice(void)
{
    ls_rng_t rng;
    ls_rng_seed(&rng, 0x25u);
    for (int f = 0; f < CALL_FRAMES; f++)
        for (int b = 0; b < P25_CALL_IMBE_BYTES; b++)
            s_voice[f][b] = (uint8_t)ls_rng_u32(&rng);
}

static p25_call_t clear_call(void)
{
    p25_call_t c = { .nac = 0x527, .talkgroup = 101, .source = 4242,
                     .algid = 0x80, .kid = 0, .hdu = 1, .tdu = 1 };
    return c;
}

static p25_call_channel_t quiet_channel(void)
{
    p25_call_channel_t ch;
    memset(&ch, 0, sizeof(ch));
    ch.noise_sd = 0.02f;
    ch.lead_s = 0.30f;
    ch.tail_s = 0.30f;
    return ch;
}

static int s_sym[40000];
static uint8_t *s_iq;
static int s_iq_max;

static void on_air(const p25_call_t *c, const p25_call_channel_t *ch, uint32_t seed, result_t *r)
{
    make_voice();
    int n = p25_call_symbols(c, s_voice, CALL_FRAMES, s_sym, (int)(sizeof(s_sym) / sizeof(s_sym[0])));
    LS_CHECK(n > 0);
    int bytes = p25_call_render_bytes(n, ch);
    if (bytes > s_iq_max) {
        free(s_iq);
        s_iq = malloc((size_t)bytes);
        s_iq_max = bytes;
    }
    LS_CHECK(s_iq != NULL);
    ls_rng_t rng;
    ls_rng_seed(&rng, seed);
    LS_EQ_INT(p25_call_render(s_sym, n, ch, &rng, s_iq, s_iq_max), bytes);
    const char *dump = getenv("LS_P25_DUMP");
    if (dump && seed == 1) {
        char path[512];
        snprintf(path, sizeof(path), "%s.syms", dump);
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "%f\n", ch->lead_s);
            for (int i = 0; i < n; i++) fprintf(f, "%d\n", s_sym[i]);
            fclose(f);
        }
        snprintf(path, sizeof(path), "%s.demod", dump);
        s_dump = fopen(path, "wb");
    }
    receive(s_iq, bytes, r);
    if (s_dump) { fclose(s_dump); s_dump = NULL; }
}

/* Of the IMBE frames the vocoder was handed, how many are exactly what was
   sent. Played counts frames; this counts voice that is not garbled, which
   is what degrades first. Valid while no frame was lost, so the decoded
   sequence lines up with the sent one. */
static double exact_fraction(void)
{
    int exact = 0;
    for (int f = 0; f < s_n_decoded && f < CALL_FRAMES; f++)
        exact += !memcmp(s_decoded[f], s_voice[f], P25_CALL_IMBE_BYTES);
    return (double)exact / CALL_FRAMES;
}

LS_CASE(a_clean_call_is_heard_whole)
{
    p25_call_t c = clear_call();
    p25_call_channel_t ch = quiet_channel();
    result_t r;
    on_air(&c, &ch, 1, &r);
    ls_note("clean: HDU %d LDU1 %d LDU2 %d TDU %d, played %d of %d, exact %.0f%%; "
            "FEC fixed %d header words (%d beyond repair), %d IMBE bits",
            r.hdu, r.ldu1, r.ldu2, r.tdu, r.played, CALL_FRAMES, 100 * exact_fraction(),
            r.hdr_fixed, r.hdr_critical, r.imbe_bit_fixes);
    LS_EQ_INT(r.ldu1, CALL_PAIRS);
    LS_EQ_INT(r.ldu2, CALL_PAIRS);
    LS_EQ_INT(r.tdu, 1);
    LS_EQ_INT(r.played, CALL_FRAMES);
    LS_EQ_INT(r.tg, 101);
    /* Not 100%: C4FM symbol timing leaves a floor of bit errors even with
       no noise (docs/P25_VOICE.md, "timing floor"). Measured 77% over five
       calls; this is the floor a change must not go under. */
    LS_CHECK(exact_fraction() >= 0.68);
}

LS_CASE(the_hdu_proves_the_call_clear_so_the_first_ldu1_plays_at_once)
{
    /* Twenty calls across +-800 Hz: the HDU is found and its ESS lets the
       first LDU1 play without waiting for LDU2. Measured 20 found, 18 not
       held. */
    int found = 0, not_held = 0;
    for (int seed = 100; seed < 120; seed++) {
        p25_call_t c = clear_call();
        p25_call_channel_t ch = quiet_channel();
        ch.noise_sd = 0.05f;
        ch.freq_off_hz = (float)((seed * 137) % 1600) - 800.0f;
        result_t r;
        on_air(&c, &ch, (uint32_t)seed, &r);
        found += r.hdu;
        not_held += r.held == 0;
        LS_EQ_INT(r.played, CALL_FRAMES);
    }
    ls_note("HDU found in %d of 20 calls; first LDU1 played at once in %d", found, not_held);
    LS_CHECK(found >= 18);
    LS_CHECK(not_held >= 15);
}

LS_CASE(a_call_joined_without_its_hdu_still_plays_its_first_ldu1)
{
    p25_call_t c = clear_call();
    c.hdu = 0;
    p25_call_channel_t ch = quiet_channel();
    result_t r;
    on_air(&c, &ch, 3, &r);
    LS_EQ_INT(r.played, CALL_FRAMES);
    LS_EQ_UINT(r.held, 9);          /* the first LDU1, until LDU2's ESS */
    LS_EQ_UINT(r.released, 9);
    LS_EQ_UINT(r.discarded, 0);
}

LS_CASE(an_encrypted_call_never_reaches_the_speaker)
{
    for (int with_hdu = 0; with_hdu <= 1; with_hdu++) {
        p25_call_t c = clear_call();
        c.algid = 0x84;             /* AES-256 */
        c.kid = 0x1234;
        c.hdu = with_hdu;
        p25_call_channel_t ch = quiet_channel();
        result_t r;
        on_air(&c, &ch, 4, &r);
        ls_note("encrypted, hdu=%d: played %d, held %u discarded %u", with_hdu, r.played,
                (unsigned)r.held, (unsigned)r.discarded);
        LS_EQ_INT(r.played, 0);
    }
}

LS_CASE(a_lost_ldu1_costs_its_own_frames_and_nothing_more)
{
    p25_call_t c = clear_call();
    c.corrupt_ldu1 = 3;             /* mid-call */
    p25_call_channel_t ch = quiet_channel();
    result_t r;
    on_air(&c, &ch, 5, &r);
    ls_note("lost LDU1: LDU1 %d LDU2 %d played %d", r.ldu1, r.ldu2, r.played);
    /* The orphaned LDU2 after it is decoded, and the hunt lands on the
       LDU1 after that - no cascade (it was 720 ms per missed sync). */
    LS_EQ_INT(r.ldu2, CALL_PAIRS);
    LS_EQ_INT(r.ldu1, CALL_PAIRS - 1);
    LS_EQ_INT(r.played, CALL_FRAMES - 9);
}

LS_CASE(a_carrier_that_arrives_off_frequency_is_heard_from_its_first_ldu)
{
    /* The 154.7850 failure: DC from the carrier's offset still settling
       while the first frames arrive, sliced at zero, and every call lost the
       LDU1 after its HDU. A transmission opens with its HDU, so the first
       LDU1 is 82 ms into the carrier - the case that broke. (A carrier that
       opens directly on an LDU1 still loses it about half the time: the sync
       is its first 24 symbols and nothing has settled; docs/P25_VOICE.md.) */
    const float offsets[] = { -1200.0f, -600.0f, 600.0f, 1200.0f };
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        p25_call_t c = clear_call();
        p25_call_channel_t ch = quiet_channel();
        ch.freq_off_hz = offsets[i] / 2;
        ch.keyup_hz = offsets[i];
        ch.keyup_tau_s = 0.05f;
        result_t r;
        on_air(&c, &ch, 6 + i, &r);
        ls_note("key-up %+.0f Hz settling to %+.0f: LDU1 %d played %d", offsets[i],
                offsets[i] / 2, r.ldu1, r.played);
        LS_EQ_INT(r.ldu1, CALL_PAIRS);
        LS_EQ_INT(r.played, CALL_FRAMES);
    }
}

LS_CASE(a_transmitter_clock_off_by_100_ppm_is_heard_whole)
{
    const float ppm[] = { -100.0f, 100.0f };
    for (unsigned i = 0; i < 2; i++) {
        p25_call_t c = clear_call();
        p25_call_channel_t ch = quiet_channel();
        ch.clock_ppm = ppm[i];
        result_t r;
        on_air(&c, &ch, 10 + i, &r);
        ls_note("clock %+.0f ppm: played %d", ppm[i], r.played);
        LS_EQ_INT(r.played, CALL_FRAMES);
    }
}

LS_CASE(voice_quality_under_noise_does_not_fall_below_its_measured_floor)
{
    /* noise_sd 0.3 / 0.5 / 0.7 / 1.0 are about 20 / 16 / 13 / 10 dB in the
       12.5 kHz channel; the 154.7850 capture sits between 12 and 19. Floors
       are the measured exact-frame share (5 calls each: 69 / 57 / 45 / 30%)
       less a margin. Raising them is how an improvement is kept. */
    const struct { float sd; double exact; int played_pct; } levels[] = {
        { 0.3f, 0.60, 100 }, { 0.5f, 0.48, 100 }, { 0.7f, 0.36, 100 }, { 1.0f, 0.22, 95 },
    };
    for (unsigned i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        int played = 0;
        double exact = 0;
        for (int seed = 200; seed < 205; seed++) {
            p25_call_t c = clear_call();
            p25_call_channel_t ch = quiet_channel();
            ch.noise_sd = levels[i].sd;
            result_t r;
            on_air(&c, &ch, (uint32_t)seed, &r);
            played += r.played;
            exact += exact_fraction();
        }
        ls_note("noise sd %.1f: played %d%%, exact %.0f%%", levels[i].sd,
                100 * played / (5 * CALL_FRAMES), 100 * exact / 5);
        LS_CHECK(100 * played >= levels[i].played_pct * 5 * CALL_FRAMES);
        LS_CHECK(exact / 5 >= levels[i].exact);
    }
}

LS_CASE(an_overloaded_tuner_loses_only_the_burst)
{
    /* A LoRa transmitter beside the board compressed the tuner: the wanted
       signal fell ~8 dB against the noise for each packet. What is lost must
       be what the burst covered - the decoder must be back on the next
       frame, not hunting for seconds. */
    p25_call_t c = clear_call();
    p25_call_channel_t ch = quiet_channel();
    ch.noise_sd = 0.5f;
    ch.n_fades = 1;
    ch.fades[0].start_s = ch.lead_s + 0.60f;
    ch.fades[0].len_s = 0.40f;
    ch.fades[0].gain_db = -14.0f;
    result_t r;
    on_air(&c, &ch, 30, &r);
    ls_note("14 dB burst for 400 ms: LDU1 %d LDU2 %d played %d of %d", r.ldu1, r.ldu2,
            r.played, CALL_FRAMES);
    /* 400 ms touches at most four LDUs. */
    LS_CHECK(r.played >= CALL_FRAMES - 4 * 9);
}
