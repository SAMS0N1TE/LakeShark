/* lap_dot11 (app-sdk/lib) and the WIFI WATCH engine (app-sdk/apps/wifi_watch)
   on frames made up here: headers, elements, security suites, EAPOL-Key
   messages, and every alert the engine raises, with the thresholds that
   must not fire on ordinary traffic. */

#include "ls_test.h"
#include "watch.h"

#include <stdio.h>
#include <string.h>

static const uint8_t AP[6] = {0x02, 0xaa, 0, 0, 0, 1}, STA[6] = {0x02, 0xbb, 0, 0, 0, 0x10}, BC[6] = {255, 255, 255, 255, 255, 255};
static uint8_t f[512];

static size_t hdr(uint8_t fc0, uint8_t fc1, const uint8_t *a1, const uint8_t *a2, const uint8_t *a3)
{
    memset(f, 0, sizeof(f));
    f[0] = fc0; f[1] = fc1;
    memcpy(f + 4, a1, 6); memcpy(f + 10, a2, 6); memcpy(f + 16, a3, 6);
    return 24;
}
static size_t ie(size_t at, uint8_t id, const void *v, size_t n)
{
    f[at] = id; f[at + 1] = (uint8_t)n;
    memcpy(f + at + 2, v, n);
    return at + 2 + n;
}
/* rsn: 0 none, 1 PSK, 2 SAE, 3 PSK+SAE, 4 802.1X */
static size_t beacon(const uint8_t *bssid, const char *ssid, int ch, int rsn, bool hidden, bool probe_resp)
{
    size_t at = hdr(probe_resp ? 0x50 : 0x80, 0, probe_resp ? STA : BC, bssid, bssid);
    f[at + 8] = 100; f[at + 10] = (uint8_t)(rsn ? 0x11 : 0x01);
    at += 12;
    uint8_t name[32] = {0};
    const size_t n = strlen(ssid);
    if (!hidden) memcpy(name, ssid, n);
    at = ie(at, 0, name, n);
    const uint8_t c = (uint8_t)ch;
    at = ie(at, 3, &c, 1);
    if (rsn) {
        uint8_t r[32] = {1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4};
        size_t k = 12;
        const int akms = rsn == 3 ? 2 : 1;
        r[k++] = (uint8_t)akms; r[k++] = 0;
        if (rsn == 1 || rsn == 3) { r[k++] = 0; r[k++] = 0x0f; r[k++] = 0xac; r[k++] = 2; }
        if (rsn == 2 || rsn == 3) { r[k++] = 0; r[k++] = 0x0f; r[k++] = 0xac; r[k++] = 8; }
        if (rsn == 4) { r[k++] = 0; r[k++] = 0x0f; r[k++] = 0xac; r[k++] = 1; }
        r[k++] = 0; r[k++] = 0;
        at = ie(at, 48, r, k);
    }
    return at;
}
static size_t eapol(int msg, bool pmkid)
{
    static const uint16_t info[5] = {0, 0x008A, 0x010A, 0x13CA, 0x030A};
    size_t at = msg == 1 || msg == 3 ? hdr(0x88, 0x02, STA, AP, AP) : hdr(0x88, 0x01, AP, STA, AP);
    at += 2;                                            /* QoS control */
    static const uint8_t llc[8] = {0xaa, 0xaa, 3, 0, 0, 0, 0x88, 0x8e};
    memcpy(f + at, llc, 8); at += 8;
    const size_t eap = at;
    f[at] = 2; f[at + 1] = 3;                           /* version, key */
    at += 4;
    f[at] = 2; f[at + 1] = (uint8_t)(info[msg] >> 8); f[at + 2] = (uint8_t)info[msg];
    at += 1 + 2 + 2 + 8 + 32 + 16 + 8 + 8 + 16;
    const size_t kdl = at;
    at += 2;
    if (pmkid) {
        static const uint8_t kde[22] = {0xdd, 20, 0x00, 0x0f, 0xac, 4, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        memcpy(f + at, kde, sizeof(kde)); at += sizeof(kde);
    }
    f[kdl] = 0; f[kdl + 1] = (uint8_t)(at - kdl - 2);
    f[eap + 2] = (uint8_t)((at - eap - 4) >> 8); f[eap + 3] = (uint8_t)(at - eap - 4);
    return at;
}
static size_t kick(const uint8_t *src, const uint8_t *dst, uint16_t reason, bool dis)
{
    size_t at = hdr(dis ? 0xA0 : 0xC0, 0, dst, src, src);
    f[at] = (uint8_t)reason; f[at + 1] = (uint8_t)(reason >> 8);
    return at + 2;
}

LS_CASE(dot11_beacon_elements_and_security)
{
    lap_dot11_t d;
    LS_CHECK(lap_dot11_parse(f, beacon(AP, "TEST-LakeNet", 6, 1, false, false), &d));
    LS_EQ_INT(d.type, LAP_DOT11_MGMT);
    LS_EQ_INT(d.subtype, LAP_DOT11_BEACON);
    LS_EQ_STR(d.ssid, "TEST-LakeNet");
    LS_EQ_INT(d.channel, 6);
    LS_EQ_INT(d.security, LAP_DOT11_SEC_WPA2);
    LS_CHECK(!memcmp(d.bssid, AP, 6) && d.have_bssid);
    LS_EQ_INT(d.interval, 100);
    lap_dot11_parse(f, beacon(AP, "x", 1, 2, false, false), &d);
    LS_EQ_INT(d.security, LAP_DOT11_SEC_WPA3);
    lap_dot11_parse(f, beacon(AP, "x", 1, 3, false, false), &d);
    LS_EQ_INT(d.security, LAP_DOT11_SEC_WPA2_WPA3);
    lap_dot11_parse(f, beacon(AP, "x", 1, 4, false, false), &d);
    LS_EQ_INT(d.security, LAP_DOT11_SEC_WPA2_EAP);
    lap_dot11_parse(f, beacon(AP, "x", 1, 0, false, false), &d);
    LS_EQ_INT(d.security, LAP_DOT11_SEC_OPEN);
    lap_dot11_parse(f, beacon(AP, "TEST-Secret", 11, 1, true, false), &d);
    LS_CHECK(d.ssid_present && d.ssid_hidden);
    LS_EQ_INT(d.ssid_len, 11);
    LS_EQ_STR(d.ssid, "");
    LS_EQ_STR(lap_dot11_sec_name(LAP_DOT11_SEC_WPA2_WPA3), "WPA2/3");
}

LS_CASE(dot11_short_cut_and_odd_frames)
{
    lap_dot11_t d;
    LS_CHECK(!lap_dot11_parse(f, 9, &d));
    const size_t n = beacon(AP, "TEST-LakeNet", 6, 1, false, false);
    for (size_t cut = 10; cut < n; cut++) lap_dot11_parse(f, cut, &d);   /* never reads past the end (ASan) */
    /* an element that claims more than the frame holds stops the walk */
    beacon(AP, "TEST-LakeNet", 6, 1, false, false);
    f[24 + 12 + 1] = 200;
    LS_CHECK(lap_dot11_parse(f, n, &d));
    LS_CHECK(!d.ssid_present);
    /* control frames: ACK has one address, RTS two */
    memset(f, 0, 16); f[0] = 0xD4;
    LS_CHECK(lap_dot11_parse(f, 10, &d) && d.type == LAP_DOT11_CTRL && d.naddr == 1 && !d.have_ta);
    f[0] = 0xB4;
    LS_CHECK(lap_dot11_parse(f, 16, &d) && d.naddr == 2 && d.have_ta);
    /* a protected deauthentication keeps its reason sealed */
    kick(AP, STA, 7, false);
    f[1] = 0x40;
    lap_dot11_parse(f, 26, &d);
    LS_EQ_INT(d.reason, 0);
    LS_CHECK(d.protect);
}

LS_CASE(dot11_eapol_messages_and_pmkid_presence)
{
    lap_dot11_t d;
    for (int m = 1; m <= 4; m++) {
        const size_t n = eapol(m, m == 1);
        LS_CHECK(lap_dot11_parse(f, n, &d));
        LS_CHECK(d.eapol);
        LS_EQ_INT(d.eapol_type, 3);
        LS_EQ_INT(d.eapol_msg, m);
        LS_EQ_INT(d.eapol_pmkid, m == 1);
        LS_CHECK(!memcmp(d.bssid, AP, 6));
    }
    /* an all-zero PMKID is not one */
    const size_t n = eapol(1, true);
    memset(f + n - 16, 0, 16);
    lap_dot11_parse(f, n, &d);
    LS_CHECK(!d.eapol_pmkid);
    /* a protected data frame is never opened */
    eapol(2, false);
    f[1] |= 0x40;
    lap_dot11_parse(f, n, &d);
    LS_CHECK(!d.eapol);
}

LS_CASE(dot11_addresses_channels_airtime)
{
    const uint8_t phone[6] = {0xda, 0xa1, 0x19, 0, 0, 1}, laptop[6] = {0x00, 0x1b, 0x63, 0, 0, 2};
    LS_CHECK(lap_dot11_is_random(phone));
    LS_CHECK(!lap_dot11_is_random(laptop));
    LS_CHECK(!lap_dot11_is_random(BC));
    LS_CHECK(lap_dot11_is_broadcast(BC));
    LS_EQ_INT(lap_dot11_channel_mhz(1), 2412);
    LS_EQ_INT(lap_dot11_channel_mhz(14), 2484);
    LS_EQ_INT(lap_dot11_channel_mhz(36), 5180);
    LS_EQ_INT(lap_dot11_mhz_channel(2437), 6);
    LS_EQ_INT(lap_dot11_mhz_channel(5745), 149);
    LS_EQ_INT(lap_dot11_mhz_channel(2400), 0);
    /* 100 bytes at 1 Mb/s: 192 us preamble + 104 bytes * 8 us */
    LS_EQ_INT(lap_dot11_airtime_us(100, 2, 6), 192 + 832);
    /* the same at 54 Mb/s OFDM: 20 us + 4 us per 216-bit symbol */
    LS_EQ_INT(lap_dot11_airtime_us(100, 108, 36), 20 + 4 * ((16 + 832 + 6 + 215) / 216));
    char m[18];
    lap_dot11_mac(m, sizeof(m), AP);
    LS_EQ_STR(m, "02:aa:00:00:00:01");
}

static watch_t w;
static int feed(size_t n, uint32_t ms) { return watch_frame(&w, f, n, -50, 6, ms); }
static bool raised(int kind)
{
    for (uint32_t i = 0; i < watch_alerts_kept(&w); i++) if (watch_alert(&w, i)->kind == kind) return true;
    return false;
}

LS_CASE(watch_quiet_traffic_raises_nothing)
{
    watch_init(&w);
    for (int i = 0; i < 200; i++) {
        feed(beacon(AP, "TEST-LakeNet", 6, 1, false, false), 1000u + (uint32_t)i * 102u);
        if (i % 20 == 0) feed(kick(AP, STA, 3, false), 1000u + (uint32_t)i * 102u);   /* a station leaving now and then */
    }
    LS_EQ_UINT(w.nalerts, 0);
    LS_EQ_UINT(w.beacons, 200);
    LS_EQ_UINT(w.deauth, 10);
    LS_EQ_INT(w.naps, 1);
}

LS_CASE(watch_deauth_burst_once_per_window)
{
    watch_init(&w);
    for (int i = 0; i < 40; i++) feed(kick(AP, BC, 7, false), 1000u + (uint32_t)i * 20u);
    LS_EQ_UINT(w.by_kind[K_DEAUTH], 1);                 /* 40 frames, one alert: it repeats after 10 s */
    const watch_alert_t *a = watch_alert(&w, 0);
    LS_EQ_STR(a->who, "02:aa:00:00:00:01");
    LS_EQ_STR(a->whom, "broadcast");
    LS_CHECK(strstr(a->text, "reason 7") != NULL);
    for (int i = 0; i < 12; i++) feed(kick(AP, BC, 7, false), 12000u + (uint32_t)i * 20u);
    LS_EQ_UINT(w.by_kind[K_DEAUTH], 2);
    for (int i = 0; i < 12; i++) feed(kick(AP, STA, 8, true), 13000u + (uint32_t)i * 20u);
    LS_EQ_UINT(w.by_kind[K_DISASSOC], 1);
    LS_EQ_INT(w.npairs, 2);
}

LS_CASE(watch_flood_twin_hidden_handshake)
{
    watch_init(&w);
    feed(beacon(AP, "TEST-LakeNet", 6, 1, false, false), 1000);
    const uint8_t hid[6] = {0x02, 0xaa, 0, 0, 0, 3}, twin[6] = {0x02, 0xee, 0, 0, 0, 0x66};
    feed(beacon(hid, "TEST-Secret", 6, 1, true, false), 1010);
    LS_EQ_UINT(w.nalerts, 0);
    feed(beacon(hid, "TEST-Secret", 6, 1, false, true), 1500);
    LS_CHECK(raised(K_HIDDEN));
    feed(beacon(twin, "TEST-LakeNet", 6, 0, false, false), 2000);
    LS_CHECK(raised(K_TWIN));
    LS_CHECK(strstr(watch_alert(&w, 0)->text, "open here, WPA2") != NULL);
    feed(beacon(twin, "TEST-LakeNet", 6, 0, false, false), 2100);
    LS_EQ_UINT(w.by_kind[K_TWIN], 1);                   /* once */
    for (int m = 1; m <= 4; m++) feed(eapol(m, m == 1), 3000u + (uint32_t)m);
    LS_EQ_UINT(w.by_kind[K_PMKID], 1);
    LS_EQ_UINT(w.by_kind[K_HANDSHAKE], 1);
    LS_EQ_INT(w.shake[0].msgs, 0x0F);
    /* the alert text names the event, never the key data */
    for (uint32_t i = 0; i < watch_alerts_kept(&w); i++) LS_CHECK(strstr(watch_alert(&w, i)->text, "0102") == NULL);
    uint8_t b[6] = {0x02, 0xf1, 0, 0, 0, 0};
    for (int i = 0; i < 40; i++) { b[5] = (uint8_t)i; feed(beacon(b, "TEST-Flood", 6, 0, false, false), 5000u + (uint32_t)i * 40u); }
    LS_EQ_UINT(w.by_kind[K_FLOOD], 1);
}

LS_CASE(watch_tables_stay_bounded)
{
    watch_init(&w);
    uint8_t b[6] = {0x02, 0xf2, 0, 0, 0, 0}, c[6] = {0xda, 0, 0, 0, 0, 0};
    for (int i = 0; i < 1000; i++) {
        b[4] = (uint8_t)(i >> 8); b[5] = (uint8_t)i;
        feed(beacon(b, "TEST-Many", 1 + i % 11, i & 1, false, false), 1000u + (uint32_t)i * 10u);
        c[4] = (uint8_t)(i >> 8); c[5] = (uint8_t)i;
        size_t at = hdr(0x40, 0, BC, c, BC);
        at = ie(at, 0, "TEST-Probe", 10);
        feed(at, 1000u + (uint32_t)i * 10u);
        feed(kick(b, c, 7, i & 1), 1000u + (uint32_t)i * 10u);
    }
    LS_EQ_INT(w.naps, W_APS);
    LS_EQ_INT(w.ncl, W_CLIENTS);
    LS_EQ_INT(w.npairs, W_PAIRS);
    LS_EQ_UINT(w.aps_over, 1000 - W_APS);
    LS_CHECK(watch_alerts_kept(&w) <= W_ALERTS);
    LS_CHECK(w.cl[0].random);
}

/* ---- alert delivery: the mesh relay, the notice, the saved choices ---- */

static watch_alert_t mk(int kind, int sev, uint32_t ms)
{
    watch_alert_t a;
    memset(&a, 0, sizeof(a));
    a.kind = (uint8_t)kind; a.sev = (uint8_t)sev; a.ms = ms; a.channel = 6;
    snprintf(a.who, sizeof(a.who), "02:aa:00:00:00:01");
    snprintf(a.whom, sizeof(a.whom), "broadcast");
    snprintf(a.text, sizeof(a.text), "Deauth burst to broadcast: 25 in 480 ms");
    return a;
}

LS_CASE(relay_first_goes_at_once_repeats_coalesce)
{
    static watch_relay_t r;
    watch_relay_init(&r, SEV_ALERT, 10, 6);
    watch_alert_t a = mk(K_DEAUTH, SEV_ALERT, 1000);
    LS_CHECK(watch_relay_note(&r, &a));
    LS_EQ_INT(watch_relay_next(&r, 1000), K_DEAUTH);    /* a new kind goes at once */
    watch_relay_sent(&r, K_DEAUTH, 1000);
    LS_EQ_INT(watch_relay_next(&r, 1001), -1);
    /* a flood: eleven more inside the ten minutes are held and counted */
    for (int i = 0; i < 11; i++) { a = mk(K_DEAUTH, SEV_ALERT, 2000u + (uint32_t)i * 10000u); LS_CHECK(watch_relay_note(&r, &a)); }
    LS_EQ_INT(watch_relay_next(&r, 200000), -1);
    LS_EQ_UINT(watch_relay_waiting(&r), 11);
    LS_EQ_UINT(r.coalesced, 10);
    LS_EQ_UINT(watch_relay_wait_ms(&r, 200000), 600000u + 1000u - 200000u);
    LS_EQ_INT(watch_relay_next(&r, 601000), K_DEAUTH);   /* the gap is over: one notice for all */
    LS_EQ_UINT(r.k[K_DEAUTH].pending, 11);
    LS_EQ_UINT(r.k[K_DEAUTH].latest.ms, 2000u + 10u * 10000u);   /* the newest stands for them */
    watch_relay_sent(&r, K_DEAUTH, 601000);
    LS_EQ_UINT(watch_relay_waiting(&r), 0);
    LS_EQ_UINT(r.nsent, 2);
}

LS_CASE(relay_severity_kinds_and_hourly_cap)
{
    static watch_relay_t r;
    watch_relay_init(&r, SEV_WARN, 1, 3);
    watch_alert_t a = mk(K_HIDDEN, SEV_INFO, 10);
    LS_CHECK(!watch_relay_note(&r, &a));                 /* below the chosen severity */
    LS_EQ_INT(watch_relay_next(&r, 10), -1);
    /* each kind has its own gap; the oldest waiting goes first */
    a = mk(K_TWIN, SEV_ALERT, 20); watch_relay_note(&r, &a);
    a = mk(K_PMKID, SEV_WARN, 30); watch_relay_note(&r, &a);
    a = mk(K_FLOOD, SEV_ALERT, 40); watch_relay_note(&r, &a);
    a = mk(K_DEAUTH, SEV_ALERT, 50); watch_relay_note(&r, &a);
    int k = watch_relay_next(&r, 100); LS_EQ_INT(k, K_TWIN); watch_relay_sent(&r, k, 100);
    k = watch_relay_next(&r, 100); LS_EQ_INT(k, K_PMKID); watch_relay_sent(&r, k, 100);
    k = watch_relay_next(&r, 100); LS_EQ_INT(k, K_FLOOD); watch_relay_sent(&r, k, 100);
    /* three in the hour: DEAUTH waits for the cap, not for its own gap */
    LS_EQ_INT(watch_relay_next(&r, 100), -1);
    LS_EQ_INT(watch_relay_next(&r, 3000000), -1);
    LS_EQ_UINT(watch_relay_wait_ms(&r, 3000000), 3600100u - 3000000u);
    LS_EQ_INT(watch_relay_next(&r, 3600100), K_DEAUTH);
}

LS_CASE(relay_flood_stays_bounded)
{
    static watch_relay_t r;
    watch_relay_init(&r, SEV_ALERT, 1, 30);
    uint32_t sent = 0;
    /* an hour of deauthentication bursts, one a second, from many pairs */
    for (uint32_t t = 0; t < 3600u * 1000u; t += 1000u) {
        watch_alert_t a = mk(t % 3000u ? K_DEAUTH : K_DISASSOC, SEV_ALERT, t);
        watch_relay_note(&r, &a);
        const int k = watch_relay_next(&r, t);
        if (k >= 0) { watch_relay_sent(&r, k, t); sent++; }
    }
    LS_CHECK(sent <= 30);                                /* the hour's cap */
    LS_CHECK(sent >= 20);                                /* and each kind still speaks every minute or so */
    LS_EQ_UINT(r.noted, 3600);
}

LS_CASE(mesh_text_is_short_and_plain)
{
    char t[160];
    watch_alert_t a = mk(K_DEAUTH, SEV_ALERT, 0);
    watch_mesh_text(t, sizeof(t), &a, NULL, 12, "14:03");
    LS_EQ_STR(t, "WIFI WATCH DEAUTH x12 02:aa:00:00:00:01>bcast ch6 14:03");
    watch_mesh_text(t, sizeof(t), &a, NULL, 1, "14:03");
    LS_EQ_STR(t, "WIFI WATCH DEAUTH 02:aa:00:00:00:01>bcast ch6 14:03");
    a.kind = K_TWIN;
    snprintf(a.whom, sizeof(a.whom), "02:aa:00:00:00:01");
    snprintf(a.text, sizeof(a.text), "Evil twin? \"TEST-LakeNet-With-A-Very-Long-Name\" open here, WPA2 on 02:aa:00:00:00:01");
    watch_mesh_text(t, sizeof(t), &a, NULL, 1, "09:15");
    LS_EQ_STR(t, "WIFI WATCH TWIN \"TEST-LakeNet-With-A-\" 02:aa:00:00:00:01>..00:00:01 ch6 09:15");
    watch_mesh_text(t, sizeof(t), NULL, "TEST", 1, "12:00");
    LS_EQ_STR(t, "WIFI WATCH TEST 12:00");
    /* the longest notice fits a mesh message (150 bytes) */
    memset(a.text, 'n', sizeof(a.text) - 1); a.text[0] = '"'; a.text[40] = '"'; a.text[sizeof(a.text) - 1] = 0;
    a.kind = K_HANDSHAKE; a.channel = 13;
    LS_CHECK(watch_mesh_text(t, sizeof(t), &a, NULL, 65535, "23:59") <= 150);
}

LS_CASE(prefs_default_off_and_round_trip)
{
    watch_prefs_t p, q;
    watch_prefs_default(&p);
    LS_EQ_INT(p.mesh_to, MESH_OFF);                      /* never on air unless asked */
    LS_EQ_INT(p.out[SEV_INFO], OUT_NONE);
    p.mesh_to = MESH_DM; p.mesh_gap_min = 5; p.mesh_per_hour = 12; p.out[SEV_WARN] = OUT_VIBRATE; p.speak = 1;
    p.banner = BANNER_BOARD; p.quiet_on = 1; p.quiet_from = 23; p.quiet_until = 6;
    snprintf(p.dm_id, sizeof(p.dm_id), "00000000A1B2C3D5"); snprintf(p.dm_name, sizeof(p.dm_name), "KD9XYZ");
    LS_CHECK(watch_prefs_load(&q, &p, sizeof(p)));
    LS_CHECK(!memcmp(&p, &q, sizeof(p)));
    /* not ours, or the wrong size: the defaults */
    LS_CHECK(!watch_prefs_load(&q, &p, sizeof(p) - 1));
    LS_EQ_INT(q.mesh_to, MESH_OFF);
    /* out of range comes back in range; a DM without a node id is off */
    p.mesh_gap_min = 9999; p.mesh_per_hour = 0; p.out[0] = 9; p.mesh_to = MESH_DM; p.dm_id[3] = 'z';
    LS_CHECK(watch_prefs_load(&q, &p, sizeof(p)));
    LS_EQ_INT(q.mesh_gap_min, WATCH_GAP_MIN_MAX);
    LS_EQ_INT(q.mesh_per_hour, 1);
    LS_EQ_INT(q.out[0], OUT_NONE);
    LS_EQ_INT(q.mesh_to, MESH_OFF);
}

LS_CASE(quiet_hours_across_midnight)
{
    watch_prefs_t p;
    watch_prefs_default(&p);
    LS_CHECK(!watch_quiet(&p, 23));                       /* off by default */
    p.quiet_on = 1; p.quiet_from = 22; p.quiet_until = 7;
    LS_CHECK(watch_quiet(&p, 22)); LS_CHECK(watch_quiet(&p, 0)); LS_CHECK(watch_quiet(&p, 6));
    LS_CHECK(!watch_quiet(&p, 7)); LS_CHECK(!watch_quiet(&p, 12)); LS_CHECK(!watch_quiet(&p, 21));
    p.quiet_from = 9; p.quiet_until = 17;
    LS_CHECK(watch_quiet(&p, 9)); LS_CHECK(!watch_quiet(&p, 17)); LS_CHECK(!watch_quiet(&p, 3));
    p.quiet_until = 9;
    LS_CHECK(watch_quiet(&p, 3));                         /* from == until: all day */
}
