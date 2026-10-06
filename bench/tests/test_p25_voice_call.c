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
#include "p25_symbol_synth.h"
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
    const uint8_t *dib;     /* set: symbol bytes (p25_os4_decode.h) synthesized as the LR2021 path does */
    size_t dib_n;
} rx;

/* LS_P25_DUMP=<prefix>: the demodulator's output (int16, 48 kHz) and the
   transmitted symbols, for looking at the eye by hand. Off by default. */
static FILE *s_dump;

void dsd_yield(void)
{
    if (rx.dib) {
        if (rx.offset >= rx.dib_n) { exitflag = 1; dsd_abort = 1; return; }
        for (int k = 0; k < 100 && rx.offset < rx.dib_n; k++, rx.offset++) {
            int16_t s[P25_SYNTH_SAMPLES_PER_SYMBOL];
            const uint8_t b = rx.dib[rx.offset];
            p25_symbol_synth(b & 7u, s);
            for (int i = 0; i < P25_SYNTH_SAMPLES_PER_SYMBOL; i++) {
                rx.ring.buf[rx.ring.write_idx] = s[i];
                rx.ring.aux[rx.ring.write_idx] = (uint8_t)((b >> 3) & 15u);
                rx.ring.write_idx = (rx.ring.write_idx + 1) % DSD_SAMPLE_RING_SIZE;
            }
        }
        return;
    }
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
    if (rx.dib) return (double)rx.offset / 4800.0 - queued / 48000.0;
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

static void receive_from(const uint8_t *iq, int bytes, const uint8_t *dib, size_t dib_n,
                         int erasure_marks, int soft, result_t *r);

/* set: the repeat rule off, so the vocoder gets what FEC made of every frame */
static int s_fec_only;

static void receive(const uint8_t *iq, int bytes, result_t *r)
{
    receive_from(iq, bytes, NULL, 0, 0, 0, r);
}

static void receive_from(const uint8_t *iq, int bytes, const uint8_t *dib, size_t dib_n,
                         int erasure_marks, int soft, result_t *r)
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
    rx.dib = dib; rx.dib_n = dib_n;
    if (dib) {
        rx.opts.play_unproven = 1;      /* as the LR2021 path runs it */
        rx.opts.erasure_marks = erasure_marks;
        rx.opts.soft_symbols = soft;
    }
    if (s_fec_only) rx.opts.imbe_repeat = 0;
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
    /* 77% before the symbol clock stopped dithering across the eye
       (dsd_symbol.c; docs/P25_VOICE.md), 99% after. */
    LS_CHECK(exact_fraction() >= 0.95);
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

/* Inside a call the NID can only be its NAC with LDU1 or LDU2: one bent past
   BCH's eleven bits, but much nearer one of the two than chance, is that
   frame, and its voice plays. */
LS_CASE(a_nid_bent_past_bch_inside_a_call_still_plays_its_ldu)
{
    p25_call_t c = clear_call();
    c.bend_ldu1 = 3;
    p25_call_channel_t ch = quiet_channel();
    result_t r;
    on_air(&c, &ch, 5, &r);
    ls_note("bent LDU1 NID: LDU1 %d LDU2 %d played %d", r.ldu1, r.ldu2, r.played);
    LS_EQ_INT(r.ldu1, CALL_PAIRS);
    LS_EQ_INT(r.played, CALL_FRAMES);
}

/* The LR2021 loses ~150 symbols at every packet seam. Sent as erasures, the
   IMBE frames they fall in are replaced by the last good frame: no frame
   that is not one the radio sent reaches the vocoder. As plain symbols, FEC
   makes some frame of them and it plays. */
static uint8_t s_dib[40000];

static size_t call_dibits(const p25_call_t *c, int cut_at, int lost)
{
    make_voice();
    int n = p25_call_symbols(c, s_voice, CALL_FRAMES, s_sym, (int)(sizeof(s_sym) / sizeof(s_sym[0])));
    LS_CHECK(n > 0 && n <= (int)sizeof(s_dib));
    for (int i = 0; i < n; i++)
        s_dib[i] = s_sym[i] == 3 ? 1 : s_sym[i] == 1 ? 0 : s_sym[i] == -1 ? 2 : 3;
    for (int i = cut_at; i < cut_at + lost && i < n; i++) s_dib[i] = P25_SYNTH_ERASED;
    return (size_t)n;
}

static int sent_frames_only(void)
{
    int strangers = 0;
    for (int f = 0; f < s_n_decoded && f < MAX_FRAMES; f++) {
        int known = 0;
        for (int g = 0; g < CALL_FRAMES && !known; g++)
            known = !memcmp(s_decoded[f], s_voice[g], P25_CALL_IMBE_BYTES);
        strangers += !known;
    }
    return strangers;
}

LS_CASE(symbols_lost_at_a_packet_seam_are_replaced_not_played)
{
    p25_call_t c = clear_call();
    result_t r;
    /* the middle of the third LDU, past its sync, NID and first frames */
    const int cut = 2 * 864 + 300 + 864 / 2;
    size_t n = call_dibits(&c, cut, 150);
    receive_from(NULL, 0, s_dib, n, 1, 0, &r);
    const int strangers = sent_frames_only();
    ls_note("erased: decoded %d, played %d, frames not sent %d, erased frames %u",
            r.decoded, r.played, strangers, rx.state.imbe_erased);
    LS_EQ_INT(strangers, 0);
    LS_CHECK(rx.state.imbe_erased >= 1);
    LS_CHECK(r.played >= CALL_FRAMES - 4);

    n = call_dibits(&c, cut, 150);
    for (size_t i = (size_t)cut; i < (size_t)cut + 150; i++) s_dib[i] = 0;   /* the old filler */
    receive_from(NULL, 0, s_dib, n, 1, 0, &r);
    ls_note("filled with +1: decoded %d, frames not sent %d", r.decoded, sent_frames_only());
}

/* One LDU2 whose ESS decodes to ADP inside a call its HDU proved clear, as
   RS(24,16,9) past its reach does: the algorithm cannot change inside a
   call, so nothing mutes until a second LDU2 agrees. */
LS_CASE(one_ess_saying_encrypted_does_not_mute_a_clear_call)
{
    p25_call_t c = clear_call();
    c.enc_ldu2 = 3;
    p25_call_channel_t ch = quiet_channel();
    result_t r;
    on_air(&c, &ch, 5, &r);
    ls_note("one ADP ESS in a clear call: played %d of %d, doubted %u",
            r.played, CALL_FRAMES, rx.state.p25_ess_doubted);
    LS_EQ_INT(r.played, CALL_FRAMES);
    LS_CHECK(rx.state.p25_ess_doubted >= 1);
}

/* Of the IMBE frames the vocoder got, how many carry the sent c0..c6: the 81
   bits FEC protects (c7's 7 never are, so no decoder can be held to them). */
static double fec_exact_fraction(void)
{
    int exact = 0;
    for (int f = 0; f < s_n_decoded && f < CALL_FRAMES; f++)
        exact += !memcmp(s_decoded[f], s_voice[f], 10) &&
                 !((s_decoded[f][10] ^ s_voice[f][10]) & 0x80u);
    return (double)exact / CALL_FRAMES;
}

/* The LR2021's lookup says which symbols it doubts (p25_os4_decode.h), and
   its errors are mostly an inner level read as outer or back, where it was
   unsure. Here one symbol in eight has its inner/outer bit wrong and doubted,
   and as many right ones are doubted too. The hard FEC brings 41% of frames'
   protected bits back whole; with the doubts Chase-II brings 84% (measured).
   That is the FEC alone: about nine fixes a frame is past what the repeat
   rule lets through, so it is off here. */
static double doubted_errors_exact(int soft)
{
    p25_call_t c = clear_call();
    const size_t n = call_dibits(&c, 0, 0);
    ls_rng_t g;
    ls_rng_seed(&g, 0x5EEDu);
    for (size_t i = 0; i < n; i++) {
        const uint32_t v = ls_rng_u32(&g) % 8u;
        if (v == 0) s_dib[i] = (uint8_t)((s_dib[i] ^ 1u) | (3u << 3));
        else if (v == 1) s_dib[i] |= (uint8_t)(3u << 3);
    }
    result_t r;
    s_fec_only = 1;
    receive_from(NULL, 0, s_dib, n, 1, soft, &r);
    s_fec_only = 0;
    ls_note("doubted inner/outer errors, %s: decoded %d, c0..c6 exact %.0f%% (all 88 bits %.0f%%)",
            soft ? "soft FEC" : "hard FEC", r.decoded, 100.0 * fec_exact_fraction(), 100.0 * exact_fraction());
    return fec_exact_fraction();
}

LS_CASE(doubted_symbol_errors_are_corrected_by_the_soft_fec)
{
    const double hard = doubted_errors_exact(0);
    const double soft = doubted_errors_exact(1);
    LS_CHECK(soft >= 0.80);
    LS_CHECK(soft >= hard + 0.35);
}

/* A weak stretch: most of one LDU's symbols are noise, all of them doubted.
   Chase still finds a codeword for every word, so the frames come out
   "corrected" and garbage unless the many fixes in c1..c6 give them away.
   A clear call next to it must not lose a frame to that rule. */
/* where the k-th frame sync of the call in s_sym begins */
static int frame_sync_at(int k, int n)
{
    static const int sync[24] = { 3, 3, 3, 3, 3, -3, 3, 3, -3, -3, 3, 3,
                                  -3, -3, -3, -3, 3, -3, 3, -3, -3, -3, -3, -3 };
    for (int i = 0; i + 24 <= n; i++) {
        int j = 0;
        while (j < 24 && s_sym[i + j] == sync[j]) j++;
        if (j == 24 && k-- == 0) return i;
    }
    return -1;
}

LS_CASE(frames_that_needed_many_fixes_are_replaced_not_played)
{
    p25_call_t c = clear_call();
    size_t n = call_dibits(&c, 0, 0);
    result_t r;
    receive_from(NULL, 0, s_dib, n, 1, 1, &r);
    ls_note("clear, soft FEC: played %d of %d, gated %u", r.played, CALL_FRAMES, rx.state.imbe_gated);
    LS_EQ_INT(r.played, CALL_FRAMES);
    LS_EQ_INT((int)rx.state.imbe_gated, 0);

    n = call_dibits(&c, 0, 0);
    /* the fifth frame (HDU, LDU1, LDU2, LDU1, LDU2), past its sync and NID */
    const int at = frame_sync_at(4, (int)n);
    LS_CHECK(at > 0);
    ls_rng_t g;
    ls_rng_seed(&g, 0xBADu);
    const size_t from = (size_t)at + 100;
    for (size_t i = from; i < from + 600 && i < n; i++)
        s_dib[i] = (uint8_t)((ls_rng_u32(&g) & 3u) | 0x78u);
    receive_from(NULL, 0, s_dib, n, 1, 1, &r);
    const int strangers = sent_frames_only();
    ls_note("600 noise symbols, soft FEC: decoded %d, frames not sent %d, gated %u, repeated %u, muted %u",
            r.decoded, strangers, rx.state.imbe_gated, rx.state.imbe_repeated, rx.state.imbe_muted);
    LS_CHECK(rx.state.imbe_gated >= 4);
    LS_CHECK(strangers <= 2);
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
       are the measured exact-frame share over 5 calls, less a margin: 97 /
       91 / 84 / 62% now, 69 / 56 / 45 / 30% before the symbol clock was
       fixed. Raising them is how an improvement is kept. */
    const struct { float sd; double exact; int played_pct; } levels[] = {
        { 0.3f, 0.92, 100 }, { 0.5f, 0.85, 100 }, { 0.7f, 0.76, 100 }, { 1.0f, 0.52, 95 },
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
