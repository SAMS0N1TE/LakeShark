/* LS_TEST_SOURCES: ${FW}/components/apps/shell/ls_hub_present.c */
#include "ls_test.h"
#include "shell/ls_hub_present.h"

#include "fm_state.h"
#include "rec_state.h"

static ls_hub_observation_t base(const char *backend)
{
    ls_hub_observation_t o = {0};
    o.backend_mode = backend;
    o.receiver_ready = true;
    o.freq_hz = 154785000u;
    o.signal_pct = 37;
    o.iq_bytes_sec = 512000u;
    return o;
}

LS_CASE(p25_summary_and_navigation_are_preserved)
{
    ls_hub_observation_t o = base("P25");
    ls_hub_presentation_t p;
    o.freq_hz = 851012500u;
    o.signal_pct = 84;
    o.active = true;
    o.p25_nac = 0x293;
    o.p25_tg = 1201;
    ls_hub_present(&o, &p);

    LS_EQ_STR(p.mode, "P25");
    LS_EQ_STR(p.target_app, "P25");
    LS_EQ_STR(p.detail, "NAC 293  TG 1201");
    LS_EQ_UINT(p.freq_hz, 851012500u);
    LS_EQ_INT(p.signal_pct, 84);
    LS_CHECK(p.active);
}

LS_CASE(ordinary_fm_keeps_analog_squelch_summary)
{
    ls_hub_observation_t o = base("FM");
    ls_hub_presentation_t p;
    o.fm_submode = FM_MODE_LISTEN;
    o.fm_squelch_tenths = 15;
    o.active = true;
    ls_hub_present(&o, &p);

    LS_EQ_STR(p.mode, "FM");
    LS_EQ_STR(p.target_app, "FM");
    LS_EQ_STR(p.detail, "OPEN  SQL 1.5");
    LS_CHECK(p.active);
}

LS_CASE(fm_acars_submode_maps_to_acars_face_and_target)
{
    ls_hub_observation_t o = base("FM");
    ls_hub_presentation_t p;
    o.freq_hz = 131550000u;
    o.fm_submode = FM_MODE_ACARS;
    o.fm_squelch_tenths = 27;
    o.active = true; /* stale analog squelch state must not leak through */
    ls_hub_present(&o, &p);

    LS_EQ_STR(p.mode, "ACARS");
    LS_EQ_STR(p.target_app, "ACARS");
    LS_EQ_STR(p.detail, "MSK 2400  AIRCRAFT TEXT");
    LS_EQ_UINT(p.freq_hz, 131550000u);
    LS_CHECK(!p.active);
    LS_CHECK(strstr(p.detail, "SQL") == NULL);
}

LS_CASE(adsb_summary_and_navigation_are_preserved)
{
    ls_hub_observation_t o = base("ADS-B");
    ls_hub_presentation_t p;
    o.freq_hz = 1090000000u;
    o.active = true;
    o.adsb_tracked = 7;
    o.adsb_msgs_sec = 19;
    ls_hub_present(&o, &p);

    LS_EQ_STR(p.mode, "ADS-B");
    LS_EQ_STR(p.target_app, "ADS-B");
    LS_EQ_STR(p.detail, "7 TRACKED  19/s");
    LS_EQ_INT(p.contacts, 7);
    LS_CHECK(p.active);
}

LS_CASE(rec_maps_frequency_phase_signal_and_navigation)
{
    static const struct { int phase; const char *detail; bool active; } cases[] = {
        { REC_IDLE,      "IDLE  23 EDGES",      false },
        { REC_ARMED,     "ARMED  23 EDGES",     true  },
        { REC_CAPTURING, "CAPTURING  23 EDGES", true  },
        { REC_DONE,      "DONE  23 EDGES",      false },
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ls_hub_observation_t o = base("REC");
        ls_hub_presentation_t p;
        o.freq_hz = 433920000u;
        o.rec_phase = cases[i].phase;
        o.rec_edges = 23;
        o.rec_mag_now = 64;
        o.rec_mag_thresh = 96;
        ls_hub_present(&o, &p);

        LS_EQ_STR(p.mode, "REC");
        LS_EQ_STR(p.target_app, "REC");
        LS_EQ_STR(p.detail, cases[i].detail);
        LS_EQ_UINT(p.freq_hz, 433920000u);
        LS_EQ_INT(p.signal_pct, 25);
        LS_EQ_INT(p.active, cases[i].active);
    }
}

LS_CASE(parked_receiver_has_no_live_activity_but_keeps_reopen_target)
{
    ls_hub_observation_t o = base("P25");
    ls_hub_presentation_t p;
    o.parked = true;
    o.freq_hz = 851012500u;
    o.active = true;
    o.signal_pct = 99;
    ls_hub_present(&o, &p);

    LS_EQ_STR(p.mode, "P25");
    LS_EQ_STR(p.target_app, "P25");
    LS_EQ_STR(p.detail, "RADIO PARKED");
    LS_EQ_UINT(p.freq_hz, 851012500u);
    LS_EQ_INT(p.signal_pct, 0);
    LS_EQ_UINT(p.iq_bytes_sec, 0);
    LS_CHECK(!p.active);
}

LS_CASE(absent_receiver_overrides_stale_activity_without_erasing_station)
{
    ls_hub_observation_t o = base("REC");
    ls_hub_presentation_t p;
    o.receiver_ready = false;
    o.freq_hz = 315000000u;
    o.signal_pct = 93;
    o.iq_bytes_sec = 512000u;
    o.rec_phase = REC_CAPTURING;
    o.rec_mag_now = 255;
    o.rec_mag_thresh = 80;
    ls_hub_present(&o, &p);

    LS_EQ_STR(p.mode, "REC");
    LS_EQ_STR(p.target_app, "REC");
    LS_EQ_STR(p.detail, "RECEIVER UNAVAILABLE");
    LS_EQ_UINT(p.freq_hz, 315000000u);
    LS_EQ_INT(p.signal_pct, 0);
    LS_EQ_UINT(p.iq_bytes_sec, 0);
    LS_CHECK(!p.active);
    LS_EQ_UINT(o.freq_hz, 315000000u);
}

LS_CASE(no_backend_is_idle_and_has_no_navigation_target)
{
    ls_hub_observation_t o = base("--");
    ls_hub_presentation_t p;
    ls_hub_present(&o, &p);

    LS_EQ_STR(p.mode, "--");
    LS_EQ_STR(p.target_app, "");
    LS_EQ_STR(p.detail, "IDLE");
    LS_EQ_UINT(p.freq_hz, 0);
    LS_CHECK(!p.active);
}
