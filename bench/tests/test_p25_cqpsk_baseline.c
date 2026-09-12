/* LS_TEST_SOURCES: ${APP}/p25/dsp_pipeline.c ${APP}/p25/dsd_symbol.c ${APP}/p25/dsd_frame_sync.c ${APP}/p25/dsd_filters.c ${APP}/p25/p25_tsbk.c fixtures/lsm_gen.c */
/* /protocol-level CQPSK coverage and live-shape C4FM replay. */
#include "ls_test.h"
#include "dsp_pipeline.h"
#include "lsm_gen.h"
#include "p25_tsbk.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "p25p1_check_nid.c"

void sys_log(unsigned char color, const char *fmt, ...)
{
    (void)color;
    (void)fmt;
}

#ifndef LS_P25_REAL_ACQUISITION
int exitflag;
volatile int dsd_abort;

void dsd_yield(void) {}

int comp(const void *a, const void *b)
{
    const int left = *(const int *)a;
    const int right = *(const int *)b;
    return (left > right) - (left < right);
}

void noCarrier(dsd_opts *opts, dsd_state *state)
{
    (void)opts;
    state->carrier = 0;
    state->lastsynctype = -1;
    state->jitter = -1;
}
#endif

enum {
    NID_DIBITS = 33,
    TSDU_DIBITS = 328,
    FRAME_LEN = LSM_SYNC_LEN + NID_DIBITS + TSDU_DIBITS,
    N_FRAMES = 20,
    EXPECTED_COMPLETE_FRAMES = 19,
    TAIL_SYMBOLS = 64,
    N_SYMBOLS = FRAME_LEN * N_FRAMES,
    RENDER_SYMBOLS = N_SYMBOLS + TAIL_SYMBOLS,
    IQ_BYTES = RENDER_SYMBOLS * 100,
    AUDIO_SAMPLES = (RENDER_SYMBOLS + 32) * DSP_SPS,
    RX_SYMBOLS = RENDER_SYMBOLS + 32,
};

typedef struct {
    int syncs;
    int valid_nids;
    int valid_tsbks;
} decode_result_t;

typedef struct {
    const char *name;
    float noise_sd;
    float freq_off_hz;
    int expected_syncs;
    int expected_nids;
    int expected_tsbks;
} baseline_case_t;

static decode_result_t count_protocol(const int *rx, int n_rx);

static const baseline_case_t BASELINE_CASES[] = {
    { "clean -200 Hz", 0.00f, -200.0f, 19, 19, 19 },
    { "clean 0 Hz",    0.00f,    0.0f, 19, 19, 19 },
    { "clean +200 Hz", 0.00f,  200.0f, 19, 19, 19 },
    { "noise -200 Hz", 0.03f, -200.0f, 19, 19, 19 },
    { "noise 0 Hz",    0.03f,    0.0f, 19, 19, 19 },
    { "noise +200 Hz", 0.03f,  200.0f, 19, 19, 19 },
};

static const uint8_t s_constellation[4][4] = {
    { 0x2, 0xc, 0x1, 0xf },
    { 0xe, 0x0, 0xd, 0x3 },
    { 0x9, 0x7, 0xa, 0x4 },
    { 0x5, 0xb, 0x6, 0x8 },
};

static int dibit_to_symbol(unsigned int dibit)
{
    static const int LEVELS[4] = { 1, 3, -1, -3 };
    return LEVELS[dibit & 3u];
}

static int symbol_to_dibit(int symbol)
{
    return symbol == 3 ? 1 : symbol == 1 ? 0 : symbol == -1 ? 2 : 3;
}

static void make_nid(uint8_t out[NID_DIBITS])
{
    int data[BCH_KK];
    int parity[BCH_RR];
    char codeword[BCH_NN];
    const int nac = 0x293;
    const int duid = 0x7;

    for (int i = 0; i < 12; i++) data[i] = (nac >> (11 - i)) & 1;
    for (int i = 0; i < 4; i++) data[12 + i] = (duid >> (3 - i)) & 1;
    bch_encode(data, parity);
    for (int i = 0; i < BCH_KK; i++) codeword[i] = (char)data[i];
    for (int i = 0; i < BCH_RR; i++) codeword[BCH_KK + i] = (char)parity[i];

    for (int i = 0; i < 11; i++)
        out[i] = (uint8_t)((codeword[i * 2] << 1) | codeword[i * 2 + 1]);
    out[11] = 0;
    for (int i = 0; i < 20; i++)
        out[12 + i] = (uint8_t)((codeword[22 + i * 2] << 1)
                                | codeword[23 + i * 2]);
    out[32] = (uint8_t)(codeword[62] << 1);
}

static void set_bits(uint8_t block[P25_TSBK_BYTES], unsigned first,
                     unsigned count, uint32_t value)
{
    for (unsigned i = 0; i < count; i++) {
        unsigned bit = first + i;
        uint8_t mask = (uint8_t)(1u << (7 - bit % 8));
        if ((value >> (count - 1 - i)) & 1u)
            block[bit / 8] |= mask;
        else
            block[bit / 8] &= (uint8_t)~mask;
    }
}

static void add_crc(uint8_t block[P25_TSBK_BYTES])
{
    block[10] = 0;
    block[11] = 0;
    uint16_t crc = p25_tsbk_crc16(block, P25_TSBK_BYTES);
    block[10] = (uint8_t)(crc >> 8);
    block[11] = (uint8_t)crc;
}

static void make_tsbk(uint8_t wire[P25_TSBK_ENCODED_DIBITS])
{
    uint8_t block[P25_TSBK_BYTES] = { 0 };
    uint8_t decoded[P25_TSBK_DECODED_DIBITS];
    uint8_t encoded[P25_TSBK_ENCODED_DIBITS];

    block[0] = 0x80 | 0x3b;
    set_bits(block, 24, 20, 0xabcde);
    set_bits(block, 44, 12, 0x789);
    add_crc(block);
    for (unsigned i = 0; i < P25_TSBK_DECODED_DIBITS; i++)
        decoded[i] = (uint8_t)((block[i / 4] >> (6 - (i % 4) * 2)) & 3);

    uint8_t state = 0;
    for (unsigned i = 0; i <= P25_TSBK_DECODED_DIBITS; i++) {
        uint8_t next = i == P25_TSBK_DECODED_DIBITS ? 0 : decoded[i];
        uint8_t word = s_constellation[state][next];
        encoded[i * 2] = (uint8_t)(word >> 2);
        encoded[i * 2 + 1] = (uint8_t)(word & 3);
        state = next;
    }

    unsigned out = 0;
    for (unsigned start = 0; start < 8; start += 2) {
        unsigned end = start == 0 ? 98 : 96;
        for (unsigned i = start; i < end; i += 8) {
            wire[out++] = encoded[i];
            wire[out++] = encoded[i + 1];
        }
    }
}

static void make_protocol_symbols(int out[N_SYMBOLS], ls_rng_t *rng)
{
    uint8_t nid[NID_DIBITS];
    uint8_t tsbk[P25_TSBK_ENCODED_DIBITS];
    make_nid(nid);
    make_tsbk(tsbk);

    int n = 0;
    for (int frame = 0; frame < N_FRAMES; frame++) {
        for (int i = 0; i < LSM_SYNC_LEN; i++) out[n++] = LSM_SYNC[i];
        for (int i = 0; i < NID_DIBITS; i++) out[n++] = dibit_to_symbol(nid[i]);

        unsigned encoded = 0;
        for (int i = 0; i < TSDU_DIBITS; i++) {
            unsigned dibit;
            if (i >= 14 && (i - 14) % 36 == 0)
                dibit = 0;
            else if (encoded < P25_TSBK_ENCODED_DIBITS)
                dibit = tsbk[encoded++];
            else
                dibit = ls_rng_u32(rng) & 3u;
            out[n++] = dibit_to_symbol(dibit);
        }
    }
    LS_EQ_INT(n, N_SYMBOLS);
}

static void render_c4fm_iq(const int tx[RENDER_SYMBOLS], float freq_off_hz,
                           float noise_sd, ls_rng_t *rng,
                           uint8_t iq[IQ_BYTES])
{
    const int samples_per_symbol = DSP_SAMPLE_RATE / DSP_BAUD;
    double phase = 0.0;
    int previous = tx[0];
    int out = 0;

    for (int symbol = 0; symbol < RENDER_SYMBOLS; symbol++) {
        int current = tx[symbol];
        for (int sample = 0; sample < samples_per_symbol; sample++) {
            /* A half-cosine transition keeps the generated discriminator
             * waveform band-limited without making the fixture depend on
             * the receiver's own RRC coefficients. */
            double level = (double)current;
            int transition = samples_per_symbol / 2;
            if (sample < transition) {
                double u = (double)sample / (double)transition;
                double blend = 0.5 - 0.5 * cos(M_PI * u);
                level = previous + (current - previous) * blend;
            }
            phase += 2.0 * M_PI * (level * 600.0 + freq_off_hz)
                   / DSP_SAMPLE_RATE;
            double ni = 0.0;
            double nq = 0.0;
            if (noise_sd > 0.0f && rng) {
                /* ls_rng_noise has sd ~= 0.289; express noise_sd relative
                 * to the unit-amplitude carrier, like lsm_render_iq. */
                double gain = 110.0 * noise_sd / 0.289;
                ni = gain * ls_rng_noise(rng);
                nq = gain * ls_rng_noise(rng);
            }
            int i = (int)(127.5 + 110.0 * cos(phase) + ni);
            int q = (int)(127.5 + 110.0 * sin(phase) + nq);
            if (i < 0) i = 0; else if (i > 255) i = 255;
            if (q < 0) q = 0; else if (q > 255) q = 255;
            iq[out++] = (uint8_t)i;
            iq[out++] = (uint8_t)q;
        }
        previous = current;
    }
    LS_EQ_INT(out, IQ_BYTES);
}

static int c4fm_sliced_symbol(const int16_t *audio, int sample)
{
    int value = 0;
    for (int i = 3; i <= 6; i++) value += audio[sample + i];
    value /= 4;
    return value > 1100 ? 3 : value > 0 ? 1 : value > -1100 ? -1 : -3;
}

static decode_result_t demodulate_c4fm(const uint8_t iq[IQ_BYTES])
{
    static int16_t audio[AUDIO_SAMPLES];
    static int rx[RX_SYMBOLS];
    dsp_state_t dsp;
    dsp_init(&dsp);
    dsp_set_mode(&dsp, DEMOD_C4FM);
    /* This is the polarity selected by app_p25.c when invert is off. */
    dsp_set_gain(&dsp, -9000.0f);
    int n_audio = 0;
    for (int offset = 0; offset < IQ_BYTES; offset += 16384) {
        int bytes = IQ_BYTES - offset;
        if (bytes > 16384) bytes = 16384;
        n_audio += dsp_process_iq(&dsp, iq + offset, bytes,
                                  audio + n_audio, AUDIO_SAMPLES - n_audio);
    }
    LS_EQ_INT(n_audio, IQ_BYTES / 2 / DSP_DECIMATION);

    decode_result_t best = { 0 };
    for (int phase = 0; phase < DSP_SPS; phase++) {
        int n_rx = 0;
        for (int sample = phase;
             sample + DSP_SPS <= n_audio && n_rx < RX_SYMBOLS;
             sample += DSP_SPS)
            rx[n_rx++] = c4fm_sliced_symbol(audio, sample);
        decode_result_t result = count_protocol(rx, n_rx);
        if (result.valid_tsbks > best.valid_tsbks) best = result;
    }
    return best;
}

static decode_result_t demodulate_fsk4_tracking(const uint8_t iq[IQ_BYTES])
{
    static int16_t audio[AUDIO_SAMPLES];
    static int rx[RX_SYMBOLS];
    dsp_state_t dsp;
    dsp_init(&dsp);
    dsp_set_mode(&dsp, DEMOD_FSK4_TRACKING);
    dsp_set_gain(&dsp, 9000.0f);

    int n_audio = 0;
    for (int offset = 0; offset < IQ_BYTES; offset += 16384) {
        int bytes = IQ_BYTES - offset;
        if (bytes > 16384) bytes = 16384;
        n_audio += dsp_process_iq(&dsp, iq + offset, bytes,
                                  audio + n_audio, AUDIO_SAMPLES - n_audio);
    }

    LS_NEAR(dsp.ft_symbol_time, 0.1, 1e-12);
    LS_EQ_INT(n_audio, RENDER_SYMBOLS * DSP_SPS);
    LS_EQ_INT(n_audio % DSP_SPS, 0);
    int n_rx = 0;
    for (int sample = 0; sample < n_audio && n_rx < RX_SYMBOLS;
         sample += DSP_SPS) {
        for (int repeat = 1; repeat < DSP_SPS; repeat++)
            LS_EQ_INT(audio[sample + repeat], audio[sample]);
        int value = audio[sample];
        rx[n_rx++] = value > 8000 ? 3 : value > 0 ? 1
                   : value > -8000 ? -1 : -3;
    }
    return count_protocol(rx, n_rx);
}

static int valid_nid_at(const int *rx, int pos)
{
    char codeword[BCH_NN];
    int bit = 0;
    for (int i = 0; i < NID_DIBITS; i++) {
        int dibit = symbol_to_dibit(rx[pos + i]);
        if (i == 11) continue;
        codeword[bit++] = (char)((dibit >> 1) & 1);
        if (bit < BCH_NN) codeword[bit++] = (char)(dibit & 1);
    }
    int nac = 0;
    int errors = 0;
    char duid[3] = { 0 };
    return bit == BCH_NN
        && check_NID_ec(codeword, &nac, duid, 0, &errors)
        && nac == 0x293
        && strcmp(duid, "13") == 0;
}

static int valid_tsbk_at(const int *rx, int pos)
{
    uint8_t wire[TSDU_DIBITS];
    dsd_state state;
    memset(&state, 0, sizeof(state));
    for (int i = 0; i < TSDU_DIBITS; i++)
        wire[i] = (uint8_t)symbol_to_dibit(rx[pos + i]);
    return p25_tsbk_process_tsdu(&state, wire, TSDU_DIBITS) == 1
        && state.p25_tsbk_wacn == 0xabcde
        && state.p25_tsbk_sysid == 0x789;
}

static decode_result_t count_protocol(const int *rx, int n_rx)
{
    decode_result_t result = { 0 };
    /* Search the recovered stream. Do not use transmitted frame positions:
     * acquisition and all filter/timing latency must be visible here. */
    for (int i = 0; i + FRAME_LEN <= n_rx; i++) {
        int k = 0;
        while (k < LSM_SYNC_LEN && rx[i + k] == LSM_SYNC[k]) k++;
        if (k != LSM_SYNC_LEN) continue;
        result.syncs++;
        if (valid_nid_at(rx, i + LSM_SYNC_LEN)) result.valid_nids++;
        if (valid_tsbk_at(rx, i + LSM_SYNC_LEN + NID_DIBITS))
            result.valid_tsbks++;
        i += FRAME_LEN - 1;
    }
    return result;
}

static decode_result_t demodulate(const uint8_t iq[IQ_BYTES])
{
    static int16_t audio[AUDIO_SAMPLES];
    static int rx[RX_SYMBOLS];
    dsp_state_t dsp;
    dsp_init(&dsp);
    p25_cqpsk_config_t tested;
    p25_cqpsk_config_defaults(&tested);
    LS_CHECK(dsp_set_cqpsk_loops(&dsp, &tested));
    dsp_set_mode(&dsp, DEMOD_CQPSK);
    int n_audio = dsp_process_iq(&dsp, iq, IQ_BYTES, audio, AUDIO_SAMPLES);
    int n_rx = lsm_slice(audio, n_audio, dsp.demod_gain, rx, RX_SYMBOLS);
    return count_protocol(rx, n_rx);
}

static decode_result_t run_channel(const int tx[N_SYMBOLS],
                                   const baseline_case_t *test_case,
                                   uint32_t noise_seed)
{
    static uint8_t iq[IQ_BYTES];
    static int rendered[RENDER_SYMBOLS];
    memcpy(rendered, tx, sizeof(int) * N_SYMBOLS);
    /* A tail supplies the samples needed to flush the transmitter RRC and
     * receive filters; protocol scanning still begins at rx[0]. */
    for (int i = N_SYMBOLS; i < RENDER_SYMBOLS; i++) rendered[i] = 1;

    lsm_channel_t channel;
    lsm_channel_clean(&channel);
    channel.noise_sd = test_case->noise_sd;
    channel.freq_off_hz = test_case->freq_off_hz;
    ls_rng_t noise_rng;
    ls_rng_seed(&noise_rng, noise_seed);
    LS_EQ_INT(lsm_render_iq(rendered, RENDER_SYMBOLS, &channel, &noise_rng,
                            iq, sizeof(iq)), IQ_BYTES);
    return demodulate(iq);
}

LS_CASE(clean_and_noisy_signed_offset_protocol_counts)
{
    static int tx[N_SYMBOLS];
    ls_rng_t data_rng;
    ls_rng_seed(&data_rng, 0x660u);
    make_protocol_symbols(tx, &data_rng);

    for (unsigned i = 0; i < sizeof(BASELINE_CASES) / sizeof(BASELINE_CASES[0]); i++) {
        const baseline_case_t *test_case = &BASELINE_CASES[i];
        decode_result_t result = run_channel(tx, test_case, 0x310u);
        ls_note("%s: sync=%d NID=%d TSBK=%d", test_case->name,
                result.syncs, result.valid_nids, result.valid_tsbks);
        LS_EQ_INT(result.syncs, test_case->expected_syncs);
        LS_EQ_INT(result.valid_nids, test_case->expected_nids);
        LS_EQ_INT(result.valid_tsbks, test_case->expected_tsbks);
        LS_EQ_INT(result.valid_tsbks, EXPECTED_COMPLETE_FRAMES);
    }
}

LS_CASE(clean_c4fm_protocol_counts)
{
    static int tx[N_SYMBOLS];
    static int rendered[RENDER_SYMBOLS];
    static uint8_t iq[IQ_BYTES];
    ls_rng_t data_rng;
    ls_rng_seed(&data_rng, 0x731u);
    make_protocol_symbols(tx, &data_rng);
    memcpy(rendered, tx, sizeof(tx));
    for (int i = N_SYMBOLS; i < RENDER_SYMBOLS; i++) rendered[i] = 1;

    render_c4fm_iq(rendered, 0.0f, 0.0f, NULL, iq);
    decode_result_t result = demodulate_c4fm(iq);
    ls_note("clean C4FM: sync=%d NID=%d TSBK=%d",
            result.syncs, result.valid_nids, result.valid_tsbks);
    LS_EQ_INT(result.syncs, N_FRAMES);
    LS_EQ_INT(result.valid_nids, N_FRAMES);
    LS_EQ_INT(result.valid_tsbks, N_FRAMES);
}

/* the retired 21-tap/5-sps filter produced zero syncs, NIDs and
 * TSBKs for every case below at the live 48 kHz boundary. This drives the
 * production tracker in live 16 KiB blocks and pins its ten-sample DSD shape
 * before accepting BCH-valid NIDs and CRC-valid TSBKs. */
LS_CASE(fsk4_tracking_recovers_protocol_at_ten_samples_per_symbol)
{
    static const struct {
        const char *name;
        float freq_off_hz;
        float noise_sd;
        int min_valid_frames;
    } cases[] = {
        { "clean",          0.0f, 0.00f, 18 },
        { "offset -150", -150.0f, 0.00f, 16 },
        { "offset +150",  150.0f, 0.00f, 16 },
        { "noise/offset", 100.0f, 0.02f, 14 },
    };
    static int tx[N_SYMBOLS];
    static int rendered[RENDER_SYMBOLS];
    static uint8_t iq[IQ_BYTES];
    ls_rng_t data_rng;
    ls_rng_seed(&data_rng, 0x732u);
    make_protocol_symbols(tx, &data_rng);
    memcpy(rendered, tx, sizeof(tx));
    for (int i = N_SYMBOLS; i < RENDER_SYMBOLS; i++) rendered[i] = 1;

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ls_rng_t noise_rng;
        ls_rng_seed(&noise_rng, 0x732100u + i);
        render_c4fm_iq(rendered, cases[i].freq_off_hz, cases[i].noise_sd,
                       &noise_rng, iq);
        decode_result_t result = demodulate_fsk4_tracking(iq);
        ls_note("FSK4 %s: sync=%d NID=%d TSBK=%d", cases[i].name,
                result.syncs, result.valid_nids, result.valid_tsbks);
        LS_CHECK_MSG(result.valid_nids >= cases[i].min_valid_frames,
                     "%s: only %d BCH-valid NIDs", cases[i].name,
                     result.valid_nids);
        LS_CHECK_MSG(result.valid_tsbks >= cases[i].min_valid_frames,
                     "%s: only %d CRC-valid TSBKs", cases[i].name,
                     result.valid_tsbks);
    }
}

LS_CASE(clean_c4fm_reaches_production_dsd_frame_sync)
{
    LS_EQ_INT(SAMPLE_RATE_IN, DSP_AUDIO_RATE);
    LS_EQ_INT(DSP_SPS, SAMPLE_RATE_IN / DSP_BAUD);

    static int tx[N_SYMBOLS];
    static int rendered[RENDER_SYMBOLS];
    static uint8_t iq[IQ_BYTES];
    static int16_t audio[DSD_SAMPLE_RING_SIZE - 1];
    static dsd_sample_ring_t ring;
    static dsd_state state;
    static int dibit_buf[10000];
    ls_rng_t data_rng;
    ls_rng_seed(&data_rng, 0x7315u);
    make_protocol_symbols(tx, &data_rng);
    memcpy(rendered, tx, sizeof(tx));
    for (int i = N_SYMBOLS; i < RENDER_SYMBOLS; i++) rendered[i] = 1;
    render_c4fm_iq(rendered, 0.0f, 0.0f, NULL, iq);

    dsp_state_t dsp;
    dsp_init(&dsp);
    dsp_set_mode(&dsp, DEMOD_C4FM);
    dsp_set_gain(&dsp, -9000.0f);
    int n_audio = 0;
    const int input_bytes = 2 * FRAME_LEN * (IQ_BYTES / RENDER_SYMBOLS);
    for (int offset = 0; offset < input_bytes; offset += 16384) {
        int bytes = input_bytes - offset;
        if (bytes > 16384) bytes = 16384;
        n_audio += dsp_process_iq(&dsp, iq + offset, bytes,
                                  audio + n_audio,
                                  (int)(sizeof(audio) / sizeof(audio[0])) - n_audio);
    }
    LS_EQ_INT(n_audio, 2 * FRAME_LEN * DSP_SPS);

    memset(&ring, 0, sizeof(ring));
    memcpy(ring.buf, audio, (size_t)n_audio * sizeof(audio[0]));
    ring.write_idx = n_audio;

    memset(&state, 0, sizeof(state));
    state.dibit_buf = dibit_buf;
    state.dibit_buf_p = dibit_buf + 200;
    state.samplesPerSymbol = DSP_SPS;
    state.symbolCenter = 4;
    state.jitter = -1;
    state.synctype = -1;
    state.lastsynctype = -1;
    state.min = -15000;
    state.max = 15000;
    state.minref = -15000;
    state.maxref = 15000;
    for (int i = 0; i < 1024; i++) {
        state.minbuf[i] = -15000;
        state.maxbuf[i] = 15000;
    }

    dsd_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.frame_p25p1 = 1;
    opts.mod_c4fm = 1;
    opts.mod_threshold = 26;
    opts.ssize = 36;
    opts.msize = 256;
    opts.ring = &ring;

    LS_EQ_INT(getFrameSync(&opts, &state), 0);
    LS_CHECK_MSG(ring.read_idx < ring.write_idx,
                 "DSD consumed the whole replay without finding P25 sync");
}

LS_CASE(noise_only_has_no_protocol_lock)
{
    static uint8_t iq[IQ_BYTES];
    ls_rng_t rng;
    ls_rng_seed(&rng, 0x679deadU);
    for (int i = 0; i < IQ_BYTES; i++) {
        int sample = 128 + (int)(ls_rng_noise(&rng) * 14.0f);
        if (sample < 0) sample = 0;
        if (sample > 255) sample = 255;
        iq[i] = (uint8_t)sample;
    }
    decode_result_t result = demodulate(iq);
    ls_note("noise only: sync=%d NID=%d TSBK=%d",
            result.syncs, result.valid_nids, result.valid_tsbks);
    LS_EQ_INT(result.syncs, 0);
    LS_EQ_INT(result.valid_nids, 0);
    LS_EQ_INT(result.valid_tsbks, 0);
}

LS_CASE(polarity_and_gain_api_contract_is_preserved)
{
    dsp_state_t dsp;
    dsp_init(&dsp);

    dsp_set_mode(&dsp, DEMOD_C4FM);
    dsp_set_gain(&dsp, 1234.0f);
    dsp_flip_polarity(&dsp);
    LS_NEAR(dsp.demod_gain, -1234.0f, 0.0f);
    LS_EQ_INT(dsp.cqpsk_polarity, 0);

    dsp_set_mode(&dsp, DEMOD_CQPSK);
    dsp_set_gain(&dsp, 2345.0f);
    for (int i = 1; i <= 8; i++) {
        dsp_flip_polarity(&dsp);
        LS_NEAR(dsp.demod_gain, 2345.0f, 0.0f);
        LS_EQ_INT(dsp.cqpsk_polarity, i % 8);
    }
}
