/* DETECTOR the SD app: its engine (app-sdk/apps/detector/det_engine.c) as
   the app builds it. The shipped rules parse; malformed files are refused
   and the old rules kept; each source's predicates; cooldowns, rule windows
   and the token bucket; quiet hours; drone watch's built-in rule; shared
   mesh alerts in and out. */
#include "ls_test.h"
#include "det.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static det_t *d;
static int deliveries;
static det_event_t last;
static uint8_t last_sinks;
static uint8_t deliver(det_t *x, det_event_t *e, uint8_t sinks) {
    (void)x;
    deliveries++;
    last = *e;
    last_sinks = sinks;
    return sinks;
}
static void fresh(const char *json) {
    if (!d) d = malloc(sizeof(*d));
    memset(d, 0, sizeof(*d));
    d->deliver = deliver;
    det_init(d);
    deliveries = 0;
    memset(&last, 0, sizeof(last));
    if (json) LS_CHECK(det_parse(d, json, strlen(json)));
}
static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *b = malloc(DET_RULES_BYTES + 1);
    *n = fread(b, 1, DET_RULES_BYTES + 1, f);
    fclose(f);
    return b;
}
static int find(const char *id) {
    for (unsigned i = 0; i < d->n_rules; i++) if (!strcmp(d->rules[i].id, id)) return (int)i;
    return -1;
}

LS_CASE(detector_app_builtin_and_shipped_rules) {
    fresh(NULL);
    LS_EQ_INT(d->n_rules, 11);
    LS_CHECK(find("remote-id") >= 0 && d->rules[find("remote-id")].enabled);
    size_t n = 0;
    char *text = slurp("data/detector/rules.json", &n);
    LS_CHECK(text != NULL);
    LS_CHECK(det_parse(d, text, n));
    LS_EQ_INT(d->n_rules, 22);
    LS_CHECK(find("p25-emergency") >= 0 && find("mesh-emergency") >= 0 && find("subghz-new-433") >= 0);
    free(text);
}

LS_CASE(detector_app_refuses_bad_files_and_keeps_rules) {
    fresh(NULL);
    const unsigned before = d->n_rules;
    static const char *const bad[] = {
        "{\"version\":2,\"rules\":[]}",
        "{\"version\":1,\"rules\":[{\"id\":\"x\",\"sources\":[\"nope\"],\"match\":{},\"sinks\":[]}]}",
        "{\"version\":1,\"rules\":[{\"id\":\"x\",\"sources\":[\"adsb\",\"sweep.ble\"],\"match\":{\"emergency_status\":true},\"sinks\":[]}]}",
        "{\"version\":1,\"rules\":[{\"id\":\"x\",\"sources\":[\"p25\"],\"match\":{\"category_in\":[\"CAMERA\"]},\"sinks\":[]}]}",
        "{\"version\":1,\"rules\":[{\"id\":\"a b\",\"sources\":[\"p25\"],\"match\":{},\"sinks\":[]}]}",
        "{\"version\":1,\"rules\":[],\"rules\":[]}",
        "{\"version\":1,\"rules\":[]} trailing",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        LS_CHECK(!det_parse(d, bad[i], strlen(bad[i])));
        LS_EQ_INT(d->n_rules, before);
    }
    LS_CHECK(d->error[0]);
}

static const char *const SIG_RULES =
    "{\"version\":1,\"defaults\":{\"enabled\":true,\"cooldown_s\":60,\"rule_cooldown_s\":0},\"rules\":["
    "{\"id\":\"tg\",\"sources\":[\"p25\"],\"match\":{\"talkgroup_in\":[4512]},\"sinks\":[\"banner\"]},"
    "{\"id\":\"emg\",\"sources\":[\"p25\"],\"match\":{\"emergency\":true},\"sinks\":[\"tts\"]},"
    "{\"id\":\"words\",\"sources\":[\"pocsag\"],\"match\":{\"text_contains_any_ci\":[\"fire\"]},\"sinks\":[\"banner\"]},"
    "{\"id\":\"ism\",\"sources\":[\"rec.watch\"],\"match\":{\"frequency_khz\":[433000,435000],\"novel_pattern\":true},\"sinks\":[\"tone\"]},"
    "{\"id\":\"node\",\"sources\":[\"mesh.peer\"],\"match\":{\"event_in\":[\"lost\"]},\"sinks\":[\"banner\"]}]}";

LS_CASE(detector_app_signal_predicates) {
    fresh(SIG_RULES);
    det_signal_t p = {.source = DET_P25, .a = 4512, .b = 77};
    det_signal(d, &p, 1000000);
    LS_EQ_INT(deliveries, 1);
    LS_EQ_STR(last.reason, "tg");
    LS_EQ_INT(last_sinks, DET_BANNER);
    p.a = 9; p.flag = 1;
    det_signal(d, &p, 3000000);
    LS_EQ_INT(deliveries, 2);
    LS_EQ_STR(last.reason, "emg");
    LS_CHECK(det_emergency(&last));
    char words[96];
    det_speech(&last, words, sizeof(words));
    LS_EQ_STR(words, "P25 emergency.");
    /* A pager match keeps the word, never the page. */
    det_signal_t g = {.source = DET_POCSAG, .a = 1234};
    snprintf(g.text, sizeof(g.text), "Structure Fire at 12 Main St");
    det_signal(d, &g, 5000000);
    LS_EQ_INT(deliveries, 3);
    LS_EQ_STR(last.text, "FIRE");
    LS_CHECK(!strstr(last.text, "Main"));
    det_signal_t r = {.source = DET_REC, .a = 433920000, .b = 4, .flag = 0};
    det_signal(d, &r, 7000000);
    LS_EQ_INT(deliveries, 3);                          /* not a new pattern */
    r.flag = 1;
    det_signal(d, &r, 9000000);
    LS_EQ_INT(deliveries, 4);
    r.a = 868000000; r.b = 5;
    det_signal(d, &r, 11000000);
    LS_EQ_INT(deliveries, 4);                          /* out of the band */
    det_signal_t m = {.source = DET_MESH_PEER, .kind = 0};
    snprintf(m.who, sizeof(m.who), "ABCDEF0123456789");
    det_signal(d, &m, 13000000);
    LS_EQ_INT(deliveries, 4);
    m.kind = 1;
    det_signal(d, &m, 15000000);
    LS_EQ_INT(deliveries, 5);
}

static const char *const BLE_RULES =
    "{\"version\":1,\"rules\":["
    "{\"id\":\"trk\",\"enabled\":true,\"cooldown_s\":60,\"rule_cooldown_s\":10,\"sources\":[\"sweep.ble\"],"
    "\"match\":{\"category_in\":[\"TRACKER\"],\"section_in\":[\"with_you\"],\"min_rssi_dbm\":-80},\"sinks\":[\"vibrate\",\"banner\"]}]}";

LS_CASE(detector_app_cooldowns_windows_and_tokens) {
    fresh(BLE_RULES);
    det_obs_t o = {.entity = 1, .source = DET_BLE, .category = DET_CAT_TRACKER, .section = 0, .rssi = -60};
    det_observe(d, &o, 1000000);
    LS_EQ_INT(deliveries, 1);
    det_observe(d, &o, 2000000);                       /* same entity, in its cooldown */
    LS_EQ_INT(deliveries, 1);
    o.entity = 2;
    det_observe(d, &o, 3000000);                       /* another, in the rule's window */
    LS_EQ_INT(deliveries, 1);
    det_observe(d, &o, 12000000);                      /* window over: it raises, standing for the held ones */
    LS_EQ_INT(deliveries, 2);
    LS_CHECK(last.coalesced >= 1);
    o.entity = 3; o.rssi = -90;
    det_observe(d, &o, 40000000);                      /* too weak */
    LS_EQ_INT(deliveries, 2);
    o.rssi = -60; o.section = 2;
    det_observe(d, &o, 41000000);                      /* passing, not with you */
    LS_EQ_INT(deliveries, 2);
    o.category = DET_CAT_CAMERA; o.section = 0;
    det_observe(d, &o, 42000000);
    LS_EQ_INT(deliveries, 2);
    /* The global bucket: four at once, then one every 2 s. */
    fresh("{\"version\":1,\"rules\":[{\"id\":\"all\",\"enabled\":true,\"cooldown_s\":0,\"rule_cooldown_s\":0,"
          "\"sources\":[\"sweep.ble\"],\"match\":{\"category_in\":[\"CAMERA\"]},\"sinks\":[\"banner\"]}]}");
    det_obs_t a = {.source = DET_BLE, .category = 0, .section = 0, .rssi = -50};
    for (int i = 0; i < 8; i++) { a.entity = 100 + (uint64_t)i; det_observe(d, &a, 50000000); }
    LS_EQ_INT(deliveries, 4);
    a.entity = 200;
    det_observe(d, &a, 52000000);
    LS_EQ_INT(deliveries, 5);
}

LS_CASE(detector_app_quiet_hours_hold_noisy_sinks) {
    LS_CHECK(det_quiet_at(true, 22, 7, 0, 1735689600 + 23 * 3600));     /* 23:00 UTC */
    LS_CHECK(!det_quiet_at(true, 22, 7, 0, 1735689600 + 12 * 3600));
    LS_CHECK(det_quiet_at(true, 22, 7, 0, 0));                          /* no clock: quiet */
    LS_CHECK(!det_quiet_at(false, 22, 7, 0, 0));
    fresh(BLE_RULES);
    d->prefs.quiet_on = true;
    d->utc = 1735689600 + 23 * 3600;
    det_obs_t o = {.entity = 1, .source = DET_BLE, .category = DET_CAT_TRACKER, .section = 0, .rssi = -60};
    det_observe(d, &o, 1000000);
    LS_EQ_INT(deliveries, 1);
    LS_EQ_INT(last_sinks, DET_BANNER);                 /* vibration held back */
    LS_CHECK(last.quiet);
}

LS_CASE(detector_app_aircraft) {
    fresh(NULL);
    det_air_t a = {.icao = 0xABC123, .squawk = 7700, .category = -1, .altitude_ft = 3000, .distance_x10_km = DET_NO_DISTANCE};
    snprintf(a.callsign, sizeof(a.callsign), "UAL123");
    det_air(d, &a, 1000000);
    LS_EQ_INT(deliveries, 1);
    LS_EQ_STR(last.reason, "air-emergency-squawk");
    char t[64];
    det_air_event_text(&last, t, sizeof(t));
    LS_EQ_STR(t, "UAL123 sq7700 3000ft");
    det_air(d, &a, 1500000);                           /* once a second */
    det_air(d, &a, 3000000);                           /* same squawk: in its cooldown */
    LS_EQ_INT(deliveries, 1);
    a.squawk = 7600;
    det_air(d, &a, 5000000);                           /* a new squawk is a new alert */
    LS_EQ_INT(deliveries, 2);
    LS_EQ_INT(det_air_category_bit("A7"), 6);           /* rotorcraft */
    LS_EQ_INT(det_air_category_bit("B6"), 11);          /* uav */
    LS_EQ_INT(det_air_category_bit("A0"), -1);
    LS_EQ_INT(det_air_category_bit(""), -1);
}

LS_CASE(detector_app_drone_watch_builtin_rule) {
    fresh(NULL);
    det_obs_t o = {.entity = 7, .source = DET_DRONE_WATCH, .rssi = -60, .validated = true, .band = 915};
    det_drone_watch(d, &o, 10000000);
    LS_EQ_INT(deliveries, 1);
    LS_EQ_INT(last.builtin, 1);
    LS_EQ_INT(last_sinks, DET_BANNER | DET_VIBRATE);
    o.entity = 8;
    det_drone_watch(d, &o, 10500000);                  /* the built-in rule's 2 s window */
    LS_EQ_INT(deliveries, 1);
    o.approaching = true;
    det_drone_watch(d, &o, 13000000);
    LS_EQ_INT(deliveries, 2);
    LS_CHECK(strstr(last.reason, "approaching") != NULL);
    /* A drone.watch rule in the file replaces the built-in one. */
    fresh("{\"version\":1,\"rules\":[{\"id\":\"dw\",\"enabled\":false,\"sources\":[\"drone.watch\"],\"match\":{\"min_rssi_dbm\":-100},\"sinks\":[\"banner\"]}]}");
    det_drone_watch(d, &o, 20000000);
    LS_EQ_INT(deliveries, 0);
}

LS_CASE(detector_app_mesh_alert_line) {
    det_alert_t m = {.seq = 0x12340001, .source = 9, .flags = DET_ALERT_F_EMERG, .has_pos = true, .lat_e4 = 436532, .lon_e4 = -793832};
    memcpy(m.origin, "A1B2C3D4", 8);
    snprintf(m.summary, sizeof(m.summary), "P25 TG 4512 unit 7 EMERGENCY|x");
    char line[160];
    LS_CHECK(det_alert_encode(&m, line, sizeof(line)) > 0);
    LS_EQ_STR(line, "#LSA/1|A1B2C3D4|12340001|9|E|436532,-793832|P25 TG 4512 unit 7 EMERGENCY/x");
    det_alert_t back;
    LS_EQ_INT(det_alert_decode(line, &back), DET_ALERT_OK);
    LS_EQ_INT(back.source, 9);
    LS_CHECK(back.has_pos && back.lat_e4 == 436532 && back.lon_e4 == -793832);
    LS_EQ_INT(det_alert_decode("#LSA/2|whatever", &back), DET_ALERT_NEWER);
    LS_EQ_INT(det_alert_decode("#LSA/1|nothex!!|12340001|9||| x", &back), DET_ALERT_BAD);
    LS_EQ_INT(det_alert_decode("hello", &back), DET_ALERT_NOT_ALERT);
    det_alert_gate_t g;
    memset(&g, 0, sizeof(g));
    LS_EQ_INT(det_alert_gate(&g, 1, 1000000), DET_ALERT_GO);
    LS_EQ_INT(det_alert_gate(&g, 2, 2000000), DET_ALERT_WAIT);
    LS_EQ_INT(det_alert_gate(&g, 1, 70000000), DET_ALERT_REPEAT);
    LS_EQ_INT(det_alert_gate(&g, 2, 70000000), DET_ALERT_GO);
    det_alert_seen_t seen;
    memset(&seen, 0, sizeof(seen));
    LS_CHECK(!det_alert_seen(&seen, 5, 6));
    LS_CHECK(det_alert_seen(&seen, 5, 6));
    /* A received alert no rule takes raises by itself and is never shared again. */
    fresh(NULL);
    d->sinks |= DET_MESH;
    d->prefs.mesh_rx_sinks = DET_BANNER;
    det_signal_t s = {.source = DET_MESH_ALERT, .kind = 9, .flag = 1, .a = 1, .distance_x10_km = DET_NO_DISTANCE};
    snprintf(s.text, sizeof(s.text), "P25 TG 4512 EMERGENCY");
    det_signal(d, &s, 1000000);
    LS_EQ_INT(deliveries, 1);
    LS_EQ_INT(last.builtin, 2);
    LS_EQ_INT(last_sinks & DET_MESH, 0);
    char words[96];
    det_speech(&last, words, sizeof(words));
    LS_EQ_STR(words, "Mesh alert from a node. P25, emergency.");
}

LS_CASE(detector_app_log_line_has_no_address) {
    fresh(BLE_RULES);
    det_obs_t o = {.entity = UINT64_C(0x1122334455667788), .source = DET_BLE, .category = DET_CAT_TRACKER, .section = 0, .rssi = -60};
    det_observe(d, &o, 1000000);
    LS_EQ_INT(deliveries, 1);
    char line[1024];
    LS_CHECK(det_log_line(d, &last, 0x0102030405060708ull, line, sizeof(line)) > 0);
    LS_CHECK(strstr(line, "\"rule_ids\":[\"trk\"]") != NULL);
    LS_CHECK(strstr(line, "\"entity\":\"1122334455667788\"") != NULL);
    LS_CHECK(strstr(line, "\"source\":\"sweep.ble\"") != NULL);
    LS_CHECK(line[strlen(line) - 1] == '\n');
}

LS_CASE(detector_app_seen_mask) {
    fresh(NULL);
    const uint32_t m = det_seen_mask(d);
    LS_CHECK(m & DET_RID && m & DET_BLE && m & DET_DRONE_WATCH && m & DET_P25);
    LS_CHECK(!(m & DET_LORA));                         /* the LoRa listener only for a LoRa rule */
    LS_CHECK(!(m & (DET_ADSB | DET_MESH_PEER | DET_MESH_ALERT)));   /* the app reads those itself */
    d->rules[find("drone-link-example")].enabled = true;
    LS_CHECK(det_seen_mask(d) & DET_LORA);
}
