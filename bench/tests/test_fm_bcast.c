/* The FM broadcast extras' shared state: options packed into one word,
   the screen-to-receiver request queue, the published snapshots and the
   rule for trusting RDS clock time. */
#include "ls_test.h"
#include "fm_bcast.h"

#include <string.h>

static uint32_t s_saved = 0xFFFFFFFFu;
uint32_t settings_get_fm_bcast(uint32_t fallback) { return s_saved == 0xFFFFFFFFu ? fallback : s_saved; }
void settings_set_fm_bcast(uint32_t value) { s_saved = value; }

LS_CASE(options_pack_bits_and_the_weather_speaker_without_disturbing_each_other)
{
    s_saved = 0xFFFFFFFFu;
    fm_bcast_load();
    LS_EQ_UINT(fm_bcast_options(), 0);
    fm_bcast_set_opt(FM_BC_MONO, true);
    fm_bcast_set_wx_audio(2);
    fm_bcast_set_opt(FM_BC_SSTV, true);
    LS_CHECK(fm_bcast_opt(FM_BC_MONO));
    LS_CHECK(fm_bcast_opt(FM_BC_SSTV));
    LS_EQ_INT(fm_bcast_wx_audio(), 2);
    fm_bcast_set_wx_audio(1);
    LS_EQ_INT(fm_bcast_wx_audio(), 1);
    LS_CHECK(fm_bcast_opt(FM_BC_MONO) && fm_bcast_opt(FM_BC_SSTV));
    fm_bcast_set_wx_audio(7);                       /* out of range: off */
    LS_EQ_INT(fm_bcast_wx_audio(), 0);
    /* What was saved comes back after a restart. */
    const uint32_t word = s_saved;
    fm_bcast_set_opt(FM_BC_MONO, false);
    s_saved = word;
    fm_bcast_load();
    LS_CHECK(fm_bcast_opt(FM_BC_MONO));
}

LS_CASE(requests_queue_four_in_order_and_refuse_a_fifth)
{
    int req, arg;
    while (fm_air_take(&req, &arg)) {}
    fm_air_request(FM_AIR_REQ_PRESET, AIR_PRESET_AIR833);
    fm_air_request(FM_AIR_REQ_LISTEN, 1234);
    fm_air_request(FM_AIR_REQ_HOLD, 1);
    fm_air_request(FM_AIR_REQ_SKIP, 0);
    fm_air_request(FM_AIR_REQ_LOCKOUT, 0);          /* full: dropped */
    LS_CHECK(fm_air_take(&req, &arg)); LS_EQ_INT(req, FM_AIR_REQ_PRESET); LS_EQ_INT(arg, AIR_PRESET_AIR833);
    LS_CHECK(fm_air_take(&req, &arg)); LS_EQ_INT(req, FM_AIR_REQ_LISTEN); LS_EQ_INT(arg, 1234);
    LS_CHECK(fm_air_take(&req, &arg)); LS_EQ_INT(req, FM_AIR_REQ_HOLD);
    LS_CHECK(fm_air_take(&req, &arg)); LS_EQ_INT(req, FM_AIR_REQ_SKIP);
    LS_CHECK(!fm_air_take(&req, &arg));
}

LS_CASE(rds_clock_is_trusted_only_after_two_groups_a_minute_apart)
{
    fm_ct_trust_t t;
    memset(&t, 0, sizeof(t));
    const int64_t ct = 1793198220;
    LS_CHECK(!fm_ct_trust(&t, ct, 10000000));
    /* The same group again is no news. */
    LS_CHECK(!fm_ct_trust(&t, ct, 12000000));
    /* A minute on, by both clocks: now it is believed. */
    LS_CHECK(fm_ct_trust(&t, ct + 60, 70000000));
    /* A jump the uptime does not agree with starts over. */
    LS_CHECK(!fm_ct_trust(&t, ct + 3600, 130000000));
    LS_CHECK(!fm_ct_trust(&t, ct + 3660, 150000000));       /* 20 s later, not 60 */
}

LS_CASE(the_wfm_history_stands_still_without_publishes_and_clears_on_idle)
{
    static fm_wfm_status_t s;
    memset(&s, 0, sizeof(s));
    s.running = true;
    s.stereo = true;
    static uint8_t l[FM_WFM_HIST], r[FM_WFM_HIST], q[FM_WFM_HIST];
    static bool mono[FM_WFM_HIST];
    s.running = false;
    fm_wfm_publish(&s);
    LS_EQ_INT(fm_wfm_history(l, r, q, mono, FM_WFM_HIST), 0);
    s.running = true;
    for (int i = 0; i < 10; ++i) {
        s.lvl_l = 0.5f; s.lvl_r = 0.01f;
        s.rds.blocks_ok += 4;
        if (i == 5) s.rds.blocks_bad += 3;
        fm_wfm_publish(&s);
    }
    LS_EQ_INT(fm_wfm_history(l, r, q, mono, FM_WFM_HIST), 10);
    LS_CHECK(l[9] > r[9]);
    LS_EQ_INT(q[9], 1);
    LS_EQ_INT(q[5], 2);
    LS_CHECK(!mono[9]);
    fm_wfm_status_t back;
    fm_wfm_status(&back);
    LS_CHECK(back.running && back.pubs >= 11);
    s.running = false;
    fm_wfm_publish(&s);
    LS_EQ_INT(fm_wfm_history(l, r, q, mono, FM_WFM_HIST), 0);
}
