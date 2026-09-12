/* LS_TEST_SOURCES: ${APP}/p25/dsd_frame.c ${APP}/p25/p25_tsbk.c */
/* P25 frame dispatch and raw TSDU payload retention. */

#include "ls_test.h"
#include "dsd.h"
#include "p25p1_check_nid.c"

#define INPUT_CAPACITY (33 + P25_TSDU_DIBIT_COUNT)

static int s_input[INPUT_CAPACITY];
static int s_input_count;
static int s_input_pos;
static int s_skip_count;
static int s_hdu_calls;
static int s_ldu1_calls;
static int s_ldu2_calls;
static int s_tdu_calls;
static int s_tdulc_calls;
static int s_mbe_init_calls;
static int s_publish_hdu_ess;

int autoscan_bch_ok_flag;
int dsd_bch_fail_counter;

int getDibit(dsd_opts *opts, dsd_state *state)
{
    (void)opts;
    (void)state;
    LS_CHECK_MSG(s_input_pos < s_input_count,
                 "dispatcher read dibit %d past %d", s_input_pos, s_input_count);
    return s_input[s_input_pos++];
}

void skipDibit(dsd_opts *opts, dsd_state *state, int count)
{
    (void)opts;
    (void)state;
    s_skip_count += count;
    s_input_pos += count;
}

void processHDU(dsd_opts *opts, dsd_state *state)
{
    (void)opts;
    (void)state;
    s_hdu_calls++;
    if (s_publish_hdu_ess) {
        state->lasttg = 42;
        state->p25_ess_valid = 1;
        state->p25_algid = 0x80;
    }
}

void processLDU1(dsd_opts *opts, dsd_state *state)
{
    (void)opts;
    (void)state;
    s_ldu1_calls++;
}

void processLDU2(dsd_opts *opts, dsd_state *state)
{
    (void)opts;
    (void)state;
    s_ldu2_calls++;
}

void processTDU(dsd_opts *opts, dsd_state *state)
{
    (void)opts;
    (void)state;
    s_tdu_calls++;
}

void processTDULC(dsd_opts *opts, dsd_state *state)
{
    (void)opts;
    (void)state;
    s_tdulc_calls++;
}

void mbe_initMbeParms(mbe_parms *cur_mp, mbe_parms *prev_mp,
                      mbe_parms *prev_mp_enhanced)
{
    (void)cur_mp;
    (void)prev_mp;
    (void)prev_mp_enhanced;
    s_mbe_init_calls++;
}

void audio_beep_request(int kind)
{
    (void)kind;
}

static void make_codeword(int nac12, int duid, char *cw63)
{
    int data[BCH_KK];
    int par[BCH_RR];

    for (int i = 0; i < 12; i++) data[i] = (nac12 >> (11 - i)) & 1;
    for (int i = 0; i < 4; i++) data[12 + i] = (duid >> (3 - i)) & 1;
    bch_encode(data, par);

    for (int i = 0; i < BCH_KK; i++) cw63[i] = (char)data[i];
    for (int i = 0; i < BCH_RR; i++) cw63[BCH_KK + i] = (char)par[i];
}

static void load_frame(int duid, int with_tsdu_payload)
{
    char cw[63];
    make_codeword(0x293, duid, cw);

    s_input_count = 0;
    s_input_pos = 0;
    s_skip_count = 0;

    for (int i = 0; i < 11; i++)
        s_input[s_input_count++] = (cw[i * 2] << 1) | cw[i * 2 + 1];
    s_input[s_input_count++] = 0; /* status dibit */
    for (int i = 11; i < 31; i++)
        s_input[s_input_count++] = (cw[i * 2] << 1) | cw[i * 2 + 1];
    s_input[s_input_count++] = cw[62] << 1; /* parity is currently ignored */

    if (with_tsdu_payload) {
        for (int i = 0; i < P25_TSDU_DIBIT_COUNT; i++)
            s_input[s_input_count++] = (i * 3 + 1) & 3;
    }
}

static void reset_dispatch_counts(void)
{
    s_hdu_calls = 0;
    s_ldu1_calls = 0;
    s_ldu2_calls = 0;
    s_tdu_calls = 0;
    s_tdulc_calls = 0;
    s_mbe_init_calls = 0;
}

LS_CASE(hdu_new_talkgroup_keeps_its_fresh_validated_ess)
{
    dsd_opts opts = {0}; dsd_state state = {0};
    state.lasttg = 7;
    s_publish_hdu_ess = 1;
    load_frame(0x0, 0);
    processFrame(&opts, &state);
    s_publish_hdu_ess = 0;
    LS_EQ_INT(state.lasttg, 42);
    LS_EQ_INT(state.p25_ess_valid, 1);
    LS_CHECK(!p25_ldu_should_mute_encrypted(&state, &opts));
}

LS_CASE(tsdu_payload_is_retained_in_wire_order)
{
    dsd_opts opts = { 0 };
    dsd_state state = { 0 };
    load_frame(0x7, 1);

    processFrame(&opts, &state);

    LS_EQ_INT(state.p25_tsdu_dibit_count, P25_TSDU_DIBIT_COUNT);
    LS_EQ_INT(state.p25_tsdu_dibits[0], s_input[33]);
    LS_EQ_INT(state.p25_tsdu_dibits[P25_TSDU_DIBIT_COUNT - 1],
              s_input[33 + P25_TSDU_DIBIT_COUNT - 1]);
    LS_EQ_INT(s_input_pos, s_input_count);
    LS_EQ_INT(s_skip_count, 0);
}

LS_CASE(all_seven_duids_reach_the_same_dispatch_branches)
{
    const struct {
        int duid;
        const char *subtype;
        int initial_type;
        int final_type;
        int *call_count;
        int mbe_init_count;
    } cases[] = {
        { 0x0, " HDU          ", 0, 2, &s_hdu_calls,   1 },
        { 0x5, " LDU1         ", 0, 1, &s_ldu1_calls, 0 },
        { 0xA, " LDU2         ", 1, 2, &s_ldu2_calls, 0 },
        { 0x3, " TDU          ", 0, 0, &s_tdu_calls,  1 },
        { 0xF, " TDULC        ", 0, 0, &s_tdulc_calls, 1 },
        { 0x7, " TSDU         ", 0, 3, NULL,          0 },
        { 0xC, " PDU          ", 0, 4, NULL,          0 },
    };

    for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        dsd_opts opts = { 0 };
        dsd_state state = { 0 };
        state.lastp25type = cases[i].initial_type;
        reset_dispatch_counts();
        load_frame(cases[i].duid, cases[i].duid == 0x7);

        processFrame(&opts, &state);

        LS_EQ_STR(state.fsubtype, cases[i].subtype);
        LS_EQ_INT(state.lastp25type, cases[i].final_type);
        LS_EQ_INT(s_mbe_init_calls, cases[i].mbe_init_count);
        if (cases[i].call_count) LS_EQ_INT(*cases[i].call_count, 1);
        LS_EQ_INT(s_hdu_calls + s_ldu1_calls + s_ldu2_calls
                  + s_tdu_calls + s_tdulc_calls,
                  cases[i].call_count ? 1 : 0);
        LS_EQ_INT(s_skip_count, 0);
        LS_EQ_INT(s_input_pos, s_input_count);
    }
}

LS_CASE(unknown_duid_after_tsdu_uses_tsdu_fallback_capture)
{
    dsd_opts opts = { 0 };
    dsd_state state = { 0 };
    state.lastp25type = 3;
    load_frame(0x1, 1); /* unsupported DUID forces the last-type fallback branch */

    processFrame(&opts, &state);

    LS_EQ_STR(state.fsubtype, "(TSDU)        ");
    LS_EQ_INT(state.p25_tsdu_dibit_count, P25_TSDU_DIBIT_COUNT);
    LS_EQ_INT(state.p25_tsdu_dibits[0], s_input[33]);
    LS_EQ_INT(state.p25_tsdu_dibits[P25_TSDU_DIBIT_COUNT - 1],
              s_input[33 + P25_TSDU_DIBIT_COUNT - 1]);
    LS_EQ_INT(s_skip_count, 0);
    LS_EQ_INT(s_input_pos, s_input_count);
}
