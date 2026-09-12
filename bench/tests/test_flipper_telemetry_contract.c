/* LS_TEST_SOURCES: ${FW}/main/flipper_link_telemetry.c */
/* LS_TEST_INCLUDE: ${FW}/main ${FW}/components/lakeshark/apps/rec ${FW}/components/lakeshark */
/* Flipper protocol key contract
   Source of truth: LakeShark-Flipper/ls_link.c -> apply_kv()
*/

#include "ls_test.h"
#include "flipper_link_telemetry.h"
#include "lakeshark_backend.h"
#include "rec_state.h"
#include "ls_board.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* test-time host shim: avoid pulling full lakeshark backend just to keep ADS-B
 * round-trip coverage independent from aircraft decoding state. */
bool lakeshark_adsb_aircraft_at(int index, lakeshark_adsb_ac_t *out)
{
    (void)index;
    (void)out;
    return false;
}

static const char *kv(const char *tok, const char *key)
{
    size_t n = strlen(key);
    if (strncmp(tok, key, n) != 0) return NULL;
    if (tok[n] != '=') return NULL;
    return tok + n + 1;
}

static bool parse_key_value(const char *line, const char *key, char *out, size_t out_len)
{
    char frame[768];
    if (!line || !key || !out || out_len == 0) return false;
    size_t n = strnlen(line, sizeof(frame) - 1);
    memcpy(frame, line, n);
    frame[n] = '\0';

    char *p = frame;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *tok = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';

        const char *v = kv(tok, key);
        if (!v) continue;
        size_t m = strnlen(v, out_len - 1);
        memcpy(out, v, m);
        out[m] = '\0';
        return true;
    }
    return false;
}

static bool parse_u32_value(const char *line, const char *key, uint32_t *out)
{
    char v[64];
    if (!parse_key_value(line, key, v, sizeof(v))) return false;
    if (v[0] == '\0') return false;
    char *end = NULL;
    unsigned long u = strtoul(v, &end, 10);
    if (*end) return false;
    *out = (uint32_t)u;
    return true;
}

static bool parse_s32_value(const char *line, const char *key, int *out)
{
    char v[64];
    if (!parse_key_value(line, key, v, sizeof(v))) return false;
    if (v[0] == '\0') return false;
    char *end = NULL;
    long s = strtol(v, &end, 10);
    if (*end) return false;
    *out = (int)s;
    return true;
}

static bool key_set(const char *line, const char *key, const char *mode, const char *label)
{
    char value[64];
    if (parse_key_value(line, key, value, sizeof(value))) return true;
    (void)value;
    (void)mode;
    (void)label;
    return false;
}

static void assert_key_set(const char *line, const char *key, const char *mode,
                          const char *kind)
{
    char value[64];
    LS_CHECK_MSG(parse_key_value(line, key, value, sizeof(value)),
                 "mode=%s missing %s key %s", mode, kind, key);
}

static void assert_frame_roundtrip_common(const char *line, uint32_t expected_f,
                                        int expected_g, int expected_v,
                                        int expected_mu, int expected_rtl,
                                        const char *expected_md,
                                        const char *expected_bd)
{
    uint32_t v_f = 0;
    int v_g = 0, v_v = 0, v_mu = 0, v_rtl = 0;
    char v_md[32];

    LS_CHECK_MSG(parse_u32_value(line, "f", &v_f), "missing f");
    LS_CHECK_MSG(parse_s32_value(line, "g", &v_g), "missing g");
    LS_CHECK_MSG(parse_s32_value(line, "v", &v_v), "missing v");
    LS_CHECK_MSG(parse_s32_value(line, "mu", &v_mu), "missing mu");
    LS_CHECK_MSG(parse_s32_value(line, "rtl", &v_rtl), "missing rtl");
    LS_CHECK_MSG(parse_key_value(line, "md", v_md, sizeof(v_md)),
                 "missing md");

    LS_EQ_UINT(v_f, expected_f);
    LS_EQ_INT(v_g, expected_g);
    LS_EQ_INT(v_v, expected_v);
    LS_EQ_INT(v_mu, expected_mu);
    LS_EQ_INT(v_rtl, expected_rtl);
    LS_CHECK_MSG(strcmp(v_md, expected_md) == 0,
                "md expected '%s' got '%s'", expected_md, v_md);
    LS_CHECK_MSG(key_set(line, "bd", expected_bd, "board"),
                "board key missing from %s frame", expected_md);
    char bd[64];
    LS_CHECK_MSG(parse_key_value(line, "bd", bd, sizeof(bd)),
                 "board key missing");
    LS_EQ_STR(bd, expected_bd);
}

static void assert_keys(const char *line, const char *mode,
                       const char *const *keys,
                       size_t key_count,
                       const char *kind)
{
    for (size_t i = 0; i < key_count; i++) {
        assert_key_set(line, keys[i], mode, kind);
    }
}

/* Keys every mode reports on this firmware (common telemetry frame keys). */
static const char *k_common[] = {
    "md", "f", "g", "v", "mu", "rtl", "bps", "up", "fi", "fd", "stl", "rhs", "bd"
};

/* Keys that only the matching mode-specific formatter emits. */
static const char *k_fm[] = {
    "ef", "eg", "efk", "egk", "tst", "ter", "gst", "ger", "rxs", "rer",
    "fm", "sq", "so", "au", "ss", "se", "spk", "sdb", "sw", "pb", "pau", "psy", "pp", "pf", "pbd", "pty", "ptx"
};
static const char *k_adsb[] = {
    "ac", "mt", "mps", "cg", "ce", "bps1", "mga", "mgp", "lms", "aci", "acn"
};
static const char *k_p25[] = {
    "dm", "dmn", "agc", "nac", "tg", "src", "na", "ta", "sa", "sy", "vo", "sc", "vc", "bo",
    "bf", "iq", "pol", "bp", "vg", "rf", "rs", "ad", "dus", "ft", "e", "re"
};
static const char *k_rec[] = {
    "rph", "red", "rsp", "rmg", "rfl", "rth", "rtf", "rgp", "rcp",
    "rbw", "rmp", "rms", "rme", "ren", "rmn", "rmx", "rbd", "rlf"
};

LS_CASE(flipper_telemetry_contract_fm)
{
    lakeshark_fm_tel_t fm = {
        .submode = 3,
        .freq_hz = 433920000u,
        .gain_tenths = 455,
        .effective_freq_hz = 433919875u,
        .effective_gain_tenths = 450,
        .effective_freq_known = 1,
        .effective_gain_known = 1,
        .tune_state = 2,
        .gain_state = 3,
        .gain_error = -6,
        .receiver_streaming = 1,
        .iq_level = 12,
        .audio_level = 7,
        .squelch_tenths = 9,
        .squelch_open = 1,
        .scan_start_hz = 430000000u,
        .scan_stop_hz = 434000000u,
        .scan_peak_hz = 431000000u,
        .scan_peak_db = -1,
        .scan_sweeps = 33u,
        .pocsag_baud = 1200,
        .pocsag_auto = 1,
        .pocsag_sync = 5,
        .pocsag_pages = 4u,
        .pocsag_frames = 7u,
        .pocsag_last_addr = 1234u,
        .pocsag_last_baud = 1200,
        .pocsag_last_type = 'A',
    };
    strcpy(fm.pocsag_last_text, "a+b");
    fm.iq_bytes_sec = 11200u;
    fm.read_errors = 5;

    ls_telemetry_common_t common = {
        .volume = 71,
        .muted = 0,
        .rtl_ready = 1,
        .uptime_s = 1000u,
        .free_internal = 3000u,
        .free_dma = 2000u,
        .sdr_stall_s = 0,
        .rhs_state = "OK",
    };

    /* Firmware callers use REPLY_MAX=384; keep this contract honest. */
    char frame[384];
    int n = ls_telemetry_build_fm(frame, sizeof(frame), &fm, &common);
    LS_CHECK_MSG(n > 0, "fm frame builder failed: %d", n);

    assert_frame_roundtrip_common(frame, fm.freq_hz, fm.gain_tenths,
                                 common.volume, common.muted, common.rtl_ready,
                                 "FM", LS_BOARD_NAME);
    assert_keys(frame, "FM", k_common, sizeof(k_common) / sizeof(k_common[0]), "common");
    assert_keys(frame, "FM", k_fm, sizeof(k_fm) / sizeof(k_fm[0]), "fm-specific");
    for (size_t i = 0; i < sizeof(k_adsb) / sizeof(k_adsb[0]); i++) {
        LS_CHECK_MSG(!parse_key_value(frame, k_adsb[i], (char[64]){0}, 64),
                     "ADS-B key leaked into FM frame");
    }
}

LS_CASE(fm_telemetry_preserves_mode_ids_and_does_not_alias_removed_value)
{
    static const char *const modes[] = {
        "listen", "scan", "pocsag", "wfm", "acars", "flex", "unknown"
    };
    lakeshark_fm_tel_t fm = {0};
    ls_telemetry_common_t common = {.rhs_state = "OK"};
    char frame[384], value[32];
    for (unsigned mode = 0; mode < sizeof(modes) / sizeof(modes[0]); ++mode) {
        fm.submode = (int)mode;
        LS_CHECK(ls_telemetry_build_fm(frame, sizeof(frame), &fm, &common) > 0);
        LS_CHECK(parse_key_value(frame, "fm", value, sizeof(value)));
        LS_EQ_STR(modes[mode], value);
    }
    fm.submode = -1;
    LS_CHECK(ls_telemetry_build_fm(frame, sizeof(frame), &fm, &common) > 0);
    LS_CHECK(parse_key_value(frame, "fm", value, sizeof(value)));
    LS_EQ_STR("unknown", value);
}

LS_CASE(flipper_telemetry_contract_adsb)
{
    lakeshark_adsb_tel_t s = {
        .freq_hz = 1090000000u,
        .gain_tenths = 420,
        .tracked = 3,
        .msgs_total = 11,
        .msgs_sec = 2,
        .crc_good = 10,
        .crc_err = 11,
        .bursts_sec = 8,
        .mag_avg = 11,
        .mag_peak = 20,
        .last_msg_ms = 123u,
        .n_aircraft = 0,
        .rtl_ready = 1,
        .iq_bytes_sec = 2000000u,
    };

    ls_telemetry_common_t common = {
        .volume = 42,
        .muted = 1,
        .rtl_ready = 1,
        .uptime_s = 2000u,
        .free_internal = 3010u,
        .free_dma = 2010u,
        .sdr_stall_s = 2,
        .rhs_state = "OK",
    };

    char frame[768];
    int n = ls_telemetry_build_adsb(frame, sizeof(frame), &s, &common);
    LS_CHECK_MSG(n > 0, "adsb frame builder failed: %d", n);

    assert_frame_roundtrip_common(frame, s.freq_hz, s.gain_tenths,
                                 common.volume, common.muted, common.rtl_ready,
                                 "ADSB", LS_BOARD_NAME);
    assert_keys(frame, "ADSB", k_common, sizeof(k_common) / sizeof(k_common[0]), "common");
    assert_keys(frame, "ADSB", k_adsb, sizeof(k_adsb) / sizeof(k_adsb[0]), "adsb-specific");
}

LS_CASE(flipper_telemetry_contract_p25)
{
    lakeshark_p25_tel_t p = {
        .freq_hz = 851000000u,
        .demod_mode = 7,
        .gain_tenths = 515,
        .agc_on = 1,
        .nac = 659,
        .tg = 1201,
        .src = 1234,
        .nac_age_ms = 120,
        .tg_age_ms = 250,
        .src_age_ms = 250,
        .has_sync = 1,
        .voice_active = 1,
        .sync_count = 12,
        .voice_count = 340,
        .bch_ok = 142,
        .bch_fail = 3,
        .iq_level = 372,
        .polarity_inverted = 1,
        .beep = 1,
        .voice_gate = 25,
        .ring_fill = 4096,
        .ring_size = 8192,
        .read_errors = 0,
        .iq_bytes_sec = 491520u,
        .audio_drops = 2u,
        .decode_us = 8100,
    };
    memset(p.ftype, 0, sizeof(p.ftype));
    memset(p.err, 0, sizeof(p.err));
    strcpy(p.ftype, "cqpsk");
    strcpy(p.err, "no_sync");

    ls_telemetry_common_t common = {
        .volume = 84,
        .muted = 0,
        .rtl_ready = 1,
        .uptime_s = 3000u,
        .free_internal = 3010u,
        .free_dma = 2010u,
        .sdr_stall_s = 1,
        .rhs_state = "OK",
    };

    char frame[768];
    int n = ls_telemetry_build_p25(frame, sizeof(frame), &p, "P25", "CQPSK", &common);
    LS_CHECK_MSG(n > 0, "p25 frame builder failed: %d", n);

    assert_frame_roundtrip_common(frame, p.freq_hz, p.gain_tenths,
                                 common.volume, common.muted, common.rtl_ready,
                                 "P25", LS_BOARD_NAME);
    assert_keys(frame, "P25", k_common, sizeof(k_common) / sizeof(k_common[0]), "common");
    assert_keys(frame, "P25", k_p25, sizeof(k_p25) / sizeof(k_p25[0]), "p25-specific");
}

LS_CASE(flipper_telemetry_contract_rec)
{
    rec_status_t s = {
        .freq_hz = 433920000u,
        .gain_tenths = 500,
        .edges = 6,
        .span_us = 120000u,
        .mag_now = -3,
        .mag_floor = -24,
        .mag_thresh = 14,
        .thresh_fixed = 25,
        .gap_ms = 10,
        .captures = 5u,
        .bytes_sec = 5000u,
        .bw_hz = 200000u,
        .min_pulse_us = 40u,
        .max_span_us = 3000000u,
        .min_edges = 6,
        .end_reason = 0,
        .min_mark_us = 50u,
        .max_mark_us = 70u,
        .baud_est = 1200u,
    };
    strcpy(s.last_file, "last.cap");

    ls_telemetry_common_t common = {
        .volume = 53,
        .muted = 1,
        .rtl_ready = 1,
        .uptime_s = 4000u,
        .free_internal = 4010u,
        .free_dma = 3010u,
        .sdr_stall_s = 0,
        .rhs_state = "OK",
    };

    char frame[768];
    int n = ls_telemetry_build_rec(frame, sizeof(frame), &s, &common);
    LS_CHECK_MSG(n > 0, "rec frame builder failed: %d", n);

    assert_frame_roundtrip_common(frame, s.freq_hz, s.gain_tenths,
                                 common.volume, common.muted, common.rtl_ready,
                                 "REC", LS_BOARD_NAME);
    assert_keys(frame, "REC", k_common, sizeof(k_common) / sizeof(k_common[0]), "common");
    assert_keys(frame, "REC", k_rec, sizeof(k_rec) / sizeof(k_rec[0]), "rec-specific");
}
