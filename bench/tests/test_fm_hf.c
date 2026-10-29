/* HF settings, band plan and routing: which demod a band uses, when AM mode
   hands off to the HF demodulators, the clarifier and step rasters, the NVS
   word, and which signal path an RTL-SDR takes for a frequency. */
#include "ls_test.h"
#include "fm_hf.h"
#include "ls_radio_hf.h"

#include <string.h>

static uint32_t g_tuned;
static int g_changes;
static void on_changed(void) { ++g_changes; }
static void on_tune(uint32_t hz) { g_tuned = hz; }
static const fm_hf_hooks_t HOOKS = { on_changed, on_tune };

static void fresh(void)
{
    const fm_hf_settings_t d = { .demod = FM_HF_AM, .agc = 1, .pitch_hz = 700 };
    fm_hf_load(&d);
    fm_hf_install(&HOOKS);
    g_tuned = 0;
    g_changes = 0;
}

LS_CASE(am_mode_hands_hf_and_sideband_to_the_hf_chain_only)
{
    LS_CHECK(fm_hf_chain(true, 7200000, FM_HF_AM));         /* 41 m broadcast */
    LS_CHECK(fm_hf_chain(true, 29999999, FM_HF_AM));
    LS_CHECK(!fm_hf_chain(true, 127500000, FM_HF_AM));      /* airband stays */
    LS_CHECK(fm_hf_chain(true, 144300000, FM_HF_USB));      /* 2 m SSB */
    LS_CHECK(!fm_hf_chain(false, 7200000, FM_HF_LSB));      /* not in AM mode */
}

LS_CASE(bands_carry_the_mode_their_users_transmit)
{
    LS_EQ_INT(FM_HF_LSB, fm_hf_auto_demod(7150000));        /* 40 m */
    LS_EQ_INT(FM_HF_AM, fm_hf_auto_demod(7300000));         /* 41 m broadcast */
    LS_EQ_INT(FM_HF_USB, fm_hf_auto_demod(14230000));
    LS_EQ_INT(FM_HF_USB, fm_hf_auto_demod(5357000));        /* 60 m is USB */
    LS_EQ_INT(FM_HF_CW, fm_hf_auto_demod(10120000));
    LS_EQ_INT(FM_HF_AM, fm_hf_auto_demod(27185000));        /* CB */
    LS_EQ_INT(FM_HF_AM, fm_hf_auto_demod(12345));           /* nothing: AM */
    LS_EQ_INT(FM_HF_LSB, fm_hf_auto_sideband(3800000));
    LS_EQ_INT(FM_HF_USB, fm_hf_auto_sideband(5357000));
    LS_EQ_INT(FM_HF_USB, fm_hf_auto_sideband(21300000));
    /* Time stations win inside the broadcast and ham bands around them. */
    const int wwv = fm_hf_band_find(10000000);
    LS_CHECK(wwv >= 0);
    LS_EQ_STR("WWV 10", fm_hf_band(wwv)->name);
    LS_EQ_STR("CHU 7.85", fm_hf_band(fm_hf_band_find(7850000))->name);
    for (int i = 0; i < fm_hf_band_count(); ++i) {
        const fm_hf_band_t *b = fm_hf_band(i);
        LS_CHECK(b->dial_hz >= b->lo_hz && b->dial_hz <= b->hi_hz);
        LS_CHECK(b->dial_hz >= LS_HF_MIN_HZ && b->dial_hz < LS_HF_TOP_HZ);
        /* A band's own dial finds its own band, so its step is its own. */
        LS_EQ_STR(b->name, fm_hf_band(fm_hf_band_find(b->dial_hz))->name);
    }
}

LS_CASE(the_time_stations_and_cb_are_all_there)
{
    const uint32_t want[] = { 2500000, 5000000, 10000000, 15000000, 20000000,
                              3330000, 7850000, 14670000 };
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); ++i) {
        const int b = fm_hf_band_find(want[i]);
        LS_CHECK(b >= 0);
        LS_EQ_INT(FM_HF_KIND_TIME, fm_hf_band(b)->kind);
        LS_EQ_UINT(want[i], fm_hf_band(b)->dial_hz);
    }
    const int cb = fm_hf_band_find(26965000);
    LS_EQ_INT(FM_HF_KIND_CB, fm_hf_band(cb)->kind);
    LS_EQ_UINT(10000, fm_hf_band(cb)->step_hz);
}

LS_CASE(choosing_a_band_sets_its_demod_and_tunes_its_dial)
{
    fresh();
    int i = -1;
    for (int j = 0; j < fm_hf_band_count(); ++j)
        if (!strcmp(fm_hf_band(j)->name, "20m ham")) i = j;
    LS_CHECK(i >= 0);
    LS_CHECK(fm_hf_band_apply(i));
    LS_EQ_UINT(14230000, g_tuned);
    LS_EQ_INT(FM_HF_USB, fm_hf_get()->demod);
    LS_EQ_UINT(100, fm_hf_step_at(14230000));
    LS_CHECK(g_changes > 0);
    LS_CHECK(!fm_hf_band_apply(-1));
    LS_CHECK(!fm_hf_band_apply(fm_hf_band_count()));
    /* A chosen step overrides the band's. */
    fm_hf_set_step(10);
    LS_EQ_UINT(10, fm_hf_step_at(14230000));
    fm_hf_set_step(7);                                      /* not a raster */
    LS_EQ_UINT(10, fm_hf_get()->step_hz);
    fm_hf_tune(7300000, FM_HF_DEMOD_COUNT);                 /* the plan picks */
    LS_EQ_INT(FM_HF_AM, fm_hf_get()->demod);
    LS_EQ_UINT(7300000, g_tuned);
}

LS_CASE(the_clarifier_moves_in_tens_and_stops_at_its_reach)
{
    fresh();
    fm_hf_rit_step(1);
    LS_EQ_INT(10, fm_hf_get()->rit_hz);
    fm_hf_rit_step(-1); fm_hf_rit_step(-1);
    LS_EQ_INT(-10, fm_hf_get()->rit_hz);
    fm_hf_set_rit(124);
    LS_EQ_INT(120, fm_hf_get()->rit_hz);
    fm_hf_set_rit(-126);
    LS_EQ_INT(-130, fm_hf_get()->rit_hz);
    fm_hf_set_rit(99999);
    LS_EQ_INT(FM_HF_RIT_MAX_HZ, fm_hf_get()->rit_hz);
    /* A new frequency starts with the clarifier centred. */
    fm_hf_tune(7100000, FM_HF_LSB);
    LS_EQ_INT(0, fm_hf_get()->rit_hz);
}

LS_CASE(filters_and_steps_follow_the_rasters_asked_for)
{
    fresh();
    const int demods[] = { FM_HF_AM, FM_HF_SAM, FM_HF_USB, FM_HF_LSB, FM_HF_CW };
    const uint32_t wide[] = { 8000, 8000, 2800, 2800, 500 };
    const uint32_t narrow[] = { 6000, 6000, 2400, 2400, 250 };
    for (int i = 0; i < 5; ++i) {
        fm_hf_set_demod(demods[i]);
        fm_hf_set_narrow(false);
        LS_EQ_UINT(wide[i], fm_hf_filter_hz(fm_hf_get()));
        fm_hf_set_narrow(true);
        LS_EQ_UINT(narrow[i], fm_hf_filter_hz(fm_hf_get()));
    }
    uint32_t s = 10, seen = 0;
    const uint32_t ladder[] = { 10, 50, 100, 500, 1000, 5000, 9000, 10000 };
    for (int i = 0; i < 8; ++i) { LS_EQ_UINT(ladder[i], s); s = fm_hf_step_next(s); ++seen; }
    LS_EQ_UINT(10, s);                                      /* wraps */
    LS_EQ_UINT(8, seen);
}

LS_CASE(the_nvs_word_round_trips_and_refuses_garbage)
{
    const fm_hf_settings_t a = { .demod = FM_HF_CW, .narrow = 1, .agc = 0, .nb = 2,
                                 .rit_hz = 0, .pitch_hz = 650, .step_hz = 50 };
    fm_hf_settings_t b;
    memset(&b, 0x55, sizeof(b));
    LS_CHECK(fm_hf_unpack(fm_hf_pack(&a), &b));
    LS_EQ_INT(a.demod, b.demod); LS_EQ_INT(a.narrow, b.narrow);
    LS_EQ_INT(a.agc, b.agc); LS_EQ_INT(a.nb, b.nb);
    LS_EQ_INT(a.pitch_hz, b.pitch_hz); LS_EQ_UINT(a.step_hz, b.step_hz);
    LS_EQ_INT(0, b.rit_hz);
    const fm_hf_settings_t before = b;
    LS_CHECK(!fm_hf_unpack(0, &b));                         /* never saved */
    LS_CHECK(!fm_hf_unpack(0x20000000u, &b));               /* a later version */
    LS_CHECK(!fm_hf_unpack(fm_hf_pack(&a) | 0x0e000000u, &b));   /* demod 7 */
    LS_CHECK(!memcmp(&before, &b, sizeof(b)));
}

LS_CASE(demod_words_parse_and_ssb_picks_the_sideband)
{
    int d = -1;
    LS_CHECK(fm_hf_demod_parse("usb", 7000000, &d)); LS_EQ_INT(FM_HF_USB, d);
    LS_CHECK(fm_hf_demod_parse("SAM", 7000000, &d)); LS_EQ_INT(FM_HF_SAM, d);
    LS_CHECK(fm_hf_demod_parse("ssb", 7000000, &d)); LS_EQ_INT(FM_HF_LSB, d);
    LS_CHECK(fm_hf_demod_parse("ssb", 14000000, &d)); LS_EQ_INT(FM_HF_USB, d);
    LS_CHECK(fm_hf_demod_parse("auto", 10120000, &d)); LS_EQ_INT(FM_HF_CW, d);
    LS_CHECK(!fm_hf_demod_parse("fm", 7000000, &d));
    LS_CHECK(!fm_hf_demod_parse(NULL, 7000000, &d));
    LS_EQ_STR("LSB", fm_hf_demod_label(FM_HF_LSB));
    LS_EQ_STR("?", fm_hf_demod_label(9));
}

LS_CASE(an_rtl_reaches_hf_by_direct_sampling_or_the_v4_upconverter)
{
    /* Plain R820T: the tuner from 24 MHz, direct sampling below. */
    LS_EQ_INT(LS_RTL_PATH_DIRECT_Q, ls_rtl_path_for(500000, false));
    LS_EQ_INT(LS_RTL_PATH_DIRECT_Q, ls_rtl_path_for(23999999, false));
    LS_EQ_INT(LS_RTL_PATH_TUNER, ls_rtl_path_for(24000000, false));
    LS_EQ_INT(LS_RTL_PATH_TUNER, ls_rtl_path_for(27185000, false));   /* CB on the tuner */
    /* Blog V4: never direct sampling; its upconverter below 28 MHz. */
    LS_EQ_INT(LS_RTL_PATH_UPCONVERT, ls_rtl_path_for(500000, true));
    LS_EQ_INT(LS_RTL_PATH_UPCONVERT, ls_rtl_path_for(27999999, true));
    LS_EQ_INT(LS_RTL_PATH_TUNER, ls_rtl_path_for(28000000, true));
    LS_EQ_INT(LS_RTL_PATH_TUNER, ls_rtl_path_for(144000000, true));
    LS_EQ_STR("direct-Q", ls_rtl_path_name(LS_RTL_PATH_DIRECT_Q));
    LS_EQ_STR("none", ls_rtl_path_name(LS_RTL_PATH_NONE));
    /* No gap: the tuner takes over before direct sampling runs out. */
    LS_CHECK(LS_RTL_TUNER_MIN_HZ < LS_RTL_DIRECT_MAX_HZ);
}
