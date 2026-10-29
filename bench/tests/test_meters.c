/* LS_TEST_SOURCES: apps/tui/ls_meters.c with experiments/lr433_dec.c. The
   METERS store: only meters are filed, a reading's history and its rate,
   use per bucket, a counter that goes backwards, eviction, pinned order,
   and the pins file round trip. */

#include "ls_test.h"

#include "ls_meters.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static lsm_store_t S;

static lr433_msg_t meter(int proto, const char *id, long long reading)
{
    lr433_msg_t m;
    memset(&m, 0, sizeof(m));
    m.proto = (uint8_t)proto;
    m.channel = -1;
    m.battery_ok = -1;
    snprintf(m.model, sizeof(m.model), "%s", lr433_proto_name(proto));
    snprintf(m.id, sizeof(m.id), "%s", id);
    m.kpa = m.temp_c = m.humidity = m.rain_mm = m.wind_ms = NAN;
    m.consumption = reading;
    return m;
}

LS_CASE(only_meters_are_filed)
{
    lsm_init(&S);
    lr433_msg_t t = meter(LR433_P_FORD, "ABC", -1);
    LS_EQ_INT(lsm_receive(&S, &t, 315000000, -60, 0, 10), -1);
    lr433_msg_t m = meter(LR433_P_ERT_SCM, "54585868", 562456);
    const int slot = lsm_receive(&S, &m, 912600000, NAN, 0, 10);
    LS_CHECK(slot >= 0);
    LS_EQ_INT(S.m[slot].value, 562456);
    LS_EQ_INT(S.m[slot].heard, 1);
    LS_EQ_INT(lsm_receive(&S, &m, 912600000, NAN, 0, 40), slot);
    LS_EQ_INT(S.m[slot].heard, 2);
    LS_EQ_INT(S.m[slot].count, 1);              /* same reading, soon: no new point */
    /* The same ID under another protocol is another meter. */
    lr433_msg_t idm = meter(LR433_P_ERT_IDM, "54585868", 562456);
    LS_CHECK(lsm_receive(&S, &idm, 912600000, NAN, 0, 41) != slot);
}

LS_CASE(rate_is_units_an_hour_over_the_history)
{
    lsm_init(&S);
    lr433_msg_t m = meter(LR433_P_ERT_SCMPLUS, "68211547", 1000);
    const int slot = lsm_receive(&S, &m, 0, NAN, 0, 100);
    LS_CHECK(isnan(lsm_rate_per_hour(&S.m[slot])));          /* one reading */
    m.consumption = 1010;
    lsm_receive(&S, &m, 0, NAN, 0, 100 + 1800);
    m.consumption = 1020;
    lsm_receive(&S, &m, 0, NAN, 0, 100 + 3600);
    LS_NEAR(lsm_rate_per_hour(&S.m[slot]), 20.0, 1e-9);
    /* A flat hour after it halves the rate: it is heard, not forgotten. */
    for (int k = 1; k <= 6; k++) lsm_receive(&S, &m, 0, NAN, 0, 100 + 3600 + k * 600);
    LS_NEAR(lsm_rate_per_hour(&S.m[slot]), 10.0, 1e-9);
    LS_EQ_INT(S.m[slot].count, 3 + 6);
    /* Too short a span is no rate. */
    lsm_init(&S);
    m.consumption = 5;
    lsm_receive(&S, &m, 0, NAN, 0, 10);
    m.consumption = 6;
    lsm_receive(&S, &m, 0, NAN, 0, 40);
    LS_CHECK(isnan(lsm_rate_per_hour(&S.m[0])));
}

LS_CASE(a_counter_that_goes_backwards_starts_over)
{
    lsm_init(&S);
    lr433_msg_t m = meter(LR433_P_ERT_SCM, "1", 900);
    lsm_receive(&S, &m, 0, NAN, 0, 0 + 1);
    m.consumption = 990;
    lsm_receive(&S, &m, 0, NAN, 0, 3601);
    m.consumption = 3;                                 /* replaced or rolled */
    lsm_receive(&S, &m, 0, NAN, 0, 3700);
    LS_CHECK(isnan(lsm_rate_per_hour(&S.m[0])));
    m.consumption = 7;
    lsm_receive(&S, &m, 0, NAN, 0, 3700 + 7200);
    LS_NEAR(lsm_rate_per_hour(&S.m[0]), 2.0, 1e-9);
}

LS_CASE(usage_per_bucket_follows_the_readings)
{
    lsm_init(&S);
    lr433_msg_t m = meter(LR433_P_ERT_SCM, "7", 0);
    lsm_receive(&S, &m, 0, NAN, 0, 1000);
    m.consumption = 0;
    lsm_receive(&S, &m, 0, NAN, 0, 1000 + 600);      /* idle first quarter-ish */
    m.consumption = 30;
    lsm_receive(&S, &m, 0, NAN, 0, 1000 + 1200);
    m.consumption = 30;
    lsm_receive(&S, &m, 0, NAN, 0, 1000 + 2400);
    float u[4];
    const int64_t span = lsm_usage(&S.m[0], 4, u);
    LS_EQ_INT(span, 600);
    LS_NEAR(u[0], 0.0, 1e-4);
    LS_NEAR(u[1], 30.0, 1e-4);
    LS_NEAR(u[2], 0.0, 1e-4);
    LS_NEAR(u[3], 0.0, 1e-4);
    float sum = 0;
    float v[24];
    lsm_usage(&S.m[0], 24, v);
    for (int i = 0; i < 24; i++) sum += v[i];
    LS_NEAR(sum, 30.0, 1e-3);
    lr433_msg_t one = meter(LR433_P_ERT_SCM, "8", 5);
    const int s2 = lsm_receive(&S, &one, 0, NAN, 0, 10);
    LS_EQ_INT(lsm_usage(&S.m[s2], 4, u), 0);
}

LS_CASE(pinned_first_then_newest_and_full_drops_the_oldest_unpinned)
{
    lsm_init(&S);
    char id[16];
    for (int i = 0; i < LSM_MAX; i++) {
        snprintf(id, sizeof(id), "%d", 1000 + i);
        lr433_msg_t m = meter(LR433_P_ERT_SCM, id, i);
        lsm_receive(&S, &m, 0, NAN, 0, 100 + i);
    }
    const int oldest = lsm_find(&S, LR433_P_ERT_SCM, "1000");
    const int second = lsm_find(&S, LR433_P_ERT_SCM, "1001");
    LS_CHECK(lsm_pin(&S, oldest, true));
    LS_CHECK(S.pins_dirty);
    lr433_msg_t m = meter(LR433_P_ERT_SCM, "9999", 1);
    LS_CHECK(lsm_receive(&S, &m, 0, NAN, 0, 500) >= 0);
    LS_CHECK(lsm_find(&S, LR433_P_ERT_SCM, "1000") == oldest);   /* pinned stays */
    LS_EQ_INT(lsm_find(&S, LR433_P_ERT_SCM, "1001"), -1);         /* dropped */
    (void)second;
    uint8_t order[LSM_MAX];
    LS_EQ_INT(lsm_order(&S, order), LSM_MAX);
    LS_EQ_INT(order[0], oldest);
    LS_EQ_STR(S.m[order[1]].id, "9999");
}

LS_CASE(pins_file_round_trips_and_waits_for_unheard_meters)
{
    lsm_init(&S);
    lr433_msg_t a = meter(LR433_P_ERT_SCM, "54585868", 562456);
    lr433_msg_t b = meter(LR433_P_ERT_IDM, "11278109", 339972);
    lr433_msg_t c = meter(LR433_P_ERT_SCMPLUS, "68211547", 10);
    const int sa = lsm_receive(&S, &a, 0, NAN, 0, 10);
    const int sb = lsm_receive(&S, &b, 0, NAN, 0, 11);
    lsm_receive(&S, &c, 0, NAN, 0, 12);
    lsm_pin(&S, sa, true);
    lsm_name(&S, sa, "HOUSE \"main\"\\");
    lsm_name(&S, sb, "GAS");
    LS_EQ_STR(S.m[sa].name, "HOUSE main");
    char json[1024];
    const size_t n = lsm_pins_json(&S, json, sizeof(json));
    LS_CHECK(n > 0);
    printf("%s", json);
    LS_CHECK(strstr(json, "\"v\":1") != NULL);
    LS_CHECK(strstr(json, "68211547") == NULL);                 /* neither pinned nor named */
    LS_EQ_UINT(lsm_pins_json(&S, json, 40), 0u);                /* does not fit */
    lsm_pins_json(&S, json, sizeof(json));

    lsm_init(&S);
    LS_EQ_INT(lsm_pins_load(&S, json), 2);
    const int la = lsm_find(&S, LR433_P_ERT_SCM, "54585868");
    const int lb = lsm_find(&S, LR433_P_ERT_IDM, "11278109");
    LS_CHECK(la >= 0 && lb >= 0);
    LS_CHECK(S.m[la].pinned && !S.m[lb].pinned);
    LS_EQ_STR(S.m[la].name, "HOUSE main");
    LS_EQ_STR(S.m[lb].name, "GAS");
    LS_EQ_INT(S.m[la].value, -1);                               /* waiting */
    LS_EQ_INT(lsm_receive(&S, &a, 0, NAN, 0, 99), la);          /* and found */
    LS_EQ_INT(S.m[la].value, 562456);

    LS_EQ_INT(lsm_pins_load(&S, "{\"v\":2,\"meters\":[]}"), -1);
    LS_EQ_INT(lsm_pins_load(&S, "not json"), -1);
    LS_EQ_INT(lsm_pins_load(&S, "{\"v\":1,\"meters\":[{\"type\":\"Ford\",\"id\":\"1\"}]}"), 0);
    lsm_clear(&S);
    LS_CHECK(lsm_find(&S, LR433_P_ERT_SCM, "54585868") >= 0);   /* pinned survives CLEAR */
}
