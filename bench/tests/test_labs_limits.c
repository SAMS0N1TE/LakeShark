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

LS_CASE(a_background_claim_keeps_its_rank_only_while_its_task_asks)
{
    ls_radio_claims_t c = {0};
    const int64_t quiet = 5000;
    LS_EQ_UINT(ls_radio_claims_peek(&c, 1, 0, quiet), 0);
    LS_EQ_UINT(ls_radio_claims_touch(&c, 1, 0, quiet, false), 0);
    const uint32_t a = ls_radio_claims_touch(&c, 1, 0, quiet, true);
    const uint32_t b = ls_radio_claims_touch(&c, 2, 100, quiet, true);
    LS_CHECK(a && b > a);
    /* Asking again keeps the place, with or without a hold. */
    LS_EQ_UINT(ls_radio_claims_touch(&c, 1, 4000, quiet, false), a);
    LS_EQ_UINT(ls_radio_claims_touch(&c, 1, 9000, quiet, true), a);
    /* Silence loses it: 2 last asked at 100. */
    LS_EQ_UINT(ls_radio_claims_peek(&c, 2, 9000, quiet), 0);
    LS_EQ_UINT(ls_radio_claims_touch(&c, 2, 9000, quiet, false), 0);
    LS_CHECK(ls_radio_claims_touch(&c, 2, 9000, quiet, true) > b);
    /* A full table gives up its stalest entry, never a live one. */
    for (uintptr_t t = 3; t <= 9; t++) LS_CHECK(ls_radio_claims_touch(&c, t, 9000 + (int64_t)t, quiet, true));
    LS_EQ_UINT(ls_radio_claims_peek(&c, 1, 9010, quiet), 0);
    LS_CHECK(ls_radio_claims_peek(&c, 2, 9010, quiet));
    LS_EQ_UINT(ls_radio_claims_touch(&c, 0, 9010, quiet, true), 0);
}

LS_CASE(a_dropped_claim_ranks_last_until_its_task_releases)
{
    ls_radio_claims_t c = {0};
    const int64_t quiet = 5000, life = 60000;
    const uint32_t a = ls_radio_claims_touch(&c, 1, 0, quiet, true);
    const uint32_t b = ls_radio_claims_touch(&c, 2, 100, quiet, true);
    LS_CHECK(!ls_radio_claims_dropped(&c, 2, 100, life));
    /* 2 is newest, goes silent and is dropped: now below 1, and it stays
       marked however often it asks. */
    ls_radio_claims_drop(&c, 2, 9000);
    LS_CHECK(ls_radio_claims_dropped(&c, 2, 20000, life));
    const uint32_t low = ls_radio_claims_peek(&c, 2, 20000, quiet);
    LS_CHECK(low && low < a && low < b);
    LS_EQ_UINT(ls_radio_claims_touch(&c, 2, 20000, quiet, true), low);
    LS_CHECK(ls_radio_claims_dropped(&c, 2, 20000, life));
    /* A second dropped holder ranks below the first: no ties. */
    ls_radio_claims_touch(&c, 3, 20000, quiet, true);
    ls_radio_claims_drop(&c, 3, 20000);
    LS_CHECK(ls_radio_claims_peek(&c, 3, 20000, quiet) < low);
    /* Released behind a holder that outranks it: unmarked, place kept. */
    ls_radio_claims_release(&c, 2, 21000, true);
    LS_CHECK(!ls_radio_claims_dropped(&c, 2, 21000, life));
    LS_EQ_UINT(ls_radio_claims_touch(&c, 2, 22000, quiet, false), low);
    /* Released with nobody ahead: the claim ends, the next is the newest. */
    ls_radio_claims_release(&c, 2, 23000, false);
    LS_EQ_UINT(ls_radio_claims_peek(&c, 2, 23000, quiet), 0);
    LS_CHECK(ls_radio_claims_touch(&c, 2, 23000, quiet, true) > b);
    /* A task that never releases: the mark is forgotten in the end. */
    LS_CHECK(ls_radio_claims_dropped(&c, 3, 20000 + life, life));
    LS_CHECK(!ls_radio_claims_dropped(&c, 3, 20001 + life, life));
    LS_EQ_UINT(ls_radio_claims_peek(&c, 3, 20001 + life, quiet), 0);
    /* A dropped claim is the last a full table gives up. */
    ls_radio_claims_t f = {0};
    for (uintptr_t t = 1; t <= 8; t++) ls_radio_claims_touch(&f, t, (int64_t)t, quiet, true);
    ls_radio_claims_drop(&f, 1, 1);
    ls_radio_claims_touch(&f, 9, 100, quiet, true);
    LS_CHECK(ls_radio_claims_dropped(&f, 1, 100, life));
    LS_EQ_UINT(ls_radio_claims_peek(&f, 2, 100, quiet), 0);
}
