/* reuse the tested production DSP/ring/sync/NID/TSBK harness.
 * This is a diagnostic executable, deliberately not a passing test when
 * no capture was supplied. Voice dispatch is counted, not PCM-validated. */
#include "../tests/test_p25_acquisition.c"
#include <errno.h>

int ls_shim_log_enabled;
void ls_register(const char *file, const char *name, ls_case_fn fn) {}
void ls_register_xfail(const char *file, const char *name, ls_case_fn fn, const char *ticket) {}
void ls_fail(const char *file, int line, const char *fmt, ...)
{
    fprintf(stderr, "replay assertion at %s:%d\n", file, line);
    exit(2);
}
void ls_note(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n');
}

int main(int argc, char **argv)
{
    if (argc != 4 || (strcmp(argv[2], "c4fm") && strcmp(argv[2], "cqpsk"))) {
        fprintf(stderr, "usage: p25_iq_replay <u8-iq.bin> <c4fm|cqpsk> <demod-gain>\n"
                        "Requires 240000 Hz IQ; read frequency/rate/gain from capture status.\n");
        return 2;
    }
    char *end; errno = 0;
    float gain = strtof(argv[3], &end);
    if (errno || !*argv[3] || *end || !isfinite(gain) || gain == 0 || fabsf(gain) > 100000) {
        fputs("invalid demod gain\n", stderr); return 2;
    }
    bool fixture = !strcmp(argv[1], "fixture:c4fm");
    FILE *file = fixture ? NULL : fopen(argv[1], "rb");
    long bytes = IQ_BYTES;
    uint8_t *iq = NULL;
    if (fixture) iq = protocol_iq();
    else {
    if (!file) { perror("capture"); return 2; }
    if (fseek(file, 0, SEEK_END)) { fclose(file); return 2; }
    bytes = ftell(file);
    if (bytes <= 0 || bytes > 4*1024*1024 || (bytes & 1) || fseek(file, 0, SEEK_SET)) {
        fputs("capture must contain 1..4 MiB of complete IQ pairs\n", stderr);
        fclose(file); return 2;
    }
    iq = malloc((size_t)bytes);
    if (!iq) { fclose(file); return 2; }
    if (fread(iq, 1, (size_t)bytes, file) != (size_t)bytes) {
        fclose(file); free(iq); return 2;
    }
    fclose(file);
    }
    replay_init(iq, (size_t)bytes, 16384);
    external_replay = true;
    dsp_set_mode(&replay.dsp, !strcmp(argv[2], "c4fm") ? DEMOD_C4FM : DEMOD_CQPSK);
    dsp_set_gain(&replay.dsp, gain);
    LS_CHECK(prepare(false, false));
    unsigned frames = 0;
    while (!exitflag) { if (replay_frame()) frames++; }
    p25_hunt_status_t *h = &replay.state.acquisition_hunt;
    printf("P25REPLAY bytes=%ld mode=%s demod_gain=%g raw_sync=%u inverted=%u "
           "hunts=%u timeouts=%u symbols=%u best_hd=%u/%u range=%ld..%ld "
           "valid_frames=%u nid_ok=%d nid_fail=%d tsbk=%u voice_dispatch=%u pcm=not-tested\n",
        bytes, argv[2], (double)gain, h->raw_syncs, h->inverted_matches,
        h->attempts, h->timeouts, h->symbols, h->best_normal_hd, h->best_inverted_hd,
        (long)h->symbol_min, (long)h->symbol_max, frames, autoscan_bch_ok_flag,
        dsd_bch_fail_counter, replay.state.p25_tsbk_valid_count, external_voice_dispatches);
    if (!fixture) free(iq);
    if (fixture && !strcmp(argv[2], "c4fm") && gain == -9000.0f)
        return frames == N_FRAMES && autoscan_bch_ok_flag == N_FRAMES &&
               dsd_bch_fail_counter == 0 ? 0 : 1;
    return 0;
}
