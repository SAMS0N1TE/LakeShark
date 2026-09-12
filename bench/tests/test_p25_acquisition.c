/* reuse the seeded IQ generator and its existing baselines. This
 * target replaces the baseline's carrier-reset stub with production DSD. */
#define LS_P25_REAL_ACQUISITION 1
#include "test_p25_cqpsk_baseline.c"
#include "p25_acquisition.h"
#include "p25_demod_control.h"
#include "esp_heap_caps.h"

int autoscan_bch_ok_flag;
int dsd_bch_fail_counter;
static bool external_replay;
static unsigned external_voice_dispatches;
static void voice_dispatch(void)
{
    if (external_replay) external_voice_dispatches++;
    else LS_CHECK(false);
}

/* These control-channel fixtures must never enter a voice decoder. */
void processHDU(dsd_opts *o, dsd_state *s) { voice_dispatch(); }
void processLDU1(dsd_opts *o, dsd_state *s) { voice_dispatch(); }
void processLDU2(dsd_opts *o, dsd_state *s) { voice_dispatch(); }
void processTDU(dsd_opts *o, dsd_state *s) { voice_dispatch(); }
void processTDULC(dsd_opts *o, dsd_state *s) { voice_dispatch(); }
int mbe_eccImbe7200x4400C0(char f[8][23]) { LS_CHECK(false); return 0; }
int mbe_eccImbe7200x4400Data(char f[8][23], char *d) { LS_CHECK(false); return 0; }
void mbe_demodulateImbe7200x4400Data(char f[8][23]) { LS_CHECK(false); }
void imbe_shim_decode_88(const uint8_t *in, int16_t *out) { LS_CHECK(false); }
bool imbe_shim_try_decode_88(const uint8_t *in, int16_t *out)
{ (void)in; (void)out; LS_CHECK(false); return false; }
void audio_beep_request(int kind) {}

static struct {
    dsp_state_t dsp;
    dsd_sample_ring_t ring;
    dsd_opts opts;
    dsd_state state;
    p25_acquisition_gate_t gate;
    int dibits[10000];
    short audio[2000];
    float audio_float[2000];
    mbe_parms cur, prev, enhanced;
    const uint8_t *iq;
    size_t iq_bytes;
    size_t offset;
    size_t chunk;
    uint32_t generation;
    bool change_on_refill;
    bool mode_change_on_refill;
} replay;

void dsd_yield(void)
{
    if (!replay.iq || replay.offset >= replay.iq_bytes) {
        exitflag = 1;
        dsd_abort = 1;
        return;
    }
    if (replay.change_on_refill) {
        replay.generation++;
        replay.change_on_refill = false;
    }
    if (replay.mode_change_on_refill) {
        if (dsp_select_mode(&replay.dsp, DEMOD_CQPSK, 9000.0f)) {
            replay.ring.read_idx = replay.ring.write_idx;
            replay.generation++;
        }
        replay.mode_change_on_refill = false;
    }
    int16_t audio[2048];
    size_t bytes = replay.iq_bytes - replay.offset;
    if (bytes > replay.chunk) bytes = replay.chunk;
    int n = dsp_process_iq(&replay.dsp, replay.iq + replay.offset,
                            (int)bytes, audio, 2048);
    replay.offset += bytes;
    for (int i = 0; i < n; i++) {
        int next = (replay.ring.write_idx + 1) % DSD_SAMPLE_RING_SIZE;
        LS_CHECK(next != replay.ring.read_idx);
        replay.ring.buf[replay.ring.write_idx] = audio[i];
        replay.ring.write_idx = next;
    }
}

static void replay_init(const uint8_t *iq, size_t bytes, size_t chunk)
{
    memset(&replay, 0, sizeof(replay));
    replay.state.dibit_buf = replay.dibits;
    replay.state.audio_out_buf = replay.audio;
    replay.state.audio_out_float_buf = replay.audio_float;
    replay.state.cur_mp = &replay.cur;
    replay.state.prev_mp = &replay.prev;
    replay.state.prev_mp_enhanced = &replay.enhanced;
    initState(&replay.state);
    initOpts(&replay.opts);
    replay.opts.mod_qpsk = 0;
    replay.opts.mod_gfsk = 0;
    replay.opts.ring = &replay.ring;
    replay.opts.verbose = 0;
    dsp_init(&replay.dsp);
    dsp_set_mode(&replay.dsp, DEMOD_C4FM);
    dsp_set_gain(&replay.dsp, -9000.0f);
    replay.iq = iq;
    replay.iq_bytes = bytes;
    replay.chunk = chunk;
    exitflag = 0;
    dsd_abort = 0;
    autoscan_bch_ok_flag = 0;
    dsd_bch_fail_counter = 0;
}

static bool prepare(bool pending, bool failed)
{
    return p25_acquisition_prepare(&replay.gate, &replay.opts,
        &replay.state, replay.generation, pending, failed);
}

static bool replay_frame(void)
{
    int sync = getFrameSync(&replay.opts, &replay.state);
    if (!p25_acquisition_accept_sync(&replay.gate, replay.generation, sync))
        return false;
    if (sync < 0) return false;
    processFrame(&replay.opts, &replay.state);
    return p25_acquisition_accept_frame(&replay.gate, replay.generation)
           && replay.state.p25_frame_valid;
}

static uint8_t *protocol_iq_variant(float offset, float noise)
{
    static int symbols[N_SYMBOLS];
    static int rendered[RENDER_SYMBOLS];
    static uint8_t iq[IQ_BYTES];
    ls_rng_t rng;
    ls_rng_seed(&rng, 0x7315u);
    make_protocol_symbols(symbols, &rng);
    memcpy(rendered, symbols, sizeof(symbols));
    for (int i = N_SYMBOLS; i < RENDER_SYMBOLS; i++) rendered[i] = 1;
    render_c4fm_iq(rendered, offset, noise, &rng, iq);
    return iq;
}

static uint8_t *protocol_iq(void)
{
    return protocol_iq_variant(0, 0);
}

static uint8_t *protocol_cqpsk_iq(void)
{
    static int symbols[N_SYMBOLS];
    static int rendered[RENDER_SYMBOLS];
    static uint8_t iq[IQ_BYTES];
    ls_rng_t rng;
    ls_rng_seed(&rng, 0x660u);
    make_protocol_symbols(symbols, &rng);
    memcpy(rendered, symbols, sizeof(symbols));
    for (int i = N_SYMBOLS; i < RENDER_SYMBOLS; ++i) rendered[i] = 1;
    lsm_channel_t channel;
    lsm_channel_clean(&channel);
    LS_EQ_INT(lsm_render_iq(rendered, RENDER_SYMBOLS, &channel, &rng,
                           iq, sizeof(iq)), IQ_BYTES);
    return iq;
}

LS_CASE(early_auto_lock_preserves_c4fm_dsp_and_queued_protocol_samples)
{
    p25_demod_control_t control;
    p25_demod_control_init(&control, P25_DEMOD_AUTO, 0, 0, 0);
    replay_init(protocol_iq(), IQ_BYTES, 16384);
    LS_CHECK(prepare(false, false));
    LS_CHECK(replay_frame());
    LS_EQ_UINT(autoscan_bch_ok_flag, 1);
    LS_EQ_UINT(replay.state.p25_tsbk_valid_count, 1);
    dsp_state_t before = replay.dsp;
    int queued = dsd_ring_available(&replay.ring);
    LS_CHECK(queued > 0);
    LS_CHECK(p25_demod_control_feed(&control, 8192, 240000,
        autoscan_bch_ok_flag, replay.state.p25_tsbk_valid_count, false));
    LS_CHECK(control.locked);
    LS_EQ_INT(control.active, DEMOD_C4FM);
    LS_CHECK(!dsp_select_mode(&replay.dsp, control.active, -9000.0f));
    LS_CHECK(memcmp(&before, &replay.dsp, sizeof(before)) == 0);
    LS_EQ_INT(dsd_ring_available(&replay.ring), queued);
    while (!exitflag) (void)replay_frame();
    LS_EQ_INT(autoscan_bch_ok_flag, 20);
    LS_EQ_UINT(replay.state.p25_tsbk_valid_count, 20);
    replay.iq = NULL;
    exitflag = dsd_abort = 0;
}

LS_CASE(auto_lock_preserves_cqpsk_dsp_and_queued_protocol_samples)
{
    p25_demod_control_t control;
    p25_demod_control_init(&control, P25_DEMOD_AUTO, 0, 0, 0);
    LS_CHECK(p25_demod_control_feed(&control, 360000, 240000, 0, 0, false));
    replay_init(protocol_cqpsk_iq(), IQ_BYTES, 16384);
    LS_CHECK(dsp_select_mode(&replay.dsp, control.active, 9000.0f));
    LS_CHECK(prepare(false, false));
    LS_CHECK(replay_frame());
    dsp_state_t before = replay.dsp;
    int queued = dsd_ring_available(&replay.ring);
    LS_CHECK(queued > 0);
    LS_CHECK(p25_demod_control_feed(&control, 360000, 240000,
        autoscan_bch_ok_flag, replay.state.p25_tsbk_valid_count, false));
    LS_CHECK(control.locked);
    LS_EQ_INT(control.active, DEMOD_CQPSK);
    LS_CHECK(!dsp_select_mode(&replay.dsp, control.active, 9000.0f));
    LS_CHECK(memcmp(&before, &replay.dsp, sizeof(before)) == 0);
    LS_EQ_INT(dsd_ring_available(&replay.ring), queued);
    /* Old unconditional apply demonstrably destroyed the acquired state. */
    dsp_state_t old = before;
    dsp_set_mode(&old, DEMOD_CQPSK);
    LS_CHECK(memcmp(&before, &old, sizeof(before)) != 0);
    while (!exitflag) (void)replay_frame();
    LS_EQ_INT(autoscan_bch_ok_flag, 19);
    LS_EQ_UINT(replay.state.p25_tsbk_valid_count, 19);
    replay.iq = NULL;
    exitflag = dsd_abort = 0;
}

LS_CASE(mode_change_crossing_a_live_hunt_discards_old_epoch_then_reacquires)
{
    replay_init(protocol_cqpsk_iq(), IQ_BYTES, 16384);
    LS_CHECK(prepare(false, false));
    replay.mode_change_on_refill = true;
    LS_CHECK(!replay_frame());
    LS_EQ_INT(replay.dsp.mode, DEMOD_CQPSK);
    LS_EQ_UINT(replay.gate.discarded_syncs, 1);
    LS_EQ_UINT(replay.gate.accepted_syncs, 0);
    LS_EQ_INT(autoscan_bch_ok_flag, 0);
    LS_CHECK(prepare(false, false));
    LS_EQ_UINT(replay.gate.resets, 1);
    LS_CHECK(replay_frame());
    LS_EQ_INT(replay.state.nac, 0x293);
    LS_EQ_UINT(replay.state.p25_tsbk_valid_count, 1);
    replay.iq = NULL;
    exitflag = dsd_abort = 0;
}

LS_CASE(production_cqpsk_sign_is_not_normalized_away_by_fixture_slicer)
{
    uint8_t *iq = protocol_cqpsk_iq();
    for (int sign = -1; sign <= 1; sign += 2) {
        replay_init(iq, IQ_BYTES, 16384);
        dsp_set_mode(&replay.dsp, DEMOD_CQPSK);
        dsp_set_gain(&replay.dsp, sign * 9000.0f);
        LS_CHECK(prepare(false, false));
        unsigned frames = 0;
        while (!exitflag) if (replay_frame()) frames++;
        ls_note("production CQPSK gain=%d raw=%u inverted=%u NID=%d fail=%d TSBK=%u",
            sign * 9000, replay.state.acquisition_hunt.raw_syncs,
            replay.state.acquisition_hunt.inverted_matches, autoscan_bch_ok_flag,
            dsd_bch_fail_counter, replay.state.p25_tsbk_valid_count);
        if (sign > 0) {
            LS_CHECK(frames >= EXPECTED_COMPLETE_FRAMES);
            LS_CHECK(autoscan_bch_ok_flag >= EXPECTED_COMPLETE_FRAMES);
            LS_CHECK(replay.state.p25_tsbk_valid_count >= EXPECTED_COMPLETE_FRAMES);
        } else {
            LS_EQ_UINT(frames, 0);
            LS_EQ_UINT(replay.state.acquisition_hunt.raw_syncs, 0);
            LS_CHECK(replay.state.acquisition_hunt.inverted_matches > 0);
        }
    }
    replay.iq = NULL;
    exitflag = dsd_abort = 0;
}

LS_CASE(production_auto_switch_and_inversion_use_mode_specific_polarity)
{
    p25_demod_control_t control;
    p25_demod_control_init(&control, P25_DEMOD_AUTO, 0, 0, 0);
    LS_NEAR(p25_demod_output_gain(control.active, false), -9000, 0);
    LS_CHECK(p25_demod_control_tick(&control, 1500, 0, 0, false));
    LS_EQ_INT(control.active, DEMOD_CQPSK);
    uint8_t *iq = protocol_cqpsk_iq();
    for (int inverted = 0; inverted <= 1; ++inverted) {
        replay_init(iq, IQ_BYTES, 16384);
        /* Match AUTO: initialize the C4FM path, then apply its selected mode
         * and the same gain contract used by the real RX owner. */
        dsp_set_gain(&replay.dsp, p25_demod_output_gain(DEMOD_C4FM, inverted));
        dsp_set_mode(&replay.dsp, control.active);
        dsp_set_gain(&replay.dsp, p25_demod_output_gain(control.active, inverted));
        LS_CHECK(prepare(false, false));
        unsigned frames = 0;
        while (!exitflag) if (replay_frame()) frames++;
        ls_note("AUTO selected CQPSK inverted=%d gain=%g sync=%u NID=%d TSBK=%u",
            inverted, (double)replay.dsp.demod_gain, replay.state.acquisition_hunt.raw_syncs,
            autoscan_bch_ok_flag, replay.state.p25_tsbk_valid_count);
        if (!inverted) {
            LS_CHECK(frames >= EXPECTED_COMPLETE_FRAMES);
            LS_CHECK(autoscan_bch_ok_flag >= EXPECTED_COMPLETE_FRAMES);
            LS_CHECK(replay.state.p25_tsbk_valid_count >= EXPECTED_COMPLETE_FRAMES);
            LS_CHECK(p25_demod_control_tick(&control, 3000,
                autoscan_bch_ok_flag, replay.state.p25_tsbk_valid_count, false));
            LS_CHECK(control.locked);
            LS_EQ_INT(control.active, DEMOD_CQPSK);
        } else {
            LS_EQ_UINT(frames, 0);
            LS_CHECK(replay.state.acquisition_hunt.inverted_matches > 0);
        }
    }
    for (int inverted = 0; inverted <= 1; ++inverted) {
        replay_init(protocol_iq(), IQ_BYTES, 16384);
        dsp_set_gain(&replay.dsp, p25_demod_output_gain(DEMOD_C4FM, inverted));
        LS_CHECK(prepare(false, false));
        unsigned frames = 0;
        while (!exitflag) if (replay_frame()) frames++;
        LS_EQ_UINT(frames, inverted ? 0 : N_FRAMES);
        LS_EQ_INT(autoscan_bch_ok_flag, inverted ? 0 : N_FRAMES);
    }
    LS_NEAR(p25_demod_output_gain(DEMOD_CQPSK, true),
            -p25_demod_output_gain(DEMOD_CQPSK, false), 0);
    replay.iq = NULL;
    exitflag = dsd_abort = 0;
}

/* Separate from the sign contract: byte16384 is inside the first of20
 * generated frames (38500 bytes/frame), leaving19 full later frames plus
 * the generator's filter tail. Those19 NIDs decode, but5 TSBKs do not.
 * Keep that startup defect visible without inventing a DSP reset policy. */
LS_CASE_KNOWN_FAIL(warm_c4fm_to_cqpsk_loses_tsdu_crc_despite_valid_nids,
                   "warm-start phase/history")
{
    uint8_t *iq = protocol_cqpsk_iq();
    replay_init(iq, IQ_BYTES, 16384);
    int16_t warmup[8192];
    (void)dsp_process_iq(&replay.dsp, iq, 16384, warmup, 8192);
    replay.offset = 16384;
    dsp_set_mode(&replay.dsp, DEMOD_CQPSK);
    dsp_set_gain(&replay.dsp, p25_demod_output_gain(DEMOD_CQPSK, false));
    LS_CHECK(prepare(false, false));
    unsigned frames = 0;
    while (!exitflag) if (replay_frame()) frames++;
    ls_note("warm switch at byte16384: complete frames available=19 sync=%u NID=%d TSBK=%u",
        replay.state.acquisition_hunt.raw_syncs, autoscan_bch_ok_flag,
        replay.state.p25_tsbk_valid_count);
    LS_EQ_UINT(frames, 19);
    LS_EQ_INT(autoscan_bch_ok_flag, 19);
    LS_EQ_UINT(replay.state.p25_tsbk_valid_count, 19);
    replay.iq = NULL;
    exitflag = dsd_abort = 0;
}

LS_CASE(production_stream_reaches_nid_and_tsdu_after_delayed_repeated_tunes)
{
    replay_init(protocol_iq(), IQ_BYTES, 16384);
    replay.generation = 1;
    for (int i = 0; i < 20; i++) LS_CHECK(!prepare(true, false));
    LS_EQ_UINT(replay.gate.resets, 1);
    LS_EQ_UINT(replay.gate.pending_waits, 20);
    LS_EQ_UINT(replay.state.acquisition_hunt.attempts, 0);
    LS_EQ_UINT(replay.offset, 0);
    replay.generation = 2;
    LS_CHECK(!prepare(true, false));
    LS_EQ_UINT(replay.gate.resets, 2);
    LS_CHECK(prepare(false, false));
    LS_CHECK(replay_frame());
    LS_EQ_INT(replay.state.nac, 0x293);
    LS_CHECK(replay.state.p25_tsbk_valid_count > 0);
    LS_EQ_UINT(replay.gate.accepted_syncs, 1);
    LS_EQ_UINT(replay.state.acquisition_hunt.raw_syncs, 1);
    LS_EQ_UINT(replay.state.acquisition_hunt.best_normal_hd, 0);
    LS_CHECK(replay.state.acquisition_hunt.symbols > 0);
    LS_CHECK(replay.state.acquisition_hunt.symbol_max >
             replay.state.acquisition_hunt.symbol_min);
}

LS_CASE(failed_tune_is_counted_and_later_success_reacquires)
{
    replay_init(protocol_iq(), IQ_BYTES, 4096);
    replay.generation = 1;
    LS_CHECK(!prepare(true, false));
    for (int i = 0; i < 20; i++) LS_CHECK(!prepare(false, true));
    LS_EQ_UINT(replay.gate.failed_waits, 20);
    LS_EQ_UINT(replay.state.acquisition_hunt.attempts, 0);
    replay.generation = 2;
    LS_CHECK(prepare(false, false));
    LS_CHECK(replay_frame());
    LS_EQ_INT(replay.state.nac, 0x293);
    LS_CHECK(replay.state.p25_tsbk_valid_count > 0);
}

LS_CASE(tune_crossing_sync_discards_then_real_reset_reacquires)
{
    replay_init(protocol_iq(), IQ_BYTES, 16384);
    LS_CHECK(prepare(false, false));
    replay.change_on_refill = true;
    LS_CHECK(!replay_frame());
    LS_EQ_UINT(replay.state.acquisition_hunt.raw_syncs, 1);
    LS_EQ_UINT(replay.gate.accepted_syncs, 0);
    LS_EQ_UINT(replay.gate.discarded_syncs, 1);
    LS_EQ_UINT(replay.state.p25_tsbk_valid_count, 0);
    LS_CHECK(prepare(false, false));
    LS_EQ_UINT(replay.gate.resets, 1);
    LS_CHECK(replay_frame());
    LS_EQ_INT(replay.state.nac, 0x293);
    LS_CHECK(replay.state.p25_tsbk_valid_count > 0);
}

LS_CASE(iq_probe_is_bounded_integer_only_and_counts_clipped_components)
{
    p25_iq_probe_t probe;
    const uint8_t iq[] = {128, 128, 0, 255, 129};
    p25_acquisition_probe_iq(&probe, iq, sizeof(iq));
    LS_EQ_UINT(probe.sampled_pairs, 2);
    LS_EQ_UINT(probe.clipped_components, 2);
    LS_EQ_INT(probe.mean_i_milli, -64000);
    LS_EQ_INT(probe.mean_q_milli, 63500);
    LS_EQ_UINT(probe.mean_square, (128 * 128 + 127 * 127) / 4);
    p25_acquisition_probe_iq(&probe, NULL, 100);
    LS_EQ_UINT(probe.sampled_pairs, 0);
    static uint8_t big[20000];
    memset(big, 255, sizeof(big));
    p25_acquisition_probe_iq(&probe, big, sizeof(big));
    LS_EQ_UINT(probe.sampled_pairs, 8192);
    LS_EQ_UINT(probe.clipped_components, 16384);
    LS_EQ_INT(probe.mean_i_milli, 127000);
    LS_EQ_INT(probe.mean_q_milli, 127000);
}

LS_CASE(tune_crossing_nid_discards_the_frame_before_dispatch_to_followers)
{
    replay_init(protocol_iq(), IQ_BYTES, 1024);
    LS_CHECK(prepare(false, false));
    int sync = getFrameSync(&replay.opts, &replay.state);
    LS_CHECK(sync >= 0);
    LS_CHECK(p25_acquisition_accept_sync(&replay.gate, replay.generation, sync));
    replay.change_on_refill = true;
    processFrame(&replay.opts, &replay.state);
    LS_CHECK(!p25_acquisition_accept_frame(&replay.gate, replay.generation));
    LS_EQ_UINT(replay.gate.discarded_frames, 1);
    LS_CHECK(prepare(false, false));
    LS_EQ_UINT(replay.state.p25_frame_valid, 0);
    LS_EQ_INT(replay.state.pcm_out_write, 0);
    LS_CHECK(replay_frame());
}

LS_CASE(silent_hunt_times_out_and_a_later_clean_burst_reacquires)
{
    static uint8_t silent[200000];
    memset(silent, 128, sizeof(silent));
    replay_init(silent, sizeof(silent), 16384);
    LS_CHECK(prepare(false, false));
    LS_CHECK(!replay_frame());
    LS_EQ_UINT(replay.state.acquisition_hunt.attempts, 1);
    LS_EQ_UINT(replay.state.acquisition_hunt.timeouts, 1);
    LS_EQ_UINT(replay.state.acquisition_hunt.raw_syncs, 0);
    LS_EQ_UINT(replay.state.acquisition_hunt.symbols, 1800);
    LS_EQ_INT(replay.state.acquisition_hunt.symbol_min, 0);
    LS_EQ_INT(replay.state.acquisition_hunt.symbol_max, 0);
    LS_CHECK(replay.state.acquisition_hunt.best_normal_hd > 0);
    replay.iq = protocol_iq();
    replay.iq_bytes = IQ_BYTES;
    replay.offset = 0;
    /* No synthetic reset here: the production hunt timeout resets carrier. */
    LS_CHECK(replay_frame());
    LS_EQ_UINT(replay.state.acquisition_hunt.attempts, 2);
    LS_EQ_INT(replay.state.nac, 0x293);
}

LS_CASE(generation_wrap_and_stable_ack_do_not_allocate_or_repeatedly_reset)
{
    replay_init(protocol_iq(), IQ_BYTES, 16384);
    ls_shim_heap_reset();
    replay.gate.generation = UINT32_MAX;
    replay.generation = 0;
    LS_CHECK(!prepare(true, false));
    for (int i = 0; i < 20; i++) LS_CHECK(prepare(false, false));
    LS_EQ_UINT(replay.gate.resets, 1);
    LS_CHECK(replay_frame());
    p25_iq_probe_t probe;
    p25_acquisition_probe_iq(&probe, replay.iq, 16384);
    LS_EQ_UINT(ls_shim_heap_call_count(), 0);
}

LS_CASE(production_stream_decodes_all_frames_across_chunk_splits_and_noise)
{
    const size_t chunks[] = {1024, 4096, 16384};
    for (unsigned c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
        replay_init(protocol_iq_variant(0, 0.03f), IQ_BYTES, chunks[c]);
        LS_CHECK(prepare(false, false));
        int frames = 0;
        while (!exitflag) {
            if (replay_frame()) frames++;
        }
        ls_note("live-path chunk=%u frames=%d NIDfail=%d TSBK=%u",
                (unsigned)chunks[c], frames, dsd_bch_fail_counter,
                replay.state.p25_tsbk_valid_count);
        LS_EQ_INT(frames, N_FRAMES);
        LS_EQ_INT(autoscan_bch_ok_flag, N_FRAMES);
        LS_EQ_INT(dsd_bch_fail_counter, 0);
        LS_EQ_UINT(replay.state.p25_tsbk_valid_count, N_FRAMES);
        LS_EQ_UINT(replay.state.acquisition_hunt.raw_syncs, N_FRAMES);
    }
}

LS_CASE(noise_has_no_valid_frames_and_inverted_sync_is_diagnostic_not_a_lock)
{
    static uint8_t noise[200000];
    ls_rng_t rng;
    ls_rng_seed(&rng, 760u);
    for (size_t i = 0; i < sizeof(noise); i++)
        noise[i] = (uint8_t)(110 + ls_rng_u32(&rng) % 36);
    replay_init(noise, sizeof(noise), 4096);
    LS_CHECK(prepare(false, false));
    while (!exitflag) LS_CHECK(!replay_frame());
    LS_EQ_UINT(replay.state.acquisition_hunt.raw_syncs, 0);
    LS_EQ_INT(autoscan_bch_ok_flag, 0);
    LS_EQ_UINT(replay.state.p25_tsbk_valid_count, 0);

    uint8_t *iq = protocol_iq();
    unsigned inverted = 0;
    /* A nonlocking 1800-symbol hunt can split a sync across two windows.
     * Isolated bursts make every counted pattern complete within a hunt. */
    for (unsigned i = 0; i < N_FRAMES; i++) {
        replay_init(iq + i * FRAME_LEN * 100, FRAME_LEN * 100, 16384);
        dsp_set_gain(&replay.dsp, 9000.0f);
        LS_CHECK(prepare(false, false));
        while (!exitflag) LS_CHECK(!replay_frame());
        LS_EQ_UINT(replay.state.acquisition_hunt.raw_syncs, 0);
        LS_EQ_UINT(replay.state.acquisition_hunt.inverted_matches, 1);
        inverted += replay.state.acquisition_hunt.inverted_matches;
        LS_EQ_UINT(replay.gate.accepted_syncs, 0);
        LS_EQ_INT(autoscan_bch_ok_flag, 0);
    }
    LS_EQ_UINT(inverted, N_FRAMES);
}

LS_CASE(exact_sync_with_corrupt_nid_does_not_become_a_valid_frame)
{
    static int symbols[N_SYMBOLS];
    static int rendered[RENDER_SYMBOLS];
    static uint8_t iq[IQ_BYTES];
    ls_rng_t rng;
    ls_rng_seed(&rng, 760u);
    make_protocol_symbols(symbols, &rng);
    memcpy(rendered, symbols, sizeof(symbols));
    for (int i = 0; i < NID_DIBITS; i++)
        rendered[LSM_SYNC_LEN + i] = dibit_to_symbol(ls_rng_u32(&rng) & 3u);
    for (int i = N_SYMBOLS; i < RENDER_SYMBOLS; i++) rendered[i] = 1;
    render_c4fm_iq(rendered, 0, 0, NULL, iq);
    replay_init(iq, FRAME_LEN * 100, 4096);
    LS_CHECK(prepare(false, false));
    LS_CHECK(!replay_frame());
    LS_EQ_UINT(replay.state.acquisition_hunt.raw_syncs, 1);
    LS_EQ_INT(dsd_bch_fail_counter, 1);
    LS_EQ_INT(autoscan_bch_ok_flag, 0);
    LS_EQ_UINT(replay.state.p25_frame_valid, 0);
    LS_EQ_UINT(replay.state.p25_tsbk_valid_count, 0);
}

LS_CASE(acquisition_format_is_bounded_and_reports_zero_as_unobserved)
{
    p25_acquisition_status_t status = {0};
    char text[768];
    size_t needed = p25_acquisition_format(&status, text, sizeof(text));
    LS_CHECK(needed > 0 && needed < sizeof(text));
    LS_CHECK(strstr(text, "decoder_ms=0 iq_ms=0") != NULL);
    LS_CHECK(strstr(text, "attempts=0 timeouts=0 symbols=0") != NULL);
    char tiny[4] = {'x', 'x', 'x', 'x'};
    LS_EQ_UINT(p25_acquisition_format(&status, tiny, sizeof(tiny)), needed);
    LS_EQ_INT(tiny[3], 0);
    LS_EQ_UINT(p25_acquisition_format(&status, NULL, 0), 0);
    status.hunt.inverted_matches = 20;
    status.gate.discarded_syncs = 7;
    status.iq.mean_i_milli = -1250;
    p25_acquisition_format(&status, text, sizeof(text));
    LS_CHECK(strstr(text, "discard_sync=7") != NULL);
    LS_CHECK(strstr(text, "inverted=20") != NULL);
    LS_CHECK(strstr(text, "dc_milli=-1250/0") != NULL);
}
