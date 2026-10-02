/* LS_TEST_SOURCES: experiments/lr433_dec.c and experiments/exp_lr433.c, with
   ls_experiments.c, against recordings from rtl_433_tests turned into what the
   LR2021's sessions would hand over, and a faked LoRa chip: every recording
   decodes to what rtl_433 said of it, noise decodes as nothing, FSK in either
   polarity, and the experiment's plan, round robin, device table and the
   count of what keyed but did not decode */

#include "ls_test.h"

#include "experiments/lr433_dec.h"
#include "ls_experiments.h"
#include "ls_lora.h"
#include "esp_timer.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "exp_lr433_vectors.h"

void ls_shim_time_set(int64_t us);
void ls_shim_time_advance(int64_t us);

#define N_VECS ((int)(sizeof(VECS) / sizeof(VECS[0])))

static lr433_work_t W;

/* A vector's frame with its trailing zeros put back. */
static uint8_t s_frame[256];

static lr433_capture_t capture_of(const lr433_vec_t *v)
{
    memset(s_frame, 0, sizeof(s_frame));
    memcpy(s_frame, v->data, (size_t)v->len);
    const lr433_capture_t c = {
        .kind = v->kind, .rate = v->rate, .lead = v->lead, .lead_bits = v->lead_bits,
        .data = s_frame, .bits = v->bits,
    };
    return c;
}

static const lr433_msg_t *find_msg(const lr433_msg_t *m, int n, const lr433_vec_t *v)
{
    for (int i = 0; i < n; i++)
        if (!strncmp(m[i].model, v->model, strlen(v->model)) && !strcmp(m[i].id, v->id)) return &m[i];
    return NULL;
}

/* -------------------------------------------------------------- decoders -- */

LS_CASE(every_recording_decodes_to_what_rtl_433_said_of_it)
{
    LS_CHECK(N_VECS >= 20);
    for (int i = 0; i < N_VECS; i++) {
        const lr433_vec_t *v = &VECS[i];
        const lr433_capture_t c = capture_of(v);
        lr433_msg_t m[8];
        const int n = lr433_decode(&c, &W, m, 8);
        const lr433_msg_t *got = find_msg(m, n, v);
        LS_CHECK_MSG(got != NULL, "%s: no %s %s among %d decoded", v->source, v->model, v->id, n);
        if (!got) continue;
        if (isnan(v->kpa)) LS_CHECK_MSG(isnan(got->kpa), "%s: pressure where none was sent", v->source);
        else LS_CHECK_MSG(fabs(got->kpa - v->kpa) < 0.05, "%s: %.2f kPa, want %.2f", v->source, got->kpa, v->kpa);
        if (!isnan(v->temp_c))
            LS_CHECK_MSG(fabs(got->temp_c - v->temp_c) < 0.05, "%s: %.2f C, want %.2f", v->source, got->temp_c,
                         v->temp_c);
        if (!isnan(v->humidity))
            LS_CHECK_MSG(fabs(got->humidity - v->humidity) < 0.01, "%s: %.0f %%, want %.0f", v->source,
                         got->humidity, v->humidity);
        LS_CHECK_MSG(got->consumption == v->consumption, "%s: consumption %lld, want %lld", v->source,
                     (long long)got->consumption, v->consumption);
    }
}

LS_CASE(the_protocols_cover_tpms_sensors_and_meters)
{
    bool seen[LR433_P_COUNT] = { false };
    for (int i = 0; i < N_VECS; i++) {
        const lr433_capture_t c = capture_of(&VECS[i]);
        lr433_msg_t m[8];
        const int n = lr433_decode(&c, &W, m, 8);
        for (int k = 0; k < n; k++) seen[m[k].proto] = true;
    }
    LS_CHECK(seen[LR433_P_SCHRADER] && seen[LR433_P_SCHRADER_EG53MA4] && seen[LR433_P_SCHRADER_SMD3MA4]);
    LS_CHECK(seen[LR433_P_GM_AFTERMARKET] && seen[LR433_P_FORD] && seen[LR433_P_TOYOTA]);
    LS_CHECK(seen[LR433_P_PMV107J] && seen[LR433_P_CITROEN] && seen[LR433_P_ELANTRA2012]);
    LS_CHECK(seen[LR433_P_ACURITE_TOWER] && seen[LR433_P_ACURITE_5N1] && seen[LR433_P_LACROSSE_TX141]);
    LS_CHECK(seen[LR433_P_AMBIENT_F007TH] && seen[LR433_P_FINEOFFSET_WH2]);
    LS_CHECK(seen[LR433_P_AMBIENT_WH31E] && seen[LR433_P_FINEOFFSET_WH24]);
    LS_CHECK(seen[LR433_P_ERT_SCM] && seen[LR433_P_ERT_SCMPLUS] && seen[LR433_P_ERT_IDM]);
    for (int p = 0; p < LR433_P_COUNT; p++) LS_CHECK(strlen(lr433_proto_label(p)) <= 9);
}

LS_CASE(a_message_repeated_in_one_capture_is_one_message)
{
    /* The 592TXR sends each message three times in a row. */
    for (int i = 0; i < N_VECS; i++) {
        if (strcmp(VECS[i].source, "acurite/Acurite_592TXR/acurite-592txr-003")) continue;
        const lr433_capture_t c = capture_of(&VECS[i]);
        lr433_msg_t m[8];
        LS_EQ_INT(lr433_decode(&c, &W, m, 8), 1);
        LS_EQ_INT(m[0].channel, 'C');
        LS_EQ_INT(m[0].battery_ok, 1);
        return;
    }
    LS_CHECK_MSG(0, "the 592TXR recording is missing");
}

LS_CASE(fsk_decodes_whichever_way_round_the_chip_puts_the_tones)
{
    int tried = 0;
    for (int i = 0; i < N_VECS; i++) {
        const lr433_vec_t *v = &VECS[i];
        if (v->kind != LR433_IN_FSK) continue;
        lr433_capture_t c = capture_of(v);
        for (int k = 0; k < v->bits / 8; k++) s_frame[k] = (uint8_t)~s_frame[k];
        c.lead = ~c.lead & ((1u << c.lead_bits) - 1u);
        lr433_msg_t m[8];
        const int n = lr433_decode(&c, &W, m, 8);
        LS_CHECK_MSG(find_msg(m, n, v) != NULL, "%s inverted: not decoded", v->source);
        tried++;
    }
    LS_CHECK(tried >= 7);
}

LS_CASE(noise_decodes_as_nothing)
{
    static const struct { lr433_in_t kind; uint32_t rate, lead; uint8_t lead_bits; int bytes; } S[] = {
        { LR433_IN_OOK, 40000, 0x00F, 12, 252 }, { LR433_IN_OOK, 16000, 0x00F, 12, 252 },
        { LR433_IN_FSK, 76800, 0x0F0F, 16, 128 }, { LR433_IN_FSK, 40000, 0xFFFFFF0, 28, 72 },
        { LR433_IN_FSK, 68966, 0x0F0F, 16, 112 }, { LR433_IN_ERT, 32768, 0x2665, 14, 252 },
    };
    uint32_t x = 0x12345678u;
    int false_hits = 0;
    for (size_t s = 0; s < sizeof(S) / sizeof(S[0]); s++) {
        for (int frame = 0; frame < 200; frame++) {
            /* Runs of random length, as a receiver keyed by noise gives. */
            int bit = 0, k = 0;
            memset(s_frame, 0, sizeof(s_frame));
            while (k < S[s].bytes * 8) {
                x = x * 1664525u + 1013904223u;
                int run = 1 + (int)((x >> 24) % 9);
                for (; run > 0 && k < S[s].bytes * 8; run--, k++)
                    if (bit) s_frame[k / 8] |= (uint8_t)(0x80 >> (k % 8));
                bit ^= 1;
            }
            const lr433_capture_t c = { .kind = S[s].kind, .rate = S[s].rate, .lead = S[s].lead,
                                        .lead_bits = S[s].lead_bits, .data = s_frame, .bits = S[s].bytes * 8 };
            lr433_msg_t m[8];
            false_hits += lr433_decode(&c, &W, m, 8);
        }
    }
    LS_EQ_INT(false_hits, 0);
}

LS_CASE(the_check_values_are_the_catalogued_ones)
{
    static const uint8_t digits[] = "123456789";
    LS_EQ_UINT(lr433_crc8(digits, 9, 0x07, 0x00), 0xF4);    /* CRC-8/SMBUS  */
    LS_EQ_UINT(lr433_crc16(digits, 9, 0x1021, 0x0000), 0x31C3); /* XMODEM */
    LS_EQ_UINT(lr433_crc16(digits, 9, 0x1021, 0xFFFF), 0x29B1); /* CCITT-FALSE */
}

LS_CASE(pulses_start_at_the_first_high_with_the_lead_put_back)
{
    /* Lead 0b0000_0000_1111 (the OOK detector), then 1100 0011 1000 0000. */
    static const uint8_t data[2] = { 0xC3, 0x80 };
    const lr433_capture_t c = { .kind = LR433_IN_OOK, .rate = 40000, .lead = 0x00F, .lead_bits = 12,
                                .data = data, .bits = 16 };
    static lr433_pulses_t p;
    LS_EQ_INT(lr433_pulses(&c, false, &p), 2);
    LS_EQ_INT(p.pulse[0], 6);   /* four from the lead, two from the frame */
    LS_EQ_INT(p.gap[0], 4);
    LS_EQ_INT(p.pulse[1], 3);
    LS_EQ_INT(p.gap[1], 0xFFFF);  /* the capture ended in it: the end of the message */
    LS_EQ_INT(p.rate, 40000);

    /* Inverted, the first high is the lead's quiet. */
    LS_EQ_INT(lr433_pulses(&c, true, &p), 3);
    LS_EQ_INT(p.pulse[0], 8);
    LS_EQ_INT(p.gap[0], 6);
}

/* ------------------------------------------------------- the experiment -- */

const char *ls_exp_hw_radio_busy(void) { return NULL; }
bool ls_exp_hw_radio_take(void) { return true; }
void ls_exp_hw_radio_give(void) { }
bool ls_exp_hw_wake(void) { return false; }

static uint32_t s_caps = LS_LORA_CAP_LORA | LS_LORA_CAP_FSK | LS_LORA_CAP_RSSI_INST | LS_LORA_CAP_MODES_RX;
static bool s_fsk, s_ook;
static ls_fsk_cfg_t s_fcfg;
static ls_ook_cfg_t s_ocfg;
static int s_fsk_begins, s_ook_begins, s_fsk_ends, s_ook_ends;
static uint32_t s_refuse_rate;     /* an FSK session at this rate is refused */
/* The next frame either session hands over, once. */
static uint8_t s_next[256];
static int s_next_len;
static float s_next_rssi;

uint32_t ls_lora_caps(void) { return s_caps; }
uint32_t ls_lora_fsk_bw_snap(uint32_t hz) { return hz; }
esp_err_t ls_lora_fsk_begin(const ls_fsk_cfg_t *cfg)
{
    if (cfg->bitrate == s_refuse_rate) return ESP_ERR_INVALID_ARG;
    s_fsk_begins++;
    s_fcfg = *cfg;
    s_fsk = true;
    return ESP_OK;
}
esp_err_t ls_lora_fsk_end(void) { s_fsk_ends++; s_fsk = false; return ESP_OK; }
esp_err_t ls_lora_fsk_receive(void) { return ESP_OK; }
int ls_lora_fsk_poll(uint8_t *buf, size_t size, float *rssi)
{
    if (!s_fsk || !s_next_len || (size_t)s_next_len > size) return 0;
    memcpy(buf, s_next, (size_t)s_next_len);
    *rssi = s_next_rssi;
    const int n = s_next_len;
    s_next_len = 0;
    return n;
}
esp_err_t ls_lora_ook_begin(const ls_ook_cfg_t *cfg)
{
    s_ook_begins++;
    s_ocfg = *cfg;
    s_ook = true;
    return ESP_OK;
}
int ls_lora_ook_poll(uint8_t *buf, size_t size, float *rssi)
{
    if (!s_ook || !s_next_len || (size_t)s_next_len > size) return 0;
    memcpy(buf, s_next, (size_t)s_next_len);
    if (rssi) *rssi = s_next_rssi;
    const int n = s_next_len;
    s_next_len = 0;
    return n;
}
esp_err_t ls_lora_ook_end(void) { s_ook_ends++; s_ook = false; return ESP_OK; }
bool ls_lora_ook_active(void) { return s_ook; }
esp_err_t ls_lora_rssi_inst(float *dbm) { *dbm = -110.0f; return ESP_OK; }

static int run_console(const char *line)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", line);
    char *argv[8];
    int argc = 0;
    for (char *t = strtok(buf, " "); t && argc < 8; t = strtok(NULL, " ")) argv[argc++] = t;
    return ls_exp_console(argc, argv);
}

static const ls_experiment_t *lr433(void)
{
    ls_exp_forget();
    ls_exp_register_builtin();
    const ls_experiment_t *e = ls_exp_find("lr433");
    LS_CHECK(e != NULL);
    return e;
}

/* Empty the table and counters; units back to PSI / F. */
static void clean(const ls_experiment_t *e)
{
    for (int i = 0; i < e->n_opts; i++) {
        if (!strcmp(e->opts[i].label, "CLEAR")) e->opts[i].act(&e->opts[i]);
        if (!strcmp(e->opts[i].label, "UNITS")) e->opts[i].set(&e->opts[i], 0);
    }
    s_fsk = s_ook = false;
    s_fsk_begins = s_ook_begins = s_fsk_ends = s_ook_ends = 0;
    s_next_len = 0;
    s_refuse_rate = 0;
    s_caps = LS_LORA_CAP_LORA | LS_LORA_CAP_FSK | LS_LORA_CAP_RSSI_INST | LS_LORA_CAP_MODES_RX;
    ls_shim_time_set(1000000);
}

static const lr433_vec_t *vec(const char *source)
{
    for (int i = 0; i < N_VECS; i++) if (!strcmp(VECS[i].source, source)) return &VECS[i];
    LS_CHECK_MSG(0, "no vector %s", source);
    return &VECS[0];
}

static void arrive(const lr433_vec_t *v, float rssi)
{
    memset(s_next, 0, sizeof(s_next));
    memcpy(s_next, v->data, (size_t)v->len);
    s_next_len = v->bits / 8;
    s_next_rssi = rssi;
}

static int read_lines(const ls_experiment_t *e, char (*out)[LS_EXP_LINE], int max)
{
    return ls_exp_read_lines(e, out, max);
}

static bool has_line(char (*l)[LS_EXP_LINE], int n, const char *a, const char *b)
{
    for (int i = 0; i < n; i++)
        if (strstr(l[i], a) && (!b || strstr(l[i], b))) return true;
    return false;
}

/* Print what the page would show, and hold every line to the page's 48
   columns. */
static void dump(char (*l)[LS_EXP_LINE], int n)
{
    for (int i = 0; i < n; i++) {
        ls_note("  |%s|", l[i]);
        LS_CHECK_MSG(strlen(l[i]) <= 48, "wider than the page: [%s]", l[i]);
    }
}

LS_CASE(lr433_is_built_in_as_a_trying_experiment_with_its_options)
{
    const ls_experiment_t *e = lr433();
    LS_EQ_STR(e->name, "LR433");
    LS_EQ_INT(e->maturity, LS_EXP_TRYING);
    LS_CHECK(e->configure != NULL);
    LS_EQ_INT(e->n_opts, 5);
    LS_CHECK(strlen(e->sub) <= 41);           /* what the list shows of it */
}

LS_CASE(the_315_plan_samples_ook_then_two_fsk_sessions_in_turn)
{
    const ls_experiment_t *e = lr433();
    clean(e);
    LS_EQ_INT(run_console("exp lr433 start 315 5"), 0);
    LS_CHECK(ls_exp_running() == e);

    /* First the OOK sampler: 40k decisions, 1 quiet then 3 high, 252 bytes. */
    LS_EQ_INT(s_ook_begins, 1);
    LS_EQ_UINT(s_ocfg.freq_hz, 315000000u);
    LS_EQ_UINT(s_ocfg.bitrate, 40000);
    LS_EQ_UINT(s_ocfg.pattern, 0x000E);
    LS_EQ_INT(s_ocfg.pattern_bits, 4);
    LS_EQ_INT(s_ocfg.frame_bytes, 252);
    LS_EQ_INT(s_ocfg.gain_step, 13);          /* the top step, as Mode S runs */
    LS_EQ_INT(s_fsk_begins, 0);

    /* The dwell runs out: the 19.2k TPMS session, four decisions a chip. */
    ls_shim_time_advance(5 * 1000000LL);
    ls_exp_service();
    LS_EQ_INT(s_ook_ends, 1);
    LS_EQ_INT(s_fsk_begins, 1);
    LS_EQ_UINT(s_fcfg.freq_hz, 315000000u);
    LS_EQ_UINT(s_fcfg.bitrate, 76800);
    LS_EQ_UINT(s_fcfg.sync_word, 0x0F0F0000u);
    LS_EQ_INT(s_fcfg.sync_bits, 16);
    LS_EQ_INT(s_fcfg.payload_bytes, 128);
    LS_CHECK(s_fcfg.bitrate + 2 * s_fcfg.deviation_hz <= s_fcfg.bandwidth_hz);

    /* Then PMV-107J at 40k, then round to the OOK sampler again. */
    ls_shim_time_advance(5 * 1000000LL);
    ls_exp_service();
    LS_EQ_INT(s_fsk_begins, 2);
    LS_EQ_UINT(s_fcfg.bitrate, 40000);
    LS_EQ_UINT(s_fcfg.sync_word, 0xFFF00000u);
    LS_CHECK(s_fcfg.bitrate + 2 * s_fcfg.deviation_hz <= s_fcfg.bandwidth_hz);
    ls_shim_time_advance(5 * 1000000LL);
    ls_exp_service();
    LS_EQ_INT(s_ook_begins, 2);
    LS_EQ_INT(s_fsk_ends, 2);

    LS_EQ_INT(run_console("exp lr433 stop"), 0);
    LS_CHECK(!s_ook && !s_fsk);
}

LS_CASE(a_tyre_heard_goes_in_the_table_with_its_reading)
{
    const ls_experiment_t *e = lr433();
    clean(e);
    LS_EQ_INT(run_console("exp lr433 start 315 30"), 0);
    arrive(vec("Schrader_EG53MA4_TPMS/01/gfile001"), -61.0f);
    ls_exp_service();

    char l[24][LS_EXP_LINE];
    int n = read_lines(e, l, 24);
    dump(l, n);
    LS_CHECK(has_line(l, n, "NOW  315 OOK", "315.000 MHz"));
    LS_CHECK(has_line(l, n, "PLAN [315o] 315f 315p", NULL));
    LS_CHECK(has_line(l, n, "HEARD 1 bursts, 1 decoded, 1 device", NULL));
    LS_CHECK(has_line(l, n, "Schr-EG53", "2FC871"));
    LS_CHECK(has_line(l, n, "0.0psi 90F", " -61 "));
    for (int i = 0; i < n; i++) LS_CHECK_MSG(strlen(l[i]) <= 48, "line %d is wider than the page", i);

    /* Heard again: the count, not a second row. */
    ls_shim_time_advance(2 * 1000000LL);
    arrive(vec("Schrader_EG53MA4_TPMS/01/gfile001"), -58.0f);
    ls_exp_service();
    n = read_lines(e, l, 24);
    LS_CHECK(has_line(l, n, "2FC871", " -58   2  0s"));
    LS_CHECK(has_line(l, n, "1 device", NULL));

    /* Metric. */
    for (int i = 0; i < e->n_opts; i++)
        if (!strcmp(e->opts[i].label, "UNITS")) e->opts[i].set(&e->opts[i], 1);
    n = read_lines(e, l, 24);
    LS_CHECK(has_line(l, n, "2FC871", "0kPa 32C"));
    LS_EQ_INT(run_console("exp lr433 stop"), 0);
}

LS_CASE(the_newest_device_is_first_and_noise_counts_against_its_session)
{
    const ls_experiment_t *e = lr433();
    clean(e);
    LS_EQ_INT(run_console("exp lr433 start 315 3"), 0);
    arrive(vec("Schrader/01/gfile007"), -70.0f);
    ls_exp_service();

    /* Something keyed the OOK sampler and was nothing it knows. */
    ls_shim_time_advance(1000000);
    memset(s_next, 0, sizeof(s_next));
    s_next[0] = 0xF0;
    s_next_len = 252;
    s_next_rssi = -88.0f;
    ls_exp_service();

    /* The 19.2k session hears a Ford. */
    ls_shim_time_advance(3 * 1000000LL);
    ls_exp_service();
    LS_CHECK(s_fsk);
    arrive(vec("Ford_TPMS/gfile124"), -66.0f);
    ls_exp_service();

    char l[24][LS_EXP_LINE];
    const int n = read_lines(e, l, 24);
    dump(l, n);
    int ford = -1, schrader = -1;
    for (int i = 0; i < n; i++) {
        if (strstr(l[i], "45d30b69")) ford = i;
        if (strstr(l[i], "03A38B2")) schrader = i;
    }
    LS_CHECK(ford >= 0 && schrader >= 0 && ford < schrader);
    LS_CHECK(has_line(l, n, "Ford", "34.5psi"));
    LS_CHECK(has_line(l, n, "Schrader", "58.0psi 73F"));
    LS_CHECK(has_line(l, n, "KEYED, NOT DECODED", NULL));
    LS_CHECK(has_line(l, n, "315 OOK      1  last  -88 max  -88 dBm", NULL));
    LS_CHECK(has_line(l, n, "HEARD 3 bursts, 2 decoded, 2 devices", NULL));

    /* CLEAR empties it. */
    clean(e);
    const int m = read_lines(e, l, 24);
    LS_CHECK(!has_line(l, m, "45d30b69", NULL));
    LS_CHECK(!has_line(l, m, "KEYED", NULL));
    LS_EQ_INT(run_console("exp lr433 stop"), 0);
}

LS_CASE(a_meter_and_sensors_on_their_own_bands)
{
    const ls_experiment_t *e = lr433();
    clean(e);
    LS_EQ_INT(run_console("exp lr433 start ert"), 0);
    LS_EQ_UINT(s_ocfg.freq_hz, 912600000u);
    LS_EQ_UINT(s_ocfg.bitrate, 32768);
    LS_EQ_UINT(s_ocfg.pattern, 0x2999);
    LS_EQ_INT(s_ocfg.pattern_bits, 14);
    arrive(vec("idm/IDM/g002_912.6M_2359.3k"), -55.0f);
    ls_exp_service();
    /* One session: no dwell, it stays. */
    ls_shim_time_advance(120 * 1000000LL);
    ls_exp_service();
    LS_EQ_INT(s_ook_begins, 1);
    char l[24][LS_EXP_LINE];
    int n = read_lines(e, l, 24);
    dump(l, n);
    LS_CHECK(has_line(l, n, "ERT-IDM", "11278109"));
    LS_CHECK(has_line(l, n, "339972", NULL));
    LS_CHECK(has_line(l, n, "PLAN [ert]", NULL));

    LS_EQ_INT(run_console("exp lr433 start 433"), 0);
    LS_EQ_UINT(s_ocfg.freq_hz, 433920000u);
    LS_EQ_UINT(s_ocfg.bitrate, 16000);
    arrive(vec("acurite/Acurite_592TXR/acurite-592txr-003"), -72.0f);
    ls_exp_service();
    n = read_lines(e, l, 24);
    dump(l, n);
    LS_CHECK(has_line(l, n, "Acu-Tower", "12053 C"));
    LS_CHECK(has_line(l, n, "80.1F 74%", NULL));
    /* The meter heard before is still there, below the newer sensor. */
    LS_CHECK(has_line(l, n, "11278109", NULL));

    LS_EQ_INT(run_console("exp lr433 start 915"), 0);
    LS_EQ_INT(s_fcfg.payload_bytes, 112);
    arrive(vec("ecowitt/WH31B/g006_915M_250k"), -80.0f);
    ls_exp_service();
    n = read_lines(e, l, 24);
    LS_CHECK(has_line(l, n, "WH31E", "92 3"));
    LS_EQ_INT(run_console("exp lr433 stop"), 0);
}

LS_CASE(the_console_takes_a_band_and_a_dwell_and_says_what_it_wants)
{
    const ls_experiment_t *e = lr433();
    clean(e);
    LS_CHECK(run_console("exp lr433 start 868") != 0);
    LS_CHECK(run_console("exp lr433 start 315 1") != 0);
    LS_CHECK(run_console("exp lr433 start 315 601") != 0);
    LS_CHECK(run_console("exp lr433 start 315 10 x") != 0);
    LS_CHECK(ls_exp_running() != e);
    LS_EQ_INT(run_console("exp lr433 start all 2"), 0);
    char l[24][LS_EXP_LINE];
    const int n = read_lines(e, l, 24);
    LS_CHECK(has_line(l, n, "PLAN [315o] 315f 315p 433o 433f 915f ert", NULL));
    LS_CHECK(has_line(l, n, "TPMS send about once a minute", NULL));
    LS_EQ_INT(run_console("exp lr433 stop"), 0);
}

LS_CASE(a_chip_without_the_ook_session_runs_the_fsk_entries_only)
{
    const ls_experiment_t *e = lr433();
    clean(e);
    s_caps = LS_LORA_CAP_LORA | LS_LORA_CAP_FSK | LS_LORA_CAP_RSSI_INST;
    LS_EQ_INT(run_console("exp lr433 start 315 5"), 0);
    LS_EQ_INT(s_ook_begins, 0);
    LS_EQ_INT(s_fsk_begins, 1);
    LS_EQ_INT(run_console("exp lr433 stop"), 0);

    /* Nothing that can hear meters: it says so and does not start. */
    LS_CHECK(run_console("exp lr433 start ert") != 0);
    char st[64];
    ls_exp_state_line(e, st, sizeof(st));
    LS_CHECK_MSG(strstr(st, "ERT") != NULL, "got [%s]", st);
}

LS_CASE(a_session_the_chip_refuses_is_skipped_and_marked_in_the_plan)
{
    const ls_experiment_t *e = lr433();
    clean(e);
    s_refuse_rate = 40000;                 /* the PMV-107J session */
    LS_EQ_INT(run_console("exp lr433 start 315 2"), 0);
    ls_shim_time_advance(2 * 1000000LL);
    ls_exp_service();                      /* 315 FSK */
    ls_shim_time_advance(2 * 1000000LL);
    ls_exp_service();                      /* PMV refused: round to 315 OOK */
    LS_EQ_INT(s_ook_begins, 2);
    LS_EQ_INT(s_fsk_begins, 1);
    char l[24][LS_EXP_LINE];
    const int n = read_lines(e, l, 24);
    dump(l, n);
    LS_CHECK(has_line(l, n, "PLAN [315o] 315f 315p!", NULL));
    LS_EQ_INT(run_console("exp lr433 stop"), 0);
}
