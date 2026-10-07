#include "ls_test.h"
#include "ls_labs_limits.h"
#include "ls_radio_holders.h"

LS_CASE(send_requires_us915_occupied_band_and_power_cap)
{
    ls_labs_tx_budget_t budget = {0};
    ls_lora_cfg_t cfg = {.freq_hz=915000000, .bw_hz=125000, .power_dbm=14};
    LS_CHECK(!ls_labs_tx_check(&cfg, 100, 1000000, &budget));
    cfg.freq_hz = 433920000;
    LS_CHECK(ls_labs_tx_check(&cfg, 100, 1000000, &budget));
    cfg.freq_hz = LS_MESH_US915_MIN_HZ;
    LS_CHECK(ls_labs_tx_check(&cfg, 100, 1000000, &budget));
    cfg.freq_hz += 62500;
    LS_CHECK(!ls_labs_tx_check(&cfg, 100, 1000000, &budget));
    cfg.freq_hz = LS_MESH_US915_MAX_HZ;
    LS_CHECK(ls_labs_tx_check(&cfg, 100, 1000000, &budget));
    cfg.freq_hz -= 62500;
    LS_CHECK(!ls_labs_tx_check(&cfg, 100, 1000000, &budget));
    cfg.power_dbm = 15;
    LS_CHECK(ls_labs_tx_check(&cfg, 100, 1000000, &budget));
    cfg.power_dbm = -10;
    LS_CHECK(ls_labs_tx_check(&cfg, 100, 1000000, &budget));
}

LS_CASE(airtime_and_rolling_duty_refuse_before_keying)
{
    ls_labs_tx_budget_t budget = {0};
    ls_lora_cfg_t cfg = {.freq_hz=915000000, .bw_hz=125000, .power_dbm=14};
    LS_CHECK(ls_labs_tx_check(&cfg, 0, 1000000, &budget));
    LS_CHECK(ls_labs_tx_check(&cfg, 401, 1000000, &budget));
    LS_CHECK(!ls_labs_tx_check(&cfg, 400, 1000000, &budget));
    ls_labs_tx_charge(&budget, 400, 1000000);
    LS_CHECK(!ls_labs_tx_check(&cfg, 200, 1000000, &budget));
    ls_labs_tx_charge(&budget, 200, 1000000);
    LS_CHECK(ls_labs_tx_check(&cfg, 1, 1000000, &budget));
    LS_CHECK(ls_labs_tx_check(&cfg, 400, 61000000, &budget));
    LS_CHECK(!ls_labs_tx_check(&cfg, 400, 61400000, &budget));
    /* A clock going backwards cannot erase an existing charge. */
    LS_CHECK(ls_labs_tx_check(&cfg, 1, 0, &budget));
}

LS_CASE(fsk_numbers_are_checked_before_narrowing)
{
    LS_CHECK(ls_labs_fsk_number(255, 1));
    LS_CHECK(!ls_labs_fsk_number(256, 1));
    LS_CHECK(!ls_labs_fsk_number(257, 1));
    LS_CHECK(!ls_labs_fsk_number(-1, 4));
    LS_CHECK(!ls_labs_fsk_number(1200.5, 4));
    LS_CHECK(ls_labs_fsk_number(UINT32_MAX, 4));
    LS_CHECK(!ls_labs_fsk_number(4294967296.0, 4));
    LS_CHECK(!ls_labs_fsk_number(4294968496.0, 4));
    LS_CHECK(!ls_labs_fsk_number(NAN, 4));
    LS_CHECK(!ls_labs_fsk_number(INFINITY, 4));
    LS_CHECK(!ls_labs_fsk_number(1e100, 4));
}

LS_CASE(holders_are_counted_and_release_only_their_own_lease)
{
    ls_radio_holders_t h = {0};
    LS_CHECK(ls_radio_holders_set(&h, 1, true));
    LS_CHECK(ls_radio_holders_set(&h, 1, true));
    LS_EQ_UINT(h.count, 1);
    LS_CHECK(ls_radio_holders_set(&h, 2, true));
    LS_EQ_UINT(h.count, 2);
    LS_CHECK(ls_radio_holders_owns(&h, 1));
    LS_CHECK(!ls_radio_holders_owns(&h, 2));
    LS_CHECK(ls_radio_holders_set(&h, 3, false));
    LS_EQ_UINT(h.count, 2);
    LS_CHECK(ls_radio_holders_set(&h, 1, false));
    LS_EQ_UINT(h.count, 1);
    LS_CHECK(ls_radio_holders_owns(&h, 2));
    LS_CHECK(ls_radio_holders_set(&h, 1, false));
    LS_EQ_UINT(h.count, 1);
    LS_CHECK(ls_radio_holders_set(&h, 2, false));
    LS_EQ_UINT(h.count, 0);
}
