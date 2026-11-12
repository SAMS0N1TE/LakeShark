#include "ls_test.h"
#include "subghz_file.h"
#include "tx_protection.h"
#include "ls_labs_limits.h"

#include <stdlib.h>
#include <string.h>

LS_CASE(unlocked_replay_takes_any_band_power_length_and_duration)
{
    LS_CHECK(LS_TX_UNLOCKED);
    LS_CHECK(subghz_tx_refusal(2450000000u, 22, 6, 1456) == NULL);
    LS_CHECK(subghz_tx_refusal(150000000u, 22, 6, 1456) == NULL);
    LS_CHECK(subghz_tx_refusal(915000000u, 22, 6, 1456) == NULL);
    LS_CHECK(subghz_tx_refusal(433920000u, -30, 6, 1456) == NULL);
    LS_CHECK(subghz_tx_refusal(433920000u, 10, 100000, 60000000ull) == NULL);
    LS_CHECK(subghz_tx_refusal(433920000u, 10, 2, 100) == NULL);
    LS_CHECK(subghz_tx_refusal(433920000u, 10, 1, 100) != NULL);
    LS_CHECK(subghz_tx_radio_refusal(1000000u, 30) == NULL);
}

LS_CASE(unlocked_ook_packing_has_no_edge_or_time_cap)
{
    const size_t n = 10000;
    int32_t *e = malloc(n * sizeof(*e));
    LS_CHECK(e != NULL);
    if (!e) return;
    for (size_t i = 0; i < n; i++) e[i] = (i % 2) ? -2000 : 2000;
    const size_t words = subghz_ook_symbols(e, n, NULL, 0);
    LS_CHECK(words > 0);
    uint32_t *out = malloc(words * sizeof(*out));
    LS_CHECK(out != NULL);
    if (out) LS_CHECK(subghz_ook_symbols(e, n, out, words) == words);
    int32_t bad[4] = {100, 100, -100, 100};
    LS_CHECK(subghz_ook_symbols(bad, 4, NULL, 0) == 0);
    free(out);
    free(e);
}

LS_CASE(unlocked_power_steps_reach_the_chip_maximum)
{
    LS_EQ_INT(SUBGHZ_TX_STEPS, 7);
    LS_EQ_INT(subghz_tx_step_dbm(0, false), -9);
    LS_EQ_INT(subghz_tx_step_dbm(SUBGHZ_TX_STEPS - 1, false), 22);
    LS_EQ_INT(subghz_tx_step_dbm(SUBGHZ_TX_STEPS - 1, true), 10);
    LS_CHECK(!strcmp(subghz_tx_step_name(SUBGHZ_TX_STEPS - 1), "FULL"));
}

LS_CASE(unlocked_protection_has_no_power_duty_or_heat_budget)
{
    uint64_t now = 1000000000ull;
    for (int i = 0; i < 30; i++) {
        LS_EQ_INT(ls_tx_protect(LS_TX_SOCKET, 22, false, 10000000u, now), 0);
        LS_EQ_INT(ls_tx_protect(LS_TX_MIXRF, 20, false, 10000000u, now), 0);
        LS_EQ_INT(ls_tx_protect(LS_TX_HACKRF, 30, false, 10000000u, now), 0);
        now += 1000000ull;
    }
    LS_EQ_INT(ls_tx_protect(LS_TX_SOCKET, 22, true, 1000u, now), -24);
    ls_tx_thermal(LS_TX_SOCKET, true);
    LS_EQ_INT(ls_tx_protect(LS_TX_SOCKET, 0, false, 1000u, now), -23);
    ls_tx_thermal(LS_TX_SOCKET, false);
    LS_EQ_INT(ls_tx_protect(LS_TX_SOCKET, 0, false, 1000u, now), 0);
}

LS_CASE(unlocked_labs_send_has_no_band_power_or_duty_limit)
{
    ls_labs_tx_budget_t budget;
    memset(&budget, 0, sizeof(budget));
    ls_lora_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.freq_hz = 433920000u;
    cfg.bw_hz = 125000;
    cfg.power_dbm = 22;
    for (int i = 0; i < 40; i++) {
        LS_CHECK(ls_labs_tx_check(&cfg, 5000, 1000, &budget) == NULL);
        ls_labs_tx_charge(&budget, 5000, 1000);
    }
    LS_CHECK(ls_labs_tx_check(&cfg, 0, 1000, &budget) != NULL);
}
