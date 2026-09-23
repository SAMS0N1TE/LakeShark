/* p25_voice_replay - decode a P25 IQ capture through the production voice
 * path and account for every voice frame on the air.
 *
 *     p25_voice_replay <u8-iq.bin> <demod-gain> [out.wav] [--frames]
 *
 * The input is what 'p25 capture' on the board records: unsigned 8-bit IQ at
 * 240000 Hz, demod gain from 'p25 capture status' (demod_gain_bits is the
 * float's bit pattern; c60ca000 is -9000).
 *
 * p25_iq_replay counts frames through stubs. This runs the real HDU, LDU1,
 * LDU2 and TDU processors, the ESS mute policy and the OP25 IMBE decoder, so
 * the question it answers is the operator's: of the voice that was on the
 * air, how much would the speaker have played? Voice that is lost is
 * attributed - never synced, synced but muted for unknown ESS, muted as
 * encrypted.
 *
 * The WAV, if asked for, is laid out on the air's own timeline: each voice
 * frame at the moment it was transmitted, silence where nothing played. That
 * is what choppy sounds like, and it is the same file before and after a
 * decoder change.
 */
#include "dsd.h"
#include "dsp_pipeline.h"
#include "imbe_shim.h"
#include "p25_voice_hold.h"
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- what the firmware's other layers would supply ---------------------- */
int autoscan_bch_ok_flag;
int dsd_bch_fail_counter;
int ls_shim_log_enabled;
void sys_log(unsigned char color, const char *fmt, ...) { (void)color; (void)fmt; }
void audio_beep_request(int kind) { (void)kind; }

static struct {
    dsp_state_t dsp;
    dsd_sample_ring_t ring;
    dsd_opts opts;
    dsd_state state;
    int dibits[10000];
    short audio[2000];
    float audio_float[2000];
    short pcm[2000];
    p25_voice_hold_t hold;
    int16_t out[P25_HOLD_MAX_SAMPLES + 2000];
    mbe_parms cur, prev, enhanced;
    const uint8_t *iq;
    size_t iq_bytes, offset, chunk;
} r;

void dsd_yield(void)
{
    if (r.offset >= r.iq_bytes) { exitflag = 1; dsd_abort = 1; return; }
    int16_t out[4096];
    size_t n = r.iq_bytes - r.offset;
    if (n > r.chunk) n = r.chunk;
    int got = dsp_process_iq(&r.dsp, r.iq + r.offset, (int)n, out, 4096);
    r.offset += n;
    for (int i = 0; i < got; i++) {
        int next = (r.ring.write_idx + 1) % DSD_SAMPLE_RING_SIZE;
        if (next == r.ring.read_idx) break;   /* cannot happen at this chunk */
        r.ring.buf[r.ring.write_idx] = out[i];
        r.ring.write_idx = next;
    }
}

/* Air time of the symbol being read now: IQ consumed, less what is still
   queued in the demodulated ring (10 samples a symbol at 48 kHz). */
static double air_seconds(void)
{
    int queued = (r.ring.write_idx - r.ring.read_idx + DSD_SAMPLE_RING_SIZE)
                 % DSD_SAMPLE_RING_SIZE;
    return (double)r.offset / 2.0 / 240000.0 - queued / 48000.0;
}

/* ---- the account --------------------------------------------------------- */
#define MAX_EVENTS 4096
typedef struct { double t; char duid[3]; int imbe_played; int imbe_muted; } voice_event_t;
static voice_event_t ev[MAX_EVENTS];
static int n_ev;

/* Held voice keeps the air time it was decoded at, so a release writes it
   back where it was spoken; the board plays it late but whole. */
static struct { double t; int n; int16_t pcm[1440]; } pend[8];
static int n_pend;

static void timeline_put(int16_t *tl, size_t tl_n, double t, const int16_t *pcm, int n)
{
    long at = (long)(t * 8000.0);
    for (int i = 0; i < n && at + i >= 0 && (size_t)(at + i) < tl_n; i++) tl[at + i] = pcm[i];
}

/* --symbols: every symbol the sync hunt sees, with the slicer thresholds it
   was cut against, for looking at why a frame on the air was not found. */
static FILE *s_symf;
static void symbol_observer(int symbol, int center, int umid, int lmid)
{
    if (s_symf) fprintf(s_symf, "%.5f %d %d %d %d\n", air_seconds(), symbol, center, umid, lmid);
}

static void wav_write(const char *path, const int16_t *pcm, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    uint32_t data = (uint32_t)(n * 2), rate = 8000, byte_rate = 16000;
    uint16_t fmt = 1, ch = 1, align = 2, bits = 16;
    uint32_t riff = 36 + data, fmt_len = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_len, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(pcm, 2, n, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: p25_voice_replay <u8-iq.bin> <demod-gain> [out.wav] [--frames] [--symbols out.txt]\n");
        return 2;
    }
    const char *wav = NULL;
    int list = 0;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--frames")) list = 1;
        else if (!strcmp(argv[i], "--symbols") && i + 1 < argc) {
            s_symf = fopen(argv[++i], "w");
            dsd_set_symbol_observer(symbol_observer);
        }
        else wav = argv[i];
    }
    char *end; errno = 0;
    float gain = strtof(argv[2], &end);
    if (errno || *end || !isfinite(gain) || gain == 0) { fputs("bad demod gain\n", stderr); return 2; }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long bytes = ftell(f); fseek(f, 0, SEEK_SET);
    if (bytes <= 0 || (bytes & 1)) { fputs("capture must hold whole IQ pairs\n", stderr); return 2; }
    uint8_t *iq = malloc((size_t)bytes);
    if (!iq || fread(iq, 1, (size_t)bytes, f) != (size_t)bytes) { fputs("read failed\n", stderr); return 2; }
    fclose(f);

    memset(&r, 0, sizeof(r));
    r.state.dibit_buf = r.dibits;
    r.state.audio_out_buf = r.audio;
    r.state.audio_out_float_buf = r.audio_float;
    r.state.cur_mp = &r.cur; r.state.prev_mp = &r.prev; r.state.prev_mp_enhanced = &r.enhanced;
    initState(&r.state);
    initOpts(&r.opts);
    r.opts.mod_qpsk = 0; r.opts.mod_gfsk = 0;
    r.opts.ring = &r.ring;
    r.opts.verbose = 0;
    r.state.pcm_out_buf = r.pcm;
    r.state.pcm_out_size = 2000;
    dsp_init(&r.dsp);
    dsp_set_mode(&r.dsp, DEMOD_C4FM);
    dsp_set_gain(&r.dsp, gain);
    r.iq = iq; r.iq_bytes = (size_t)bytes; r.chunk = 2048;
    imbe_shim_init();
    p25_voice_hold_reset(&r.hold);

    const double dur = (double)bytes / 2.0 / 240000.0;
    const size_t timeline_n = (size_t)(dur * 8000.0) + 8000;
    int16_t *timeline = calloc(timeline_n, sizeof(int16_t));

    unsigned hdu = 0, ldu1 = 0, ldu2 = 0, tdu = 0, other = 0, ldu2_orphan = 0;
    unsigned imbe_played = 0;
    while (!exitflag) {
        int sync = getFrameSync(&r.opts, &r.state);
        double t = air_seconds();
        if (sync < 0 || exitflag) {
            p25_voice_hold_tick(&r.hold, (uint32_t)(t * 1000.0));
            if (!r.hold.n) n_pend = 0;
            continue;
        }
        unsigned muted0 = r.state.p25_enc_muted_frames;
        int hdr0 = r.state.debug_header_errors, crit0 = r.state.debug_header_critical_errors;
        int lastp25type0 = r.state.lastp25type;
        r.state.pcm_out_write = 0;
        r.state.pcm_out_unproven = 0;
        processFrame(&r.opts, &r.state);
        if (!r.state.p25_frame_valid) {
            if (list)
                printf("%8.3f  --  sync found, frame rejected (nac=%03X synctype=%d)\n",
                       t, r.state.nac, sync);
            p25_voice_hold_tick(&r.hold, (uint32_t)(t * 1000.0));
            if (!r.hold.n) n_pend = 0;
            continue;
        }
        const char *d = "??";
        switch (r.state.p25_frame_duid) {
        case 0:  d = "00"; hdu++; break;
        case 5:  d = "11"; ldu1++; break;
        case 10: d = "22";
            if (lastp25type0 != 1) ldu2_orphan++;   /* no LDU1 before it */
            ldu2++; break;
        case 3: case 15: d = r.state.p25_frame_duid == 3 ? "03" : "33"; tdu++; break;
        default: other++; break;
        }
        /* Exactly what app_p25.c does with the frame. */
        uint8_t duid = r.state.p25_frame_duid;
        if (duid == 0 || duid == 3 || duid == 15) { p25_voice_hold_end_call(&r.hold); n_pend = 0; }
        int decoded = r.state.pcm_out_write;
        uint32_t released0 = r.hold.released_frames;
        int out_n = p25_voice_hold_frame(&r.hold, r.pcm, decoded, r.state.pcm_out_unproven,
            r.state.p25_ess_valid, r.state.p25_algid, (uint32_t)r.state.lasttg,
            (uint32_t)(t * 1000.0), r.out, (int)(sizeof(r.out) / sizeof(r.out[0])));
        int held_now = out_n == 0 && decoded > 0 && r.hold.n > 0 && r.state.pcm_out_unproven;
        if (r.hold.released_frames != released0 && timeline) {
            for (int i = 0; i < n_pend; i++)
                timeline_put(timeline, timeline_n, pend[i].t, pend[i].pcm, pend[i].n);
            n_pend = 0;
        }
        if (!r.hold.n && !held_now) n_pend = 0;
        if (held_now) {
            if (n_pend == 8) { memmove(pend, pend + 1, sizeof(pend[0]) * 7); n_pend = 7; }
            pend[n_pend].t = t;
            pend[n_pend].n = decoded > 1440 ? 1440 : decoded;
            memcpy(pend[n_pend].pcm, r.pcm, (size_t)pend[n_pend].n * 2);
            n_pend++;
            /* keep only what the hold itself kept: its newest samples */
            int kept = 0;
            for (int i = n_pend - 1; i >= 0; i--) {
                kept += pend[i].n;
                if (kept > P25_HOLD_MAX_SAMPLES) {
                    memmove(pend, pend + i + 1, sizeof(pend[0]) * (size_t)(n_pend - i - 1));
                    n_pend -= i + 1;
                    break;
                }
            }
        }
        int played = out_n / 160;
        int muted = (int)(r.state.p25_enc_muted_frames - muted0);
        imbe_played += (unsigned)played;
        if (n_ev < MAX_EVENTS) {
            voice_event_t *e = &ev[n_ev++];
            e->t = t; memcpy(e->duid, d, 3);
            e->imbe_played = played; e->imbe_muted = muted;
        }
        if (timeline && played && decoded)
            /* t is read just after the frame sync, which leads the frame,
               so the frame's voice starts there. Released held voice was
               written above at its own times. */
            timeline_put(timeline, timeline_n, t, r.pcm, decoded);
        if (list)
            printf("%8.3f  %s  decoded=%d played=%d muted=%d held=%d ess=%s algid=%02X "
                   "hdr_fixed=%d hdr_critical=%d\n",
                   t, d, decoded / 160, played, muted, r.hold.n / 160,
                   r.state.p25_ess_valid ? "valid" : "unknown", r.state.p25_algid,
                   r.state.debug_header_errors - hdr0,
                   r.state.debug_header_critical_errors - crit0);
    }

    /* Voice slots on the air: LDUs arrive every 180 ms inside a call. A gap
       between two voice frames of n slots means n-1 were never synced. */
    unsigned missed = 0, voice_frames = 0;
    double last = -1;
    for (int i = 0; i < n_ev; i++) {
        if (strcmp(ev[i].duid, "11") && strcmp(ev[i].duid, "22")) { last = -1; continue; }
        voice_frames++;
        if (last >= 0) {
            double gap = ev[i].t - last;
            if (gap < 1.5) {
                int slots = (int)floor(gap / 0.18 + 0.5);
                if (slots > 1) missed += (unsigned)(slots - 1);
            }
        }
        last = ev[i].t;
    }
    unsigned on_air = (voice_frames + missed) * 9;
    printf("P25VOICE seconds=%.2f hdu=%u ldu1=%u ldu2=%u tdu=%u other=%u "
           "ldu_missed=%u ldu2_orphan=%u imbe_on_air=%u imbe_played=%u "
           "muted=%u muted_unknown_ess=%u ess_rs_failed=%u ess_rs_kept=%u "
           "held=%u released=%u discarded=%u played_pct=%.1f\n",
           dur, hdu, ldu1, ldu2, tdu, other, missed, ldu2_orphan, on_air, imbe_played,
           r.state.p25_enc_muted_frames, r.state.p25_enc_muted_unknown,
           r.state.p25_ess_rs_failed, r.state.p25_ess_rs_kept,
           r.hold.held_frames, r.hold.released_frames, r.hold.discarded_frames,
           on_air ? 100.0 * imbe_played / on_air : 0.0);
    if (wav && timeline) wav_write(wav, timeline, (size_t)(dur * 8000.0));
    if (s_symf) fclose(s_symf);
    free(timeline); free(iq);
    return 0;
}
