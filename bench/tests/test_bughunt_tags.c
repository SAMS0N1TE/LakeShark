/* BUGHUNT's tag watch: its engine (app-sdk/apps/bughunt/bh_tags.c, which was
   the BLE WATCH app's) as the app builds it. Every advert here is made up in
   the test: each tracker kind is named, malformed AD is survived, rotating
   addresses are linked only when the old one goes quiet, presence windows
   count, the following alert waits for time (and for movement when there is
   a fix), and the CSV line. */
#include "ls_test.h"
#include "bh_tags.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bw_t *w;
static void fresh(void)
{
    if (!w) w = malloc(sizeof(*w));
    bw_init(w);
}

/* AD builder */
typedef struct { uint8_t b[64]; size_t n; } ad_t;
static void put(ad_t *a, uint8_t type, const uint8_t *v, size_t n)
{
    a->b[a->n++] = (uint8_t)(n + 1);
    a->b[a->n++] = type;
    memcpy(a->b + a->n, v, n);
    a->n += n;
}
static ad_t findmy(uint8_t status, uint8_t key, bool separated)
{
    ad_t a = {0};
    uint8_t v[29] = {0x4C, 0x00, 0x12, separated ? 25 : 2, status};
    if (separated) { for (int i = 0; i < 22; i++) v[5 + i] = (uint8_t)(key + i); v[27] = 1; v[28] = 0; }
    else v[5] = 1;
    put(&a, 0xFF, v, separated ? 29 : 6);
    return a;
}
static ad_t svc_data(uint16_t uuid, const uint8_t *d, size_t n)
{
    ad_t a = {0};
    const uint8_t f = 6;
    put(&a, 0x01, &f, 1);
    uint8_t v[32] = {(uint8_t)uuid, (uint8_t)(uuid >> 8)};
    memcpy(v + 2, d, n);
    put(&a, 0x16, v, n + 2);
    return a;
}
static const uint8_t A1[6] = {0x13, 0x9e, 0x20, 0x71, 0x5a, 0xc4}, A2[6] = {0x8a, 0x31, 0x66, 0x4b, 0x02, 0xd9},
                     A3[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0xc6}, P1[6] = {0x13, 0x71, 0xda, 0x7d, 0x1a, 0x00};

LS_CASE(bughunt_tags_names_each_tracker_kind) {
    bw_ad_t o;
    ad_t a = findmy(0x10, 0x31, true);
    LS_CHECK(bw_parse(a.b, a.n, &o));
    LS_EQ_INT(o.cls, BW_C_FINDMY);
    LS_EQ_INT(o.level, 2);
    LS_EQ_INT(o.separated, 1);
    LS_EQ_INT(o.company, 0x004C);
    a = findmy(0x00, 0, false);
    LS_CHECK(bw_parse(a.b, a.n, &o));
    LS_EQ_INT(o.cls, BW_C_FINDMY);
    LS_EQ_INT(o.separated, 0);

    const uint8_t tile[] = {2, 1, 0xa3, 0xb2, 0xc1, 0xd0, 0xe9, 0xf8};
    a = svc_data(0xFEED, tile, sizeof(tile));
    LS_CHECK(bw_parse(a.b, a.n, &o));
    LS_EQ_INT(o.cls, BW_C_TILE);
    ad_t b = {0};
    const uint8_t list[] = {0xEC, 0xFE};
    put(&b, 0x03, list, 2);
    LS_CHECK(bw_parse(b.b, b.n, &o));
    LS_EQ_INT(o.cls, BW_C_TILE);

    const uint8_t st[] = {0x13, 0x42, 1, 2, 3};
    a = svc_data(0xFD5A, st, sizeof(st));
    LS_CHECK(bw_parse(a.b, a.n, &o));
    LS_EQ_INT(o.cls, BW_C_SMARTTAG);

    uint8_t fm[21] = {0x41};
    a = svc_data(0xFEAA, fm, sizeof(fm));
    LS_CHECK(bw_parse(a.b, a.n, &o));
    LS_EQ_INT(o.cls, BW_C_FMDN);
    LS_EQ_INT(o.separated, 1);
    fm[0] = 0x40;
    a = svc_data(0xFEAA, fm, sizeof(fm));
    LS_CHECK(bw_parse(a.b, a.n, &o));
    LS_EQ_INT(o.cls, BW_C_FMDN);
    LS_EQ_INT(o.separated, -1);
    const uint8_t url[] = {0x10, 0xEB, 0x03, 'a', 'b'};
    a = svc_data(0xFEAA, url, sizeof(url));
    LS_CHECK(bw_parse(a.b, a.n, &o));
    LS_EQ_INT(o.cls, BW_C_EDDYSTONE);
    LS_EQ_INT(o.level, 0);

    const uint8_t fp[] = {0x0A, 0x1B, 0x2C};
    a = svc_data(0xFE2C, fp, sizeof(fp));
    LS_CHECK(bw_parse(a.b, a.n, &o));
    LS_EQ_INT(o.cls, BW_C_FASTPAIR);
    LS_EQ_INT(o.level, 1);
    LS_EQ_UINT(o.model, 0x0A1B2Cu);

    const uint8_t dult[] = {1, 2, 3};
    a = svc_data(0xFCB2, dult, sizeof(dult));
    LS_CHECK(bw_parse(a.b, a.n, &o));
    LS_EQ_INT(o.cls, BW_C_DULT);
    LS_EQ_INT(o.separated, 1);

    /* A tag that also speaks Fast Pair stays the stronger tracker kind. */
    ad_t both = svc_data(0xFE2C, fp, sizeof(fp));
    uint8_t fv[3] = {0xAA, 0xFE, 0x41};
    put(&both, 0x16, fv, 3);
    LS_CHECK(bw_parse(both.b, both.n, &o));
    LS_EQ_INT(o.cls, BW_C_FMDN);
    LS_EQ_INT(o.n_uuid, 2);

    /* AirPods proximity pairing: possible, with the model named. */
    ad_t pp = {0};
    uint8_t ppv[29] = {0x4C, 0x00, 0x07, 0x19, 0x01, 0x0E, 0x20, 0x55};
    put(&pp, 0xFF, ppv, sizeof(ppv));
    LS_CHECK(bw_parse(pp.b, pp.n, &o));
    LS_EQ_INT(o.cls, BW_C_APPLE_PP);
    LS_EQ_INT(o.level, 1);
    LS_EQ_UINT(o.model, 0x0E20u);

    /* iBeacon and ordinary continuity are not trackers. */
    ad_t ib = {0};
    uint8_t ibv[25] = {0x4C, 0x00, 0x02, 0x15};
    put(&ib, 0xFF, ibv, sizeof(ibv));
    LS_CHECK(bw_parse(ib.b, ib.n, &o));
    LS_EQ_INT(o.cls, BW_C_IBEACON);
    LS_EQ_INT(o.level, 0);
    ad_t nb = {0};
    uint8_t nbv[] = {0x4C, 0x00, 0x10, 0x05, 0x01, 0x18, 0x8C, 0x41, 0x2E};
    put(&nb, 0xFF, nbv, sizeof(nbv));
    LS_CHECK(bw_parse(nb.b, nb.n, &o));
    LS_EQ_INT(o.cls, BW_C_APPLE);
    LS_EQ_INT(o.sub, 0x10);
}

LS_CASE(bughunt_tags_parses_names_flags_and_survives_bad_ad) {
    bw_ad_t o;
    ad_t a = {0};
    const uint8_t f = 0x06, tx = (uint8_t)-12, u[] = {0x0D, 0x18, 0x0F, 0x18}, co[] = {0x87, 0x00, 1};
    put(&a, 0x01, &f, 1);
    put(&a, 0x08, (const uint8_t *)"Fore", 4);
    put(&a, 0x09, (const uint8_t *)"Fore,runner", 11);
    put(&a, 0x0A, &tx, 1);
    put(&a, 0x03, u, 4);
    put(&a, 0xFF, co, 3);
    LS_CHECK(bw_parse(a.b, a.n, &o));
    LS_EQ_STR(o.name, "Fore.runner");                /* the full name wins; commas cannot reach the CSV */
    LS_CHECK(o.has_flags && o.flags == 6);
    LS_CHECK(o.has_tx && o.tx_power == -12);
    LS_EQ_INT(o.n_uuid, 2);
    LS_EQ_INT(o.uuid16[0], 0x180D);
    LS_EQ_INT(o.company, 0x0087);
    LS_EQ_STR(bw_company(0x0087), "Garmin");
    LS_CHECK(bw_company(0xBEEF) == NULL);
    /* a length running past the end: false, and what came before is kept */
    uint8_t bad[] = {2, 1, 6, 9, 0x09, 'a', 'b'};
    LS_CHECK(!bw_parse(bad, sizeof(bad), &o));
    LS_CHECK(o.has_flags);
    /* zero padding ends the structures; an empty advert is fine */
    uint8_t pad[] = {2, 1, 6, 0, 0, 0};
    LS_CHECK(bw_parse(pad, sizeof(pad), &o));
    LS_CHECK(bw_parse(NULL, 0, &o));
    LS_EQ_INT(o.cls, BW_C_NONE);
    /* truncated Apple records are not read past */
    uint8_t trunc[] = {6, 0xFF, 0x4C, 0x00, 0x12, 0x19, 0x10};
    LS_CHECK(bw_parse(trunc, sizeof(trunc), &o));
    LS_EQ_INT(o.cls, BW_C_APPLE);
}

LS_CASE(bughunt_tags_addresses_and_labels) {
    char t[18];
    bw_addr_text(A1, t);
    LS_EQ_STR(t, "c4:5a:71:20:9e:13");
    LS_EQ_STR(bw_addr_kind(A1, 1), "random static");
    LS_EQ_STR(bw_addr_kind(P1, 0), "public");
    const uint8_t rpa[6] = {1, 2, 3, 4, 5, 0x4a}, nrpa[6] = {1, 2, 3, 4, 5, 0x2a};
    LS_EQ_STR(bw_addr_kind(rpa, 1), "resolvable private");
    LS_EQ_STR(bw_addr_kind(nrpa, 1), "non-resolvable");
    fresh();
    ad_t a = findmy(0x10, 0x31, true);
    const int i = bw_add(w, A1, 1, 3, -60, a.b, a.n, 1000);
    char l[32];
    LS_EQ_STR(bw_label(&w->dev[i], l, sizeof(l)), "AirTag?");
    LS_EQ_STR(bw_pdu_name(w->dev[i].pdu), "ADV_NONCONN_IND");
}

LS_CASE(bughunt_tags_table_windows_and_history) {
    fresh();
    ad_t a = findmy(0x10, 0x31, true);
    int i = -1;
    /* every 2 s for 6 minutes, getting stronger */
    for (uint32_t t = 0; t <= 360000; t += 2000) i = bw_add(w, A1, 1, 3, -80 + (int)(t / 20000), a.b, a.n, t);
    const bw_dev_t *d = &w->dev[i];
    LS_EQ_INT(w->n, 1);
    LS_EQ_UINT(d->count, 181u);
    LS_EQ_UINT(bw_span_ms(d), 360000u);
    LS_EQ_INT(bw_window(d, 360000, 1), 1);              /* the minute that has just begun */
    LS_EQ_INT(bw_window(d, 360000, 5), 121);
    LS_EQ_INT(bw_window(d, 360000, 15), 181);
    LS_EQ_UINT(bw_presence(d, 360000), 0x7Fu);           /* minutes 0..6 */
    LS_EQ_INT(bw_trend(d, 360000), 0);                    /* +1 dB per 20 s: steady over 12 s */
    int8_t h[BW_HIST];
    bw_hist(d, 360000, h);
    LS_EQ_INT(h[BW_HIST - 1], -62);
    LS_EQ_INT(h[0], -80 + (int)((360000 - 118000) / 20000));
    bw_hist(d, 360000 + 200000, h);                       /* long after: empty */
    LS_EQ_INT(h[BW_HIST - 1], BW_NO_RSSI);
    LS_EQ_INT(h[0], BW_NO_RSSI);
    /* a sharp climb reads as stronger */
    for (uint32_t t = 362000; t <= 366000; t += 2000) bw_add(w, A1, 1, 3, -40, a.b, a.n, t);
    LS_EQ_INT(bw_trend(&w->dev[i], 366000), 1);
    for (uint32_t t = 368000; t <= 372000; t += 2000) bw_add(w, A1, 1, 3, -75, a.b, a.n, t);
    LS_EQ_INT(bw_trend(&w->dev[i], 372000), -1);
    LS_EQ_INT(d->peak, -40);
}

LS_CASE(bughunt_tags_full_table_lets_the_least_go) {
    fresh();
    ad_t tag = findmy(0x10, 0x31, true), plain = {0};
    const uint8_t f = 6;
    put(&plain, 0x01, &f, 1);
    bw_add(w, A1, 1, 3, -60, tag.b, tag.n, 0);            /* a tracker, oldest of all */
    for (int k = 0; k < BW_MAX_DEV + 10; k++) {
        uint8_t ad[6] = {(uint8_t)k, (uint8_t)(k >> 8), 0, 0, 0, 0x40};
        bw_add(w, ad, 1, 0, -70, plain.b, plain.n, 1000u + (uint32_t)k * 10u);
    }
    LS_EQ_INT(w->n, BW_MAX_DEV);
    LS_EQ_UINT(w->evicted, 11u);
    bool tracker_kept = false;
    for (int k = 0; k < w->n; k++) if (!memcmp(w->dev[k].addr, A1, 6)) tracker_kept = true;
    LS_CHECK(tracker_kept);
}

LS_CASE(bughunt_tags_links_a_rotation_only_when_the_old_address_stops) {
    fresh();
    ad_t a = findmy(0x10, 0x31, true), b = findmy(0x10, 0x77, true), other = findmy(0x10, 0x55, true);
    /* a second tag of the same kind keeps talking throughout: never a candidate */
    for (uint32_t t = 0; t <= 600000; t += 3000) bw_add(w, A3, 1, 3, -85, other.b, other.n, t);
    for (uint32_t t = 0; t < 300000; t += 2000) bw_add(w, A1, 1, 3, -60, a.b, a.n, t);
    int j = -1;
    for (uint32_t t = 300500; t <= 600000; t += 2000) {
        j = bw_add(w, A2, 1, 3, -61, b.b, b.n, t);
        bw_tick(w, t);
    }
    const bw_dev_t *d = &w->dev[j];
    const int i = bw_find_uid(w, d->pred);
    LS_CHECK(i >= 0);
    LS_CHECK(!memcmp(w->dev[i].addr, A1, 6));
    LS_EQ_UINT(w->dev[i].succ, d->uid);
    LS_EQ_INT(d->rotations, 1);
    LS_EQ_UINT(d->chain_ms, 0u);                          /* presence runs from the first address */
    LS_EQ_UINT(bw_span_ms(d), 598500u);
    LS_EQ_INT(bw_window(d, 600000, 15), (int)(w->dev[i].count + 150)); /* counts carried over */
    const int o = bw_find_uid(w, w->dev[0].uid);
    LS_EQ_UINT(w->dev[o].succ, 0u);                        /* the steady neighbour is untouched */

    /* different status byte (another kind of Find My device): not linked */
    fresh();
    ad_t me = findmy(0x00, 0x31, true);
    for (uint32_t t = 0; t < 100000; t += 2000) bw_add(w, A1, 1, 3, -60, a.b, a.n, t);
    for (uint32_t t = 100500; t < 140000; t += 2000) { j = bw_add(w, A2, 1, 3, -60, me.b, me.n, t); bw_tick(w, t); }
    LS_EQ_UINT(w->dev[j].pred, 0u);

    /* the old one still talking after the new one starts: not linked */
    fresh();
    for (uint32_t t = 0; t < 140000; t += 2000) bw_add(w, A1, 1, 3, -60, a.b, a.n, t);
    for (uint32_t t = 100500; t < 140000; t += 2000) { j = bw_add(w, A2, 1, 3, -60, b.b, b.n, t); bw_tick(w, t); }
    LS_EQ_UINT(w->dev[j].pred, 0u);

    /* a public address never rotates */
    fresh();
    for (uint32_t t = 0; t < 100000; t += 2000) bw_add(w, A1, 1, 3, -60, a.b, a.n, t);
    for (uint32_t t = 100500; t < 140000; t += 2000) { j = bw_add(w, P1, 0, 3, -60, b.b, b.n, t); bw_tick(w, t); }
    LS_EQ_UINT(w->dev[j].pred, 0u);
}

LS_CASE(bughunt_tags_alerts_after_time_without_a_fix) {
    fresh();
    w->follow_ms = 10u * 60000u;
    ad_t a = findmy(0x10, 0x31, true);
    int fired = -1;
    uint32_t at = 0;
    for (uint32_t t = 0; t <= 12u * 60000u; t += 2000) {
        bw_add(w, A1, 1, 3, -60, a.b, a.n, t);
        const int k = bw_tick(w, t);
        if (k >= 0 && fired < 0) { fired = k; at = t; }
        else LS_CHECK(k < 0);                          /* once */
    }
    LS_CHECK(fired >= 0);
    LS_EQ_UINT(at, 600000u);
    LS_EQ_INT(w->dev[fired].alert, 1);
    /* the owner's own (nearby form) never alerts, nor does a beacon */
    fresh();
    ad_t mine = findmy(0x00, 0, false), beacon = {0};
    uint8_t ibv[25] = {0x4C, 0x00, 0x02, 0x15};
    put(&beacon, 0xFF, ibv, sizeof(ibv));
    for (uint32_t t = 0; t <= 30u * 60000u; t += 5000) {
        bw_add(w, A1, 1, 3, -50, mine.b, mine.n, t);
        bw_add(w, A3, 1, 3, -50, beacon.b, beacon.n, t);
        LS_CHECK(bw_tick(w, t) < 0);
    }
    /* a possible tracker alerts only when asked to */
    fresh();
    const uint8_t fp[] = {1, 2, 3};
    ad_t fpa = svc_data(0xFE2C, fp, 3);
    int k = -1;
    for (uint32_t t = 0; t <= 12u * 60000u && k < 0; t += 5000) { bw_add(w, A1, 1, 0, -50, fpa.b, fpa.n, t); k = bw_tick(w, t); }
    LS_CHECK(k < 0);
    w->alert_possible = true;
    LS_CHECK(bw_tick(w, 12u * 60000u) >= 0);
}

LS_CASE(bughunt_tags_a_long_gap_starts_presence_again) {
    fresh();
    ad_t a = findmy(0x10, 0x31, true);
    for (uint32_t t = 0; t <= 8u * 60000u; t += 2000) { bw_add(w, A1, 1, 3, -60, a.b, a.n, t); LS_CHECK(bw_tick(w, t) < 0); }
    /* gone for 6 minutes (more than the 5 minute gap), then back for 8 */
    for (uint32_t t = 14u * 60000u; t <= 22u * 60000u; t += 2000) { bw_add(w, A1, 1, 3, -60, a.b, a.n, t); LS_CHECK(bw_tick(w, t) < 0); }
    LS_EQ_UINT(bw_span_ms(&w->dev[0]), 8u * 60000u);
}

LS_CASE(bughunt_tags_with_a_fix_the_tag_has_to_come_along) {
    fresh();
    ad_t a = findmy(0x10, 0x31, true), b = findmy(0x10, 0x52, true);
    const uint8_t B1[6] = {9, 9, 9, 9, 9, 0xc9};
    /* standing still at a cafe for 20 minutes with a tag at the next table */
    bw_position(w, true, 28.5383, -81.3792);
    for (uint32_t t = 0; t <= 20u * 60000u; t += 2000) { bw_add(w, A1, 1, 3, -70, a.b, a.n, t); LS_CHECK(bw_tick(w, t) < 0); }
    LS_CHECK(w->dev[0].moved_m < 1.0f);
    /* walking north about 1.1 m a second; the cafe tag stops, another keeps up */
    int fired = -1;
    for (uint32_t t = 20u * 60000u; t <= 34u * 60000u; t += 2000) {
        const double lat = 28.5383 + (double)(t - 20u * 60000u) / 1000.0 * 0.00001;
        bw_position(w, true, lat, -81.3792);
        bw_add(w, B1, 1, 3, -60, b.b, b.n, t);
        const int k = bw_tick(w, t);
        if (k >= 0) { fired = k; break; }
    }
    LS_CHECK(fired >= 0);
    LS_CHECK(!memcmp(w->dev[fired].addr, B1, 6));
    LS_CHECK(w->dev[fired].moved_m >= 250.0f);
    LS_EQ_INT(w->dev[0].alert, 0);
    /* 0.001 degree of latitude is about 111 m */
    LS_NEAR(bw_distance_m(28.0, -81.0, 28.001, -81.0), 111.2, 0.5);
    LS_NEAR(bw_distance_m(0.0, 0.0, 0.0, 0.001), 111.2, 0.5);
}

LS_CASE(bughunt_tags_scan_response_adds_without_demoting) {
    fresh();
    ad_t a = findmy(0x10, 0x31, true), rsp = {0};
    put(&rsp, 0x09, (const uint8_t *)"Keys", 4);
    const int i = bw_add(w, A1, 1, 3, -60, a.b, a.n, 0);
    bw_add(w, A1, 1, 4, -61, rsp.b, rsp.n, 100);
    LS_EQ_INT(w->dev[i].ad.cls, BW_C_FINDMY);
    LS_EQ_STR(w->dev[i].ad.name, "Keys");
    LS_EQ_INT(w->dev[i].pdu, 3);                         /* the scan response is not the advert type */
    LS_EQ_INT(w->dev[i].raw_len, (int)a.n);
}

LS_CASE(bughunt_tags_csv_line) {
    fresh();
    ad_t a = findmy(0x10, 0x31, true);
    for (uint32_t t = 0; t <= 120000; t += 2000) bw_add(w, A1, 1, 3, -60, a.b, a.n, t);
    char line[200];
    const int n = bw_csv(&w->dev[0], "alert", 120000, line, sizeof(line));
    LS_CHECK(n > 0 && line[n - 1] == '\n');
    LS_EQ_STR(line, "120,alert,c4:5a:71:20:9e:13,random,FINDMY,004c,-60,61,120,0,0,AirTag?\n");
    /* the header names as many columns as a line has */
    int commas_h = 0, commas_l = 0;
    for (const char *p = BW_CSV_HEADER; *p; p++) commas_h += *p == ',';
    for (const char *p = line; *p; p++) commas_l += *p == ',';
    LS_EQ_INT(commas_h, commas_l);
}

LS_CASE(bughunt_tags_names_and_notes_cover_every_kind) {
    for (int c = 0; c < BW_C_COUNT; c++) {
        LS_CHECK(bw_class_tag(c)[0] && strlen(bw_class_tag(c)) <= 6);
        LS_CHECK(bw_class_name(c)[0]);
        LS_CHECK(bw_class_note(c) != NULL);
    }
    LS_EQ_STR(bw_class_tag(BW_C_COUNT), "?");
}

LS_CASE(bughunt_tags_mine_and_kind_switches_never_alert) {
    /* MINE: the same separated AirTag that alerts at 10 min stays quiet */
    fresh();
    w->follow_ms = 10u * 60000u;
    ad_t a = findmy(0x10, 0x31, true);
    const int i = bw_add(w, A1, 1, 3, -60, a.b, a.n, 0);
    bw_mark(w, i, BW_MARK_MINE);
    for (uint32_t t = 0; t <= 20u * 60000u; t += 2000) { bw_add(w, A1, 1, 3, -60, a.b, a.n, t); LS_CHECK(bw_tick(w, t) < 0); }
    /* a kind switched off is listed but never alerts; the others still do */
    fresh();
    w->follow_ms = 10u * 60000u;
    w->kind_off = bw_kind_bit(BW_C_FINDMY);
    LS_EQ_UINT(bw_kind_bit(BW_C_APPLE_PP), bw_kind_bit(BW_C_FINDMY));
    const uint8_t tile[] = {1, 2, 3, 4};
    ad_t t1 = svc_data(0xFEED, tile, 4);
    int fired = -1;
    for (uint32_t t = 0; t <= 12u * 60000u; t += 2000) {
        bw_add(w, A1, 1, 3, -60, a.b, a.n, t);
        bw_add(w, A2, 0, 0, -60, t1.b, t1.n, t);
        const int k = bw_tick(w, t);
        if (k >= 0) { LS_EQ_INT(w->dev[k].ad.cls, BW_C_TILE); fired = k; }
    }
    LS_CHECK(fired >= 0);
}

LS_CASE(bughunt_tags_watch_says_gone_and_back_through_a_rotation) {
    fresh();
    w->watch_ms = 60000u;
    ad_t a = findmy(0x10, 0x31, true);
    const int i = bw_add(w, A1, 1, 3, -60, a.b, a.n, 0);
    bw_mark(w, i, BW_MARK_WATCH);
    int ev = 0;
    for (uint32_t t = 0; t <= 60000u; t += 2000) { bw_add(w, A1, 1, 3, -60, a.b, a.n, t); LS_CHECK(bw_watch(w, t, &ev) < 0); }
    /* it rotates: the new address carries the mark and nothing fires */
    ad_t b = findmy(0x10, 0x77, true);
    for (uint32_t t = 61000u; t <= 120000u; t += 2000) { bw_add(w, A2, 1, 3, -61, b.b, b.n, t); bw_tick(w, t); LS_CHECK(bw_watch(w, t, &ev) < 0); }
    const int n2 = bw_find_uid(w, w->dev[i].succ);
    LS_CHECK(n2 >= 0);
    LS_EQ_INT(w->dev[n2].mark, BW_MARK_WATCH);
    /* quiet for the watch time and the rotation check: gone, once */
    int k = -1;
    uint32_t at = 0;
    for (uint32_t t = 122000u; t <= 300000u; t += 1000) {
        bw_tick(w, t);
        const int r = bw_watch(w, t, &ev);
        if (r >= 0) { LS_CHECK(k < 0); k = r; at = t; LS_EQ_INT(ev, BW_EV_GONE); }
    }
    LS_EQ_INT(k, n2);
    LS_CHECK(at >= 120000u + 60000u + 15000u && at <= 120000u + 60000u + 17000u);
    /* heard again: back */
    bw_add(w, A2, 1, 3, -70, b.b, b.n, 301000u);
    LS_EQ_INT(bw_watch(w, 301500u, &ev), n2);
    LS_EQ_INT(ev, BW_EV_BACK);
    LS_CHECK(bw_watch(w, 302000u, &ev) < 0);
}
