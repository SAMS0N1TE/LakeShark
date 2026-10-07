#include "ls_test.h"
#include "ls_survey.h"

static ls_survey_entry_t table[LS_SURVEY_CAP];
static ls_survey_session_t session;
static ls_survey_entry_t observation(int rssi, unsigned id)
{
    ls_survey_entry_t o = {.kind=LS_SURVEY_WIFI, .rssi=rssi, .last_s=100,
                          .channel=6, .auth=3};
    o.addr[4] = id >> 8; o.addr[5] = id;
    return o;
}

LS_CASE(dedupe_keeps_the_best_signal_and_its_position)
{
    ls_survey_init(&session, table, LS_SURVEY_CAP, false);
    ls_survey_fix_t g = {true, 42, -71, 1000000};
    ls_survey_entry_t o = observation(-70, 1);
    LS_CHECK(ls_survey_note(&session, &o, &g, 1000000, 1000));
    g.lat = 43; o.rssi = -40; o.last_s = 102;
    LS_CHECK(!ls_survey_note(&session, &o, &g, 3000000, 3000));
    g.lat = 44; o.rssi = -80; o.last_s = 103;
    ls_survey_note(&session, &o, &g, 4000000, 4000);
    LS_EQ_INT(session.count, 1); LS_EQ_INT(table[0].rssi, -40);
    LS_NEAR(table[0].lat, 43, 0.000001);
    LS_EQ_UINT(table[0].best_s, 102); LS_EQ_UINT(table[0].last_s, 103);
    LS_EQ_UINT(table[0].first_s, 100);
}

LS_CASE(position_is_never_attached_to_an_old_or_future_fix)
{
    ls_survey_init(&session, table, 8, false);
    ls_survey_fix_t g = {true, 42, -71, 1000000};
    LS_CHECK(ls_survey_fix_fresh(&g, 11000000));
    LS_CHECK(!ls_survey_fix_fresh(&g, 11000001));
    ls_survey_entry_t o = observation(-60, 1);
    ls_survey_note(&session, &o, &g, 11000001, 11000);
    LS_CHECK(!table[0].positioned); LS_NEAR(table[0].lat, 0, 0);
    o.addr[5] = 2;
    ls_survey_note(&session, &o, &g, 999999, 999);
    LS_CHECK(!table[1].positioned);
    g.fix = false; LS_CHECK(!ls_survey_fix_fresh(&g, 2000000));
    g.fix = true; g.lat = NAN; LS_CHECK(!ls_survey_fix_fresh(&g, 2000000));
    g.lat = 91; LS_CHECK(!ls_survey_fix_fresh(&g, 2000000));
}

LS_CASE(a_stronger_unpositioned_observation_does_not_keep_an_older_position)
{
    ls_survey_init(&session, table, 8, false);
    ls_survey_fix_t g = {true, 42, -71, 1000000};
    ls_survey_entry_t o = observation(-60, 1);
    ls_survey_note(&session, &o, &g, 1000000, 1000);
    o.rssi = -30;
    ls_survey_note(&session, &o, &g, 12000000, 12000);
    LS_CHECK(!table[0].positioned); LS_NEAR(table[0].lat, 0, 0);
    ls_survey_init(&session, table, 8, true);
    LS_CHECK(!ls_survey_note(&session, &o, &g, 12000000, 12000));
    LS_EQ_INT(session.count, 0);
    LS_CHECK(ls_survey_note(&session, &o, &g, 1000000, 1000));
}

LS_CASE(the_table_is_bounded_but_existing_addresses_still_update)
{
    ls_survey_init(&session, table, LS_SURVEY_CAP + 100, false);
    for (unsigned i = 0; i < LS_SURVEY_CAP + 1; i++) {
        ls_survey_entry_t o = observation(-80, i);
        ls_survey_note(&session, &o, NULL, 1000000, 1000);
    }
    LS_EQ_UINT(session.count, LS_SURVEY_CAP); LS_EQ_UINT(session.dropped, 1);
    ls_survey_entry_t o = observation(-20, 0);
    ls_survey_note(&session, &o, NULL, 2000000, 2000);
    LS_EQ_INT(table[0].rssi, -20);
    LS_EQ_UINT(session.count, LS_SURVEY_CAP);
}

LS_CASE(ble_address_type_and_wifi_bssid_define_identity)
{
    ls_survey_init(&session, table, 8, false);
    ls_survey_entry_t o = observation(-70, 1);
    strcpy(o.name, "same name");
    ls_survey_note(&session, &o, NULL, 1000000, 1000);
    o.addr[5] = 2; ls_survey_note(&session, &o, NULL, 1000000, 1000);
    o.kind = LS_SURVEY_BLE; ls_survey_note(&session, &o, NULL, 1000000, 1000);
    o.addr_type = 1; ls_survey_note(&session, &o, NULL, 1000000, 1000);
    LS_EQ_UINT(session.count, 4);
}

LS_CASE(csv_quotes_names_and_leaves_unknown_position_empty)
{
    ls_survey_init(&session, table, 8, false);
    ls_survey_entry_t o = observation(-60, 1);
    strcpy(o.name, "Cafe, \"north\"\nline");
    ls_survey_note(&session, &o, NULL, 1000000, 1000);
    o.kind = LS_SURVEY_BLE; o.addr_type = 1; o.name[0] = 0;
    ls_survey_note(&session, &o, NULL, 1000000, 1000);
    FILE *f = ls_test_tmpfile(); LS_CHECK(f != NULL); if (!f) return;
    LS_CHECK(ls_survey_csv(f, &session)); rewind(f);
    char csv[2048] = "";
    fread(csv, 1, sizeof(csv) - 1, f); fclose(f);
    LS_CHECK(strstr(csv, "kind,name,address,address_type,channel,auth,rssi,") == csv);
    LS_CHECK(strstr(csv, "wifi,\"Cafe, \"\"north\"\"\nline\",00:00:00:00:00:01,,6,3,-60,100,100,100,uptime,,,\n"));
    LS_CHECK(strstr(csv, "ble,\"\",00:00:00:00:00:01,1,,,-60,"));
    LS_CHECK(strstr(csv, "Random BLE addresses rotate"));
}

LS_CASE(session_names_use_utc_and_explicit_undated_fallback)
{
    char name[64];
    LS_CHECK(ls_survey_filename(name, sizeof(name), 0, true));
    LS_EQ_STR(name, "19700101_000000.csv");
    LS_CHECK(ls_survey_filename(name, sizeof(name), 86461, true));
    LS_EQ_STR(name, "19700102_000101.csv");
    LS_CHECK(ls_survey_filename(name, sizeof(name), 123, false));
    LS_EQ_STR(name, "undated_0000000123.csv");
    LS_CHECK(!ls_survey_filename(name, 4, 123, true));
}

LS_CASE(sort_orders_strongest_newest_and_name_with_stable_ties)
{
    ls_survey_entry_t a = observation(-70, 1), b = observation(-30, 2);
    strcpy(a.name, "alpha"); strcpy(b.name, "beta");
    a.first_ms = 2000; b.first_ms = 1000;
    LS_CHECK(ls_survey_compare(&a, &b, 0) > 0);
    LS_CHECK(ls_survey_compare(&a, &b, 1) < 0);
    LS_CHECK(ls_survey_compare(&a, &b, 2) < 0);
}
