#include "ls_test.h"
#include "experiments/sensor_history.h"

static sh_store_t S;
static lr433_msg_t message(void)
{
    lr433_msg_t m = { .proto = LR433_P_ACURITE_TOWER, .channel = -1,
        .battery_ok = 1, .temp_c = -15, .humidity = 40, .kpa = NAN,
        .rain_mm = NAN, .wind_ms = NAN };
    strcpy(m.id, "123");
    return m;
}

LS_CASE(key_and_burst_dedupe)
{
    sh_init(&S); lr433_msg_t m = message(); bool added;
    int a = sh_receive(&S, &m, 100, &added); LS_CHECK(added);
    LS_EQ_INT(sh_receive(&S, &m, 101, &added), a); LS_CHECK(!added);
    LS_EQ_INT(S.sensor[a].last_seen, 101);
    sh_receive(&S, &m, 102, &added); LS_CHECK(added);
    m.channel = 1; int b = sh_receive(&S, &m, 103, &added); LS_CHECK(b != a);
    m.proto = LR433_P_AMBIENT_F007TH; LS_CHECK(sh_receive(&S, &m, 103, &added) != b);
    m.proto = LR433_P_ACURITE_TOWER; strcpy(m.id, "124"); LS_CHECK(sh_receive(&S, &m, 103, &added) != b);
}
LS_CASE(ring_wrap_and_partial_fields)
{
    sh_init(&S); lr433_msg_t m = message(); bool added;
    for (int i = 0; i < SH_READINGS + 20; i++) { m.temp_c = i; sh_receive(&S, &m, 100 + 3 * i, &added); LS_CHECK(added); }
    sh_sensor_t *d = &S.sensor[0];
    LS_EQ_INT(d->count, SH_READINGS); LS_NEAR(sh_at(d, 0)->temp_c, 20, .01);
    LS_NEAR(sh_at(d, SH_READINGS - 1)->temp_c, SH_READINGS + 19, .01);
    LS_CHECK(sh_at(d, SH_READINGS) == NULL);
    m.temp_c = NAN; m.rain_mm = 2.54f; m.wind_ms = 3;
    sh_receive(&S, &m, 2000, &added);
    LS_NEAR(d->last.temp_c, SH_READINGS + 19, .01);
    LS_CHECK(isnan(sh_at(d, d->count - 1)->temp_c));
    LS_NEAR(d->last.rain_mm, 2.54, .001);
}
LS_CASE(csv_quotes_and_battery_low)
{
    sh_init(&S); lr433_msg_t m = message(); bool added; m.battery_ok = 0;
    int a = sh_receive(&S, &m, 100, &added);
    strcpy(S.sensor[a].name, "Cold, \"room\"\nA");
    FILE *f = ls_test_tmpfile(); LS_CHECK(f != NULL); if (!f) return;
    LS_CHECK(sh_csv(f, &S.sensor[a], &S.sensor[a].raw));
    rewind(f); char text[512] = {0}; fread(text, 1, sizeof(text) - 1, f); fclose(f);
    LS_CHECK(strstr(text, ",\"Cold, \"\"room\"\"\nA\",") != NULL);
    LS_CHECK(strstr(text, ",1\n") != NULL);
}
LS_CASE(threshold_hysteresis_and_naming)
{
    sh_init(&S); lr433_msg_t m = message(); bool added;
    int a = sh_receive(&S, &m, 100, &added); sh_sensor_t *d = &S.sensor[a];
    d->alert_on = true; d->threshold_c = -10;
    LS_CHECK(!sh_threshold(d, -5)); strcpy(d->name, "Freezer");
    LS_CHECK(!sh_threshold(d, NAN)); LS_CHECK(!sh_threshold(d, -10));
    LS_CHECK(sh_threshold(d, -9)); LS_CHECK(!sh_threshold(d, -9.5f));
    LS_CHECK(!sh_threshold(d, -10.5f)); LS_CHECK(!sh_threshold(d, -9));
    LS_CHECK(!sh_threshold(d, -11)); LS_CHECK(sh_threshold(d, -9));
}
LS_CASE(stranger_aging_sort_retention_and_cap)
{
    sh_init(&S); lr433_msg_t m = message(); bool added; uint8_t order[SH_SENSORS];
    int a = sh_receive(&S, &m, 100, &added); strcpy(S.sensor[a].name, "Mine");
    strcpy(m.id, "stranger"); int b = sh_receive(&S, &m, 101, &added);
    LS_EQ_INT(sh_order(&S, order), 2); LS_EQ_INT(order[0], a);
    S.show_strangers = false; LS_EQ_INT(sh_order(&S, order), 1);
    sh_expire(&S, 101 + SH_DAY); LS_CHECK(!S.sensor[b].used); LS_CHECK(S.sensor[a].used);
    S.days = 1; sh_expire(&S, 101 + SH_DAY); LS_EQ_INT(S.sensor[a].count, 0);
    for (int i = 0; i < SH_SENSORS; i++) {
        snprintf(m.id, sizeof(m.id), "%d", i);
        int slot = sh_receive(&S, &m, 200000, &added);
        if (slot >= 0) strcpy(S.sensor[slot].name, "Pinned");
    }
    strcpy(m.id, "overflow"); LS_EQ_INT(sh_receive(&S, &m, 200000, &added), -1);
}
LS_CASE(tpms_opt_in_no_backfill_and_revoke)
{
    sh_init(&S); lr433_msg_t m = message(); bool added;
    m.proto = LR433_P_FORD; m.kpa = 250;
    int a = sh_receive(&S, &m, 100, &added); sh_sensor_t *d = &S.sensor[a];
    LS_CHECK(!added); LS_EQ_INT(d->count, 0); LS_CHECK(isnan(d->last.temp_c)); LS_CHECK(isnan(d->last.kpa));
    FILE *f = ls_test_tmpfile(); LS_CHECK(f != NULL); if (!f) return;
    LS_CHECK(!sh_csv(f, d, &d->raw)); LS_EQ_INT(ftell(f), 0);
    strcpy(d->name, "Wheel"); sh_receive(&S, &m, 103, &added); LS_CHECK(!added);
    sh_own(d, true); sh_receive(&S, &m, 106, &added); LS_CHECK(added); LS_EQ_INT(d->count, 1);
    LS_CHECK(sh_csv(f, d, &d->raw)); fclose(f);
    sh_own(d, false); LS_EQ_INT(d->count, 0); LS_CHECK(isnan(d->last.kpa));
    sh_receive(&S, &m, 109, &added); LS_CHECK(!added);
}
LS_CASE(hidden_keys_suppress_capture)
{
    sh_init(&S); lr433_msg_t m = message(); bool added; uint8_t order[SH_SENSORS];
    int a = sh_receive(&S, &m, 100, &added); S.sensor[a].hidden = true;
    sh_receive(&S, &m, 103, &added); LS_CHECK(!added);
    LS_EQ_INT(sh_order(&S, order), 0);
    sh_expire(&S, 100 + 2 * SH_DAY); LS_CHECK(S.sensor[a].used);
}
