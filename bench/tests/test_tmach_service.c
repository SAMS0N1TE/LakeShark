/* TIME MACHINE end to end on the bench: the worker (ls_tmach.c) fed through
   its taps, a page on the decoded bus, WHAT WAS THAT? writing a clip to a
   directory, the timeline linking the page to it, and the clip replayed
   through the real replay chains (ls_tmach_decode.c) decoding the page
   again. Also the settings file, and switching it off. */
#include "ls_test.h"
#include "ls_tmach.h"
#include "ls_tmach_decode.h"
#include "fm_state.h"
#include "pocsag_gen.h"
#include "p25_state.h"
#include "ls_radio_select.h"

#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#endif

fm_state_t FM;
p25_state_t P25;
ls_rsel_radio_t ls_rsel_sdr_held_by(const char *owner) { return owner && !strcmp(owner, "fm") ? LS_RSEL_SDR_RTL : LS_RSEL_NONE; }

/* A clean directory for clips, made once. */
static const char *dir(void)
{
    static char d[200];
    if (d[0]) return d;
    snprintf(d, sizeof(d), "%s/test_tmach", getenv("TEMP") ? getenv("TEMP") : ".");
#ifdef _WIN32
    _mkdir(d);
#endif
    for (int n = 1; n < 20; n++) {
        static const char *const EXT[] = { "cs8", "cs16", "json" };
        for (int e = 0; e < 3; e++) {
            char p[240];
            snprintf(p, sizeof(p), "%s/tm%04d.%s", d, n, EXT[e]);
            remove(p);
        }
    }
    return d;
}

LS_CASE(settings_round_trip_and_clamp)
{
    ls_tm_config_t c, back;
    ls_tm_config_defaults(&c);
    LS_CHECK(c.enabled);
    LS_EQ_UINT(c.keep_hz, 60000);
    c.source = LS_TM_SOURCE_LR; c.autoclip = true; c.clip_s = 20; c.budget_kb = 8192;
    char body[320];
    LS_CHECK(ls_tm_config_format(body, sizeof(body), &c) > 0);
    LS_CHECK(ls_tm_config_parse(body, &back));
    LS_CHECK(back.enabled == c.enabled && back.budget_kb == c.budget_kb && back.keep_hz == c.keep_hz &&
             back.source == c.source && back.autoclip == c.autoclip && back.clip_s == c.clip_s &&
             back.max_rate == c.max_rate);
    /* Out of range values are held to the range; another version is refused. */
    LS_CHECK(ls_tm_config_parse("{\"version\": 1, \"budget_kb\": 99999, \"keep_hz\": 5, \"clip_s\": 1}", &back));
    LS_EQ_UINT(back.budget_kb, 16384);
    LS_EQ_UINT(back.keep_hz, 25000);
    LS_EQ_UINT(back.clip_s, 5);
    LS_CHECK(!ls_tm_config_parse("{\"version\": 2, \"enabled\": false}", &back));
    LS_CHECK(back.enabled);            /* defaults, not the refused file */
    /* data/tmach/config.json is the defaults, exactly. */
    FILE *f = fopen(TMACH_DEFAULTS, "rb");
    LS_CHECK(f != NULL);
    char file[400] = "";
    if (f) { file[fread(file, 1, sizeof(file) - 1, f)] = 0; fclose(f); }
    for (char *s = file, *o = file;; s++) { if (*s != '\r') *o++ = *s; if (!*s) break; }   /* a CRLF checkout */
    ls_tm_config_defaults(&c);
    LS_CHECK(ls_tm_config_format(body, sizeof(body), &c) > 0);
    LS_EQ_STR(file, body);
}

/* 256k CU8 the way FM's POCSAG mode reads it: a page inside quiet carrier. */
static uint8_t *page_iq(size_t *bytes, size_t lead)
{
    uint8_t bits[BITS_GEN_MAX];
    const size_t nb = pocsag_build_bits(1234568, 3, "THE ONE THAT GOT AWAY", 576, bits, sizeof(bits));
    const double step = 1200.0 / FM_RTL_RATE;
    const size_t pairs = lead + (size_t)ceil((nb + 16) / step) + lead;
    uint8_t *iq = malloc(pairs * 2);
    ls_rng_t rng;
    ls_rng_seed(&rng, 99);
    double ph = 0;
    for (size_t k = 0; k < pairs; k++) {
        double hz = 1500, amp = 4;
        if (k >= lead) {
            const size_t at = (size_t)((k - lead) * step);
            if (at < nb) { hz += bits[at] ? 4500 : -4500; amp = 90; }
        }
        ph = remainder(ph + 6.283185307179586 * hz / FM_RTL_RATE, 6.283185307179586);
        const long i = lround(128 + amp * cos(ph) + 3 * ls_rng_noise(&rng));
        const long q = lround(128 + amp * sin(ph) + 3 * ls_rng_noise(&rng));
        iq[2 * k] = (uint8_t)(i < 0 ? 0 : i > 255 ? 255 : i);
        iq[2 * k + 1] = (uint8_t)(q < 0 ? 0 : q > 255 ? 255 : q);
    }
    *bytes = pairs * 2;
    return iq;
}

static ls_tm_status_t status(void)
{
    ls_tm_status_t st;
    memset(&st, 0, sizeof(st));
    LS_CHECK(ls_tm_status(&st));
    return st;
}

LS_CASE(what_was_that_saves_a_clip_the_timeline_links_and_replay_decodes)
{
    ls_tm_set_clip_dir(dir());
    ls_tm_init();
    ls_tmach_decode_register();
    FM.mode = FM_MODE_POCSAG;                 /* what the clip says was decoding */
    int64_t t = 10000000;
    ls_tm_service(t);
    const ls_tm_src_t src = { .center_hz = 152600000, .rate_hz = FM_RTL_RATE, .gain_tenths = 400,
                              .fmt = LS_TM_FMT_CU8, .radio = LS_TM_RADIO_RTL };
    size_t bytes;
    uint8_t *iq = page_iq(&bytes, 2 * FM_RTL_RATE);
    int64_t page_at = 0;
    for (size_t at = 0; at < bytes; at += 16384) {
        const size_t n = bytes - at < 16384 ? bytes - at : 16384;
        t += (int64_t)(n / 2) * 1000000 / FM_RTL_RATE;
        ls_tm_tap(LS_TM_Q_SDR, &src, iq + at, n, t);
        ls_tm_service(t);
        if (!page_at && at > bytes / 2) page_at = t;
    }
    ls_tm_status_t st = status();
    LS_CHECK(st.allocated && st.recording);
    LS_EQ_UINT(st.seg.rate_hz, 64000);
    LS_CHECK(st.held_s > 4.0);
    /* The live decoder's page reaches the timeline, its frequency found. */
    ls_decoded_t d = { .kind = LS_DECODED_PAGE, .address = 1234568, .at_us = page_at };
    snprintf(d.from, sizeof(d.from), "POCSAG 1200");
    snprintf(d.text, sizeof(d.text), "THE ONE THAT GOT AWAY");
    ls_decoded_publish(&d);
    ls_tm_service(t);
    ls_tm_event_t ev[4];
    uint32_t total = 0;
    LS_EQ_UINT(ls_tm_events(ev, 0, 4, &total), 1);
    LS_EQ_UINT(ev[0].freq_hz, 152600000);
    LS_EQ_UINT(ev[0].radio, LS_TM_RADIO_RTL);
    LS_EQ_UINT(ev[0].clip, 0);
    /* WHAT WAS THAT? */
    LS_CHECK(ls_tm_what_was_that());
    for (int k = 0; k < 500 && (ls_tm_service(t), status().saving || !status().last_clip); k++) {}
    st = status();
    LS_EQ_UINT(st.last_clip, 1);
    LS_CHECK(st.last_clip_s > 4.0);
    ls_tm_clip_meta_t m;
    LS_CHECK(ls_tm_clip_read_meta(dir(), "tm0001", &m));
    LS_EQ_UINT(m.decim, 4);
    LS_EQ_STR(m.decoder, "POCSAG");
    LS_EQ_UINT(m.center_hz, 152600000);
    LS_EQ_UINT(ls_tm_events(ev, 0, 4, &total), 1);
    LS_EQ_UINT(ev[0].clip, 1);
    /* Replayed through the real pager chain: the page again, from the clip. */
    LS_CHECK(ls_tm_replay(1));
    for (int k = 0; k < 2000 && (ls_tm_service(t), status().replaying || !status().replay_found); k++) {
        if (k > 5 && !status().replaying) break;
    }
    st = status();
    LS_CHECK_MSG(st.replay_found >= 1, "replay found %u: %s", st.replay_found, st.note);
    LS_CHECK(strstr(st.note, "THE ONE THAT") != NULL);
    ls_tm_service(t);
    /* The replayed page merges with the live one: same pager, same moment. */
    LS_EQ_UINT(ls_tm_events(ev, 0, 4, &total), 1);
    LS_CHECK(ev[0].count >= 2);
    free(iq);
}

LS_CASE(switched_off_it_lets_go_of_its_memory_and_ignores_the_taps)
{
    ls_tm_set_clip_dir(dir());
    ls_tm_init();
    ls_tm_config_t c;
    ls_tm_config_get(&c);
    c.enabled = false;
    ls_tm_config_set(&c);
    ls_tm_service(900000000);
    ls_tm_status_t st = status();
    LS_CHECK(!st.enabled);
    LS_CHECK(!st.allocated);
    LS_CHECK(!ls_tm_what_was_that());
    uint8_t buf[512] = { 0 };
    const ls_tm_src_t src = { .center_hz = 1, .rate_hz = 256000, .fmt = LS_TM_FMT_CU8, .radio = LS_TM_RADIO_RTL };
    ls_tm_tap(LS_TM_Q_SDR, &src, buf, sizeof(buf), 900000001);
    ls_tm_service(900000002);
    LS_CHECK(!status().allocated);
    c.enabled = true;
    ls_tm_config_set(&c);
}

LS_CASE(a_quiet_minute_gives_the_memory_back_and_the_next_tap_takes_it_again)
{
    ls_tm_set_clip_dir(dir());
    ls_tm_init();
    ls_tm_config_t c;
    ls_tm_config_get(&c);
    c.enabled = true;
    ls_tm_config_set(&c);
    int64_t t = 2000000000;
    ls_tm_service(t);
    uint8_t buf[4096] = { 0 };
    const ls_tm_src_t src = { .center_hz = 152600000, .rate_hz = 256000, .fmt = LS_TM_FMT_CU8, .radio = LS_TM_RADIO_RTL };
    ls_tm_tap(LS_TM_Q_SDR, &src, buf, sizeof(buf), t);
    ls_tm_service(t + 1000);
    LS_CHECK(status().allocated);
    ls_tm_tap(LS_TM_Q_SDR, &src, buf, sizeof(buf), t + 2000);
    ls_tm_service(t + 59000000);
    LS_CHECK(status().allocated);              /* 59 s since the last tap */
    ls_tm_service(t + 61000000);
    ls_tm_status_t st = status();
    LS_CHECK(!st.allocated);
    LS_EQ_UINT(st.ring_kb, 0);
    LS_CHECK(!strncmp(st.why, "Idle", 4));
    ls_tm_service(t + 62000000);
    LS_CHECK(!strncmp(status().why, "Idle", 4)); /* still says why */
    ls_tm_tap(LS_TM_Q_SDR, &src, buf, sizeof(buf), t + 63000000);
    ls_tm_service(t + 63001000);
    st = status();
    LS_CHECK(st.allocated);
    LS_EQ_UINT(st.ring_kb, c.budget_kb);
}
