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
    ls_survey_fix_t g = {true, 42, -71, 1000000, 0, 0};
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
    ls_survey_fix_t g = {true, 42, -71, 1000000, 0, 0};
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
    ls_survey_fix_t g = {true, 42, -71, 1000000, 0, 0};
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
    LS_EQ_INT(table[session.current].rssi, -20);
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

LS_CASE(wigle_exact_header_rows_escaping_and_utc_required)
{
    ls_survey_entry_t o = observation(-60, 1);
    strcpy(o.name, "Cafe, \"north\"\r\nline"); o.epoch = true;
    ls_survey_fix_t g = {true,42,-71,1000000,12.5,1.2};
    FILE *f = ls_test_tmpfile(); LS_CHECK(f != NULL); if (!f) return;
    LS_CHECK(ls_survey_header(f, "2.13.2", "bench"));
    LS_CHECK(ls_survey_row(f, &o, &g, 1791374400));
    o.kind = LS_SURVEY_BLE; o.name[0] = 0;
    LS_CHECK(ls_survey_row(f, &o, NULL, 1791374400));
    long end = ftell(f); o.epoch = false;
    LS_CHECK(!ls_survey_row(f, &o, &g, 1791374400)); LS_CHECK(ftell(f) == end);
    o.epoch = true; LS_CHECK(!ls_survey_row(f, &o, &g, 0));
    rewind(f); char csv[2048] = ""; fread(csv, 1, sizeof(csv)-1, f); fclose(f);
    LS_EQ_STR(csv,
        "WigleWifi-1.6,appRelease=2.13.2,model=ESP32-P4,release=ESP-IDF,device=LakeShark,display=TUI,board=bench,brand=LakeShark,star=Sol,body=3,subBody=0\n"
        "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type\n"
        "00:00:00:00:00:01,\"Cafe, \"\"north\"\"\r\nline\",[WPA2-PSK-CCMP][ESS],2026-10-07 12:00:00,6,2437,-60,42.0000000,-71.0000000,12.5,6.0,,,WIFI\n"
        "00:00:00:00:00:01,\"\",[BLE],2026-10-07 12:00:00,0,0,-60,,,,,,,BLE\n");
}

LS_CASE(session_names_require_utc)
{
    char name[64];
    LS_CHECK(ls_survey_filename(name, sizeof(name), 1791374400, true));
    LS_EQ_STR(name, "20261007_120000.csv");
    LS_CHECK(!ls_survey_filename(name, sizeof(name), 123, false));
    LS_CHECK(!ls_survey_filename(name, 4, 123, true));
}

LS_CASE(auth_mapping_all_idf_55_modes)
{
    static const char *const expected[] = {
        "[ESS]", "[WEP][ESS]", "[WPA-PSK-TKIP][ESS]", "[WPA2-PSK-CCMP][ESS]",
        "[WPA-PSK-TKIP][WPA2-PSK-CCMP][ESS]", "[WPA2-EAP-CCMP][ESS]",
        "[WPA3-SAE-CCMP][ESS]", "[WPA2-PSK-CCMP][WPA3-SAE-CCMP][ESS]",
        "[WAPI-PSK-SMS4][ESS]", "[OWE-CCMP][ESS]", "[WPA3-EAP-SUITE-B-192-GCMP-256][ESS]",
        "[WPA3-SAE-CCMP][ESS]", "[WPA3-SAE-CCMP][ESS]", "[DPP][ESS]",
        "[WPA3-EAP-CCMP][ESS]", "[WPA2-EAP-CCMP][WPA3-EAP-CCMP][ESS]", "[WPA-EAP-TKIP][ESS]"
    };
    for (unsigned i=0; i<17; i++) LS_EQ_STR(ls_survey_auth(i), expected[i]);
    LS_EQ_STR(ls_survey_auth(255), "[UNKNOWN][ESS]");
    LS_EQ_UINT(ls_survey_security(4), 12); LS_EQ_UINT(ls_survey_security(7), 24);
    LS_EQ_UINT(ls_survey_security(255), 0);
}

LS_CASE(relog_uses_last_row_not_best_signal_and_current_fix)
{
    ls_survey_entry_t e = {0}, o = observation(-70, 1);
    ls_survey_fix_t g = {true,0,0,1000000,0,0};
    LS_CHECK(ls_survey_relog(&e,&o,&g,1000000));
    ls_survey_logged(&e,&o,&g,1000000);
    o.rssi=-65; LS_CHECK(!ls_survey_relog(&e,&o,&g,1000000));
    o.rssi=-64; LS_CHECK(ls_survey_relog(&e,&o,&g,1000000));
    ls_survey_logged(&e,&o,&g,1000000);
    o.rssi=-80; g.lat=49.9/111194.9266;
    LS_CHECK(!ls_survey_relog(&e,&o,&g,1000000));
    g.lat=50.1/111194.9266; LS_CHECK(ls_survey_relog(&e,&o,&g,1000000));
    LS_CHECK(!ls_survey_relog(&e,&o,&g,12000000));
    ls_survey_logged(&e,&o,&g,1000000);
    o.rssi=-74; LS_CHECK(ls_survey_relog(&e,&o,&g,1000000));
    e.row_lon=179.9999; g.lon=-179.9999; g.lat=e.row_lat;
    o.rssi=-90; LS_CHECK(!ls_survey_relog(&e,&o,&g,1000000));
}

LS_CASE(centroid_weights_every_fresh_sighting_and_handles_dateline)
{
    ls_survey_init(&session,table,2,false);
    ls_survey_entry_t o=observation(-60,1);
    ls_survey_fix_t g={true,0,0,1000000,0,0};
    ls_survey_note(&session,&o,&g,1000000,1000);
    o.rssi=-50; g.lon=0.001; ls_survey_note(&session,&o,&g,1000000,1001);
    double lat,lon; LS_CHECK(ls_survey_centroid(&table[0],&lat,&lon));
    LS_NEAR(lat,0,1e-8); LS_NEAR(lon,0.001*10/11,1e-8);
    LS_EQ_UINT(table[0].samples,2);
    ls_survey_note(&session,&o,&g,12000000,12000); LS_EQ_UINT(table[0].samples,2);
    ls_survey_init(&session,table,2,false); o.rssi=-60; g.lon=179.999;
    ls_survey_note(&session,&o,&g,1000000,1000); g.lon=-179.999;
    ls_survey_note(&session,&o,&g,1000000,1000);
    LS_CHECK(ls_survey_centroid(&table[0],&lat,&lon)); LS_NEAR(fabs(lon),180,1e-6);
}

LS_CASE(eviction_is_oldest_seen_and_preserves_summary)
{
    ls_survey_init(&session,table,2,false);
    FILE *f=ls_test_tmpfile(); LS_CHECK(f); if(!f) return;
    session.summary=f;
    ls_survey_fix_t g={true,42,-71,1000000,0,0};
    ls_survey_entry_t o=observation(-60,1);
    ls_survey_note(&session,&o,&g,1000000,1000);
    o.addr[5]=2; ls_survey_note(&session,&o,&g,1000000,2000);
    o.addr[5]=1; ls_survey_note(&session,&o,&g,1000000,3000);
    o.addr[5]=3; LS_CHECK(ls_survey_note(&session,&o,&g,1000000,4000));
    LS_EQ_UINT(table[0].addr[5],1); LS_EQ_UINT(table[1].addr[5],3);
    LS_EQ_UINT(session.totals[0],3); LS_EQ_UINT(session.count,2);
    rewind(f); char csv[512]=""; fread(csv,1,sizeof(csv)-1,f); fclose(f);
    LS_CHECK(strstr(csv,"00:00:00:00:00:02,WIFI,0,\"\",1,42.0000000,-71.0000000,"));
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
