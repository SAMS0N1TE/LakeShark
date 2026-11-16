/* WIFI SURVEY's recorder core (app-sdk/apps/wifi_survey/survey_rec.c): the
   rules and the WiGLE CSV LINK's SURVEY tab had (these are its old
   test_survey.c cases, on the app's text API), plus what is new here: the
   hash, the one-sighting-a-second rule and LAP_DOT11 security names. */
#include "ls_test.h"
#include "survey_rec.h"
#include "lap_dot11.h"
#include <math.h>

static sr_entry_t table[SR_CAP];
static uint16_t heads[SR_BUCKETS];
static sr_session_t session;
static char evicted[4096];
static size_t evicted_n;

static sr_obs_t observation(int rssi, unsigned id)
{
    sr_obs_t o = {.kind = SR_WIFI, .rssi = rssi, .channel = 6, .sec = LAP_DOT11_SEC_WPA2};
    o.addr[4] = id >> 8; o.addr[5] = id;
    return o;
}
static sr_fix_t fix_at(double lat, double lon, int64_t ms) { return (sr_fix_t){true, lat, lon, NAN, NAN, ms}; }
static void on_evict(void *ctx, const sr_entry_t *e)
{
    (void)ctx;
    const int n = sr_summary(evicted + evicted_n, sizeof(evicted) - evicted_n, e);
    if (n > 0) evicted_n += (size_t)n;
}

LS_CASE(dedupe_keeps_the_best_signal_and_its_position)
{
    sr_init(&session, table, heads, SR_CAP, false);
    sr_fix_t g = fix_at(42, -71, 1000);
    sr_obs_t o = observation(-70, 1);
    LS_EQ_INT(sr_note(&session, &o, &g, 1791374400, 1000), SR_ADDED);
    g = fix_at(43, -71, 3000); o.rssi = -40;
    LS_EQ_INT(sr_note(&session, &o, &g, 1791374402, 3000), SR_UPDATED);
    g = fix_at(44, -71, 4000); o.rssi = -80;
    sr_note(&session, &o, &g, 1791374403, 4000);
    LS_EQ_INT(session.count, 1); LS_EQ_INT(table[0].rssi, -40);
    LS_NEAR(table[0].lat, 43, 0.000001);
    LS_EQ_UINT(table[0].first_s, 1791374400);
    LS_EQ_UINT(session.positioned, 1);
}

LS_CASE(position_is_never_attached_to_an_old_or_future_fix)
{
    sr_init(&session, table, heads, 8, false);
    sr_fix_t g = fix_at(42, -71, 1000);
    LS_CHECK(sr_fix_fresh(&g, 11000));
    LS_CHECK(!sr_fix_fresh(&g, 11001));
    sr_obs_t o = observation(-60, 1);
    sr_note(&session, &o, &g, 0, 11001);
    LS_CHECK(!table[0].positioned); LS_NEAR(table[0].lat, 0, 0);
    o.addr[5] = 2;
    sr_note(&session, &o, &g, 0, 999);
    LS_CHECK(!table[1].positioned);
    g.fix = false; LS_CHECK(!sr_fix_fresh(&g, 2000));
    g.fix = true; g.lat = NAN; LS_CHECK(!sr_fix_fresh(&g, 2000));
    g.lat = 91; LS_CHECK(!sr_fix_fresh(&g, 2000));
    /* A fix taken before the app opened is a negative time, still fresh. */
    g = fix_at(42, -71, -3000);
    LS_CHECK(sr_fix_fresh(&g, 500));
}

LS_CASE(a_stronger_unpositioned_observation_does_not_keep_an_older_position)
{
    sr_init(&session, table, heads, 8, false);
    sr_fix_t g = fix_at(42, -71, 1000);
    sr_obs_t o = observation(-60, 1);
    sr_note(&session, &o, &g, 0, 1000);
    o.rssi = -30;
    sr_note(&session, &o, &g, 0, 12000);
    LS_CHECK(!table[0].positioned); LS_NEAR(table[0].lat, 0, 0);
    LS_EQ_UINT(session.positioned, 0);
    /* ONLY GPS FIX: without a fresh fix nothing is taken in. */
    sr_init(&session, table, heads, 8, true);
    LS_EQ_INT(sr_note(&session, &o, &g, 0, 12000), SR_IGNORED);
    LS_EQ_INT(session.count, 0);
    LS_EQ_INT(sr_note(&session, &o, &g, 0, 1000), SR_ADDED);
}

LS_CASE(the_table_is_bounded_but_existing_addresses_still_update)
{
    sr_init(&session, table, heads, SR_CAP + 100, false);
    for (unsigned i = 0; i < SR_CAP + 1; i++) {
        sr_obs_t o = observation(-80, i);
        sr_note(&session, &o, NULL, 0, 1000 + i);
    }
    LS_EQ_UINT(session.count, SR_CAP); LS_EQ_UINT(session.dropped, 1);
    /* id 0 was the least recently heard: it went, so it comes back new. */
    sr_obs_t o = observation(-20, 0);
    LS_EQ_INT(sr_note(&session, &o, NULL, 0, 9000), SR_ADDED);
    LS_EQ_INT(table[session.current].rssi, -20);
    LS_EQ_UINT(session.count, SR_CAP);
    /* every address still in the table is found again through the hash */
    o = observation(-10, 4000);
    LS_EQ_INT(sr_note(&session, &o, NULL, 0, 9500), SR_UPDATED);
    LS_EQ_INT(table[session.current].rssi, -10);
}

LS_CASE(ble_address_type_and_wifi_bssid_define_identity)
{
    sr_init(&session, table, heads, 8, false);
    sr_obs_t o = observation(-70, 1);
    strcpy(o.name, "same name");
    sr_note(&session, &o, NULL, 0, 1000);
    o.addr[5] = 2; sr_note(&session, &o, NULL, 0, 1000);
    o.kind = SR_BLE; sr_note(&session, &o, NULL, 0, 1000);
    o.addr_type = 1; sr_note(&session, &o, NULL, 0, 1000);
    LS_EQ_UINT(session.count, 4);
    LS_EQ_UINT(session.totals[0], 2); LS_EQ_UINT(session.totals[1], 2);
}

LS_CASE(one_sighting_a_second_counts_unless_it_is_stronger)
{
    sr_init(&session, table, heads, 8, false);
    sr_fix_t g = fix_at(0, 0, 1000);
    sr_obs_t o = observation(-60, 1);
    LS_EQ_INT(sr_note(&session, &o, &g, 0, 1000), SR_ADDED);
    LS_EQ_INT(sr_note(&session, &o, &g, 0, 1100), SR_IGNORED);   /* a beacon 100 ms on */
    LS_EQ_UINT(table[0].samples, 1); LS_EQ_UINT(table[0].seen, 1); LS_EQ_UINT(table[0].last_ms, 1100);
    o.rssi = -50;
    LS_EQ_INT(sr_note(&session, &o, &g, 0, 1200), SR_UPDATED);   /* stronger: taken */
    LS_EQ_UINT(table[0].samples, 2);
    o.rssi = -70;
    LS_EQ_INT(sr_note(&session, &o, &g, 0, 2200), SR_UPDATED);   /* a second on */
    LS_EQ_UINT(table[0].samples, 3); LS_EQ_UINT(table[0].seen, 3);
    sr_note(&session, &o, &g, 0, 13300);                           /* the fix went stale: seen, not placed */
    LS_EQ_UINT(table[0].samples, 3); LS_EQ_UINT(table[0].seen, 4);
}

LS_CASE(wigle_exact_header_rows_escaping_and_utc_required)
{
    sr_obs_t o = observation(-60, 1);
    strcpy(o.name, "Cafe, \"north\"\r\nline");
    sr_fix_t g = {true, 42, -71, 12.5f, 1.2f, 1000};
    char csv[2048] = "";
    int n = sr_header(csv, sizeof(csv), "2.13.2", "bench");
    LS_CHECK(n > 0);
    n += sr_row(csv + n, sizeof(csv) - n, &o, &g, 1791374400);
    o.kind = SR_BLE; o.name[0] = 0;
    n += sr_row(csv + n, sizeof(csv) - n, &o, NULL, 1791374400);
    char spare[256];
    LS_EQ_INT(sr_row(spare, sizeof(spare), &o, &g, 0), -1);              /* no UTC: no row */
    LS_EQ_INT(sr_row(spare, sizeof(spare), &o, &g, 1500000000), -1);     /* a clock never set */
    LS_EQ_INT(sr_row(spare, 20, &o, &g, 1791374400), -1);                /* too long */
    LS_EQ_STR(spare, "");
    LS_EQ_STR(csv,
        "WigleWifi-1.6,appRelease=2.13.2,model=ESP32-P4,release=ESP-IDF,device=LakeShark,display=TUI,board=bench,brand=LakeShark,star=Sol,body=3,subBody=0\n"
        "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type\n"
        "00:00:00:00:00:01,\"Cafe, \"\"north\"\"\r\nline\",[WPA2-PSK-CCMP][ESS],2026-10-07 12:00:00,6,2437,-60,42.0000000,-71.0000000,12.5,6.0,,,WIFI\n"
        "00:00:00:00:00:01,\"\",[BLE],2026-10-07 12:00:00,0,0,-60,,,,,,,BLE\n");
    LS_EQ_INT(n, (int)strlen(csv));
    /* 5 GHz now carries its frequency (LINK's C6 never heard one). */
    o = observation(-70, 2); o.channel = 36;
    LS_CHECK(sr_row(spare, sizeof(spare), &o, NULL, 1791374400) > 0);
    LS_CHECK(strstr(spare, ",36,5180,-70,") != NULL);
}

LS_CASE(utc_text_and_session_names)
{
    char t[20], name[64];
    LS_CHECK(sr_utc_text(t, 1791374400)); LS_EQ_STR(t, "2026-10-07 12:00:00");
    LS_CHECK(sr_utc_text(t, 1709210096)); LS_EQ_STR(t, "2024-02-29 12:34:56");   /* a leap day */
    LS_CHECK(sr_utc_text(t, 1577836800)); LS_EQ_STR(t, "2020-01-01 00:00:00");
    LS_CHECK(sr_utc_text(t, 4102444799u)); LS_EQ_STR(t, "2099-12-31 23:59:59");
    LS_CHECK(sr_filename(name, sizeof(name), 1791374400));
    LS_EQ_STR(name, "20261007_120000.csv");
    LS_CHECK(!sr_filename(name, sizeof(name), 123));
    LS_CHECK(!sr_filename(name, 4, 1791374400));
    LS_CHECK(sr_session_name("20261007_120000.csv"));
    LS_CHECK(sr_session_name("20261007_120000_002.csv"));
    LS_CHECK(sr_session_name("undated_0000012345.csv"));
    LS_CHECK(!sr_session_name("20261007_120000_summary.csv"));
    LS_CHECK(!sr_session_name("notes.csv"));
    LS_CHECK(!sr_session_name("20261007-120000.csv"));
}

LS_CASE(auth_names_for_every_beacon_suite)
{
    LS_EQ_STR(sr_auth(LAP_DOT11_SEC_OPEN), "[ESS]");
    LS_EQ_STR(sr_auth(LAP_DOT11_SEC_WEP), "[WEP][ESS]");
    LS_EQ_STR(sr_auth(LAP_DOT11_SEC_WPA), "[WPA-PSK-TKIP][ESS]");
    LS_EQ_STR(sr_auth(LAP_DOT11_SEC_WPA2), "[WPA2-PSK-CCMP][ESS]");
    LS_EQ_STR(sr_auth(LAP_DOT11_SEC_WPA2_EAP), "[WPA2-EAP-CCMP][ESS]");
    LS_EQ_STR(sr_auth(LAP_DOT11_SEC_WPA2_WPA3), "[WPA2-PSK-CCMP][WPA3-SAE-CCMP][ESS]");
    LS_EQ_STR(sr_auth(LAP_DOT11_SEC_WPA3), "[WPA3-SAE-CCMP][ESS]");
    LS_EQ_STR(sr_auth(LAP_DOT11_SEC_OWE), "[OWE-CCMP][ESS]");
    LS_EQ_STR(sr_auth(LAP_DOT11_SEC_UNKNOWN), "[UNKNOWN][ESS]");
    LS_EQ_STR(sr_auth(200), "[UNKNOWN][ESS]");
    LS_EQ_UINT(sr_security(LAP_DOT11_SEC_WPA2_WPA3), 24);
    LS_EQ_UINT(sr_security(LAP_DOT11_SEC_OPEN), 1);
    LS_EQ_UINT(sr_security(200), 0);
}

LS_CASE(relog_uses_last_row_not_best_signal_and_current_fix)
{
    sr_entry_t e = {0};
    sr_obs_t o = observation(-70, 1);
    sr_fix_t g = fix_at(0, 0, 1000);
    LS_CHECK(sr_relog(&e, &o, &g, 1000));
    sr_logged(&e, &o, &g, 1000);
    o.rssi = -65; LS_CHECK(!sr_relog(&e, &o, &g, 1000));
    o.rssi = -64; LS_CHECK(sr_relog(&e, &o, &g, 1000));
    sr_logged(&e, &o, &g, 1000);
    o.rssi = -80; g.lat = 49.9 / 111194.9266;
    LS_CHECK(!sr_relog(&e, &o, &g, 1000));
    g.lat = 50.1 / 111194.9266; LS_CHECK(sr_relog(&e, &o, &g, 1000));
    LS_CHECK(!sr_relog(&e, &o, &g, 12000));          /* the fix went stale */
    sr_logged(&e, &o, &g, 1000);
    o.rssi = -74; LS_CHECK(sr_relog(&e, &o, &g, 1000));
    e.row_lon = 179.9999; g.lon = -179.9999; g.lat = e.row_lat;
    o.rssi = -90; LS_CHECK(!sr_relog(&e, &o, &g, 1000));   /* 22 m across the date line */
    /* A row written without a position is written again once one comes. */
    sr_entry_t u = {0};
    sr_fix_t none = {0};
    sr_logged(&u, &o, &none, 1000);
    LS_CHECK(!sr_relog(&u, &o, &none, 1000));
    LS_CHECK(sr_relog(&u, &o, &g, 1000));
}

LS_CASE(centroid_weights_every_fresh_sighting_and_handles_dateline)
{
    sr_init(&session, table, heads, 2, false);
    sr_obs_t o = observation(-60, 1);
    sr_fix_t g = fix_at(0, 0, 1000);
    sr_note(&session, &o, &g, 0, 1000);
    o.rssi = -50; g.lon = 0.001; sr_note(&session, &o, &g, 0, 1001);
    double lat, lon;
    LS_CHECK(sr_centroid(&table[0], &lat, &lon));
    LS_NEAR(lat, 0, 1e-8); LS_NEAR(lon, 0.001 * 10 / 11, 1e-8);
    LS_EQ_UINT(table[0].samples, 2);
    sr_note(&session, &o, &g, 0, 12000); LS_EQ_UINT(table[0].samples, 2);   /* stale fix */
    sr_init(&session, table, heads, 2, false); o.rssi = -60; g.lon = 179.999;
    sr_note(&session, &o, &g, 0, 1000); g.lon = -179.999;
    sr_note(&session, &o, &g, 0, 2000);
    LS_CHECK(sr_centroid(&table[0], &lat, &lon)); LS_NEAR(fabs(lon), 180, 1e-6);
}

LS_CASE(eviction_is_oldest_seen_and_preserves_summary)
{
    sr_init(&session, table, heads, 2, false);
    session.evicted = on_evict; evicted_n = 0; evicted[0] = 0;
    sr_fix_t g = fix_at(42, -71, 1000);
    sr_obs_t o = observation(-60, 1);
    sr_note(&session, &o, &g, 0, 1000);
    g.fix_ms = 2000; o.addr[5] = 2; sr_note(&session, &o, &g, 0, 2000);
    g.fix_ms = 3000; o.addr[5] = 1; sr_note(&session, &o, &g, 0, 3000);
    g.fix_ms = 4000; o.addr[5] = 3; LS_EQ_INT(sr_note(&session, &o, &g, 0, 4000), SR_ADDED);
    LS_EQ_UINT(table[0].addr[5], 1); LS_EQ_UINT(table[1].addr[5], 3);
    LS_EQ_UINT(session.totals[0], 3); LS_EQ_UINT(session.count, 2);
    LS_CHECK(strstr(evicted, "00:00:00:00:00:02,WIFI,0,\"\",1,42.0000000,-71.0000000,") != NULL);
    /* the summary of one never placed has empty coordinates */
    sr_entry_t lone = {.kind = SR_BLE, .addr_type = 1};
    char row[160];
    LS_CHECK(sr_summary(row, sizeof(row), &lone) > 0);
    LS_EQ_STR(row, "00:00:00:00:00:00,BLE,1,\"\",0,,,0\n");
    LS_CHECK(sr_summary_header(row, sizeof(row)) > 0);
    LS_EQ_STR(row, "MAC,Type,AddressType,SSID,Samples,EstimatedLatitude,EstimatedLongitude,Weight\n");
}

LS_CASE(new_a_minute_counts_each_kind)
{
    sr_init(&session, table, heads, 8, false);
    sr_obs_t o = observation(-60, 1);
    sr_note(&session, &o, NULL, 0, 1000);
    o.addr[5] = 2; o.kind = SR_BLE; sr_note(&session, &o, NULL, 0, 30000);
    unsigned m[2];
    sr_minute(&session, 31000, m); LS_EQ_UINT(m[0], 1); LS_EQ_UINT(m[1], 1);
    sr_minute(&session, 62000, m); LS_EQ_UINT(m[0], 0); LS_EQ_UINT(m[1], 1);
}

LS_CASE(sort_orders_strongest_newest_and_name_with_stable_ties)
{
    sr_entry_t a = {.rssi = -70, .first_ms = 2000}, b = {.rssi = -30, .first_ms = 1000};
    a.addr[5] = 1; b.addr[5] = 2;
    strcpy(a.name, "alpha"); strcpy(b.name, "beta");
    LS_CHECK(sr_compare(&a, &b, 0) > 0);
    LS_CHECK(sr_compare(&a, &b, 1) < 0);
    LS_CHECK(sr_compare(&a, &b, 2) < 0);
}
