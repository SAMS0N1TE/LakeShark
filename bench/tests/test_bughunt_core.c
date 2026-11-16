/* The BUGHUNT detection core (app-sdk/apps/bughunt/bh_core.c) on made-up
   sweeps: RSSI timelines as the LoRa radio's scanner would sample them
   (one step at a time, about six a second), Bluetooth adverts through its
   tag engine (bh_tags.c), and 802.11 frames. Checks the verdicts, and that
   a quiet room, a busy tower downlink and a passing tag stay quiet. */

#include "ls_test.h"
#include "bh_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bh_t H;
static bh_find_t F[BH_MAX_FIND];

static uint32_t rnd_state = 12345;
static int rnd(int n)
{
    rnd_state = rnd_state * 1103515245u + 12345u;
    return (int)((rnd_state >> 16) % (uint32_t)n);
}

/* What a step reads at time t: quiet noise unless the scenario says else. */
typedef int (*level_fn)(uint32_t khz, uint32_t t_ms);
static int quiet(uint32_t khz, uint32_t t) { (void)khz; (void)t; return -1100 + rnd(60) - 30; }

/* Run the plan the way the app does: chunk after chunk, one sample every
   160 ms, for dur_ms. Returns the sweep time it measured. */
static void run(bh_t *h, level_fn lv, uint32_t t0, uint32_t dur_ms)
{
    uint64_t hz[64];
    uint32_t t = t0, pass_start = t0;
    while (t - t0 < dur_ms) {
        const int nc = bh_chunks(h, 64);
        for (int c = 0; c < nc && t - t0 < dur_ms; c++) {
            const int n = bh_chunk(h, c, 64, hz);
            for (int i = 0; i < n && t - t0 < dur_ms; i++) {
                bh_rf_sample(h, hz[i], lv((uint32_t)(hz[i] / 1000u), t), t);
                t += 160;
                if (!(t % 480)) bh_tick(h, t);
            }
            t += 300;                       /* restarting the scanner */
        }
        if (t - t0 < dur_ms) h->sweep_ms = t - pass_start;     /* a whole pass */
        pass_start = t;
    }
    bh_tick(h, t);
}
static int find(int n, int kind)
{
    for (int i = 0; i < n; i++) if (F[i].kind == kind) return i;
    return -1;
}
static int count_kind(int n, int kind)
{
    int c = 0;
    for (int i = 0; i < n; i++) c += F[i].kind == kind;
    return c;
}
static void dump(int n)
{
    for (int i = 0; i < n; i++) ls_note("  [%d] kind %d conf %d %s | %s", i, F[i].kind, F[i].conf, F[i].title, F[i].detail);
    for (int b = 0; b < H.nband; b++) {
        if (!H.band[b].nburst) continue;
        char t[400];
        int o = snprintf(t, sizeof(t), "  %s (pass %u ms) bursts:", H.band[b].name, (unsigned)H.sweep_ms);
        for (int i = 0; i < H.band[b].nburst && o < 380; i++) o += snprintf(t + o, sizeof(t) - o, " %u", (unsigned)(H.band[b].burst[i].t_ms / 1000));
        ls_note("%s", t);
    }
}

LS_CASE(plan_by_mode_region_and_radio)
{
    bh_init(&H, BH_ROOM, BH_REG_BOTH, false, 240000, 1000);
    bool b28 = false, uhf = false, b8 = false, g24 = false;
    for (int b = 0; b < H.nband; b++) {
        if (!strcmp(H.band[b].name, "B28")) { b28 = true; LS_EQ_UINT(H.band[b].lo_khz, 699000u); LS_EQ_UINT(H.band[b].hi_khz, 748000u); }
        if (!strcmp(H.band[b].name, "B12")) LS_CHECK_MSG(0, "B12 merges into B28 when both regions are on");
        uhf |= !strcmp(H.band[b].name, "UHF");
        b8 |= !strcmp(H.band[b].name, "B8");
        g24 |= !strcmp(H.band[b].name, "2G4");
    }
    LS_CHECK(b28 && uhf && b8 && !g24);
    LS_CHECK(H.nstep > 64 && H.nstep <= BH_MAX_STEPS);
    LS_EQ_INT(bh_chunks(&H, 64), (H.nstep + 63) / 64);
    uint64_t hz[64];
    int total = 0;
    for (int c = 0; c < bh_chunks(&H, 64); c++) {
        const int n = bh_chunk(&H, c, 64, hz);
        LS_CHECK(n > 0 && n <= 64);
        for (int i = 0; i < n; i++) LS_EQ_INT(bh_step_of(&H, hz[i]), total + i);
        total += n;
    }
    LS_EQ_INT(total, H.nstep);
    LS_EQ_INT(bh_step_of(&H, 433920000ull), bh_step_of(&H, 433900000ull));
    LS_EQ_INT(bh_step_of(&H, 100000000ull), -1);

    bh_init(&H, BH_CAR, BH_REG_US, false, 900000, 0);
    for (int b = 0; b < H.nband; b++) {
        LS_CHECK_MSG(strcmp(H.band[b].name, "UHF"), "CAR leaves the UHF bug band out");
        LS_CHECK_MSG(strcmp(H.band[b].name, "B8"), "US leaves 880-915 out");
    }
    bh_init(&H, BH_ROOM, BH_REG_AR, true, 240000, 0);
    g24 = false;
    for (int b = 0; b < H.nband; b++) g24 |= !strcmp(H.band[b].name, "2G4");
    LS_CHECK(g24);
    /* a band the radio refuses leaves the scan lists */
    const int before = bh_chunk(&H, bh_chunks(&H, 64) - 1, 64, hz) + (bh_chunks(&H, 64) - 1) * 64;
    bh_rf_refused(&H, 2444000000ull);
    const int after = bh_chunk(&H, bh_chunks(&H, 64) - 1, 64, hz) + (bh_chunks(&H, 64) - 1) * 64;
    LS_EQ_INT(before - after, 20);
}

LS_CASE(period_from_event_times)
{
    int m;
    const uint32_t a[] = { 10000, 130000, 250000, 370000, 490000 };
    uint32_t p = bh_period(a, 5, 16000, &m);
    LS_CHECK_MSG(p >= 118000 && p <= 122000, "period %u", (unsigned)p);
    LS_EQ_INT(m, 4);
    const uint32_t missed[] = { 10000, 130000, 370000, 490000, 610000 };      /* one check-in not caught */
    p = bh_period(missed, 5, 16000, &m);
    LS_CHECK_MSG(p >= 118000 && p <= 122000, "with a gap: %u", (unsigned)p);
    const uint32_t jitter[] = { 0, 127000, 236000, 365000, 478000, 604000 }; /* +-10 s of sweep timing */
    p = bh_period(jitter, 6, 18000, &m);
    LS_CHECK_MSG(p >= 115000 && p <= 125000, "jittered: %u", (unsigned)p);
    const uint32_t irregular[] = { 0, 23000, 101000, 118000, 199000, 260000, 270000 };
    LS_EQ_UINT(bh_period(irregular, 7, 18000, &m), 0u);
    const uint32_t two[] = { 0, 120000 };
    LS_EQ_UINT(bh_period(two, 2, 18000, &m), 0u);
}

LS_CASE(quiet_room_says_nothing_on_rf)
{
    rnd_state = 7;
    bh_init(&H, BH_ROOM, BH_REG_BOTH, false, 240000, 0);
    run(&H, quiet, 0, 240000);
    LS_CHECK(H.ref < -1050 && H.ref > -1150);
    LS_CHECK_MSG(H.sweep_ms > 14000 && H.sweep_ms < 22000, "sweep %u ms", (unsigned)H.sweep_ms);
    const int n = bh_findings(&H, 240000, F, BH_MAX_FIND);
    dump(n);
    LS_EQ_INT(n, 0);
}

/* A tracker on 840 MHz: a 20 s session every 2 min (attach, fix, upload),
   strong enough to lift the steps around it. Not caught every time. */
static int tracker(uint32_t khz, uint32_t t)
{
    const uint32_t ph = (t + 30000) % 120000;
    if (ph < 20000 && khz >= 831000 && khz <= 849000 && t < 700000) return -620 + rnd(40);
    return quiet(khz, t);
}
LS_CASE(cellular_tracker_check_ins_give_a_period)
{
    rnd_state = 11;
    bh_init(&H, BH_CAR, BH_REG_BOTH, false, 900000, 0);
    run(&H, tracker, 0, 720000);
    const int n = bh_findings(&H, 720000, F, BH_MAX_FIND);
    dump(n);
    LS_CHECK(n >= 1);
    LS_EQ_INT(F[0].kind, BH_F_CELL_PERIODIC);
    LS_CHECK_MSG(F[0].period_ms >= 110000 && F[0].period_ms <= 130000, "period %u", (unsigned)F[0].period_ms);
    LS_CHECK(F[0].khz >= 831000 && F[0].khz <= 849000);
    LS_CHECK(F[0].conf >= 65);
    LS_CHECK(F[0].target == BH_T_FREQ);
    LS_CHECK(strstr(F[0].title, "every ~2 min") != NULL);
    LS_CHECK(strstr(F[0].detail, "airplane") != NULL);
    LS_EQ_INT(count_kind(n, BH_F_PHONE), 0);
}

/* A phone in use: bursts in B28 at random moments. */
static uint32_t phone_at[16];
static int phone_level(uint32_t khz, uint32_t t)
{
    for (int i = 0; i < 16; i++)
        if (t >= phone_at[i] && t < phone_at[i] + 9000 && khz >= 711000 && khz <= 725000) return -700;
    return quiet(khz, t);
}
LS_CASE(irregular_bursts_read_as_a_phone)
{
    rnd_state = 3;
    const uint32_t at[16] = { 5000, 31000, 52000, 110000, 128000, 190000, 205000, 300000,
                              333000, 345000, 420000, 470000, 520000, 530000, 610000, 700000 };
    memcpy(phone_at, at, sizeof(at));
    bh_init(&H, BH_CAR, BH_REG_BOTH, false, 900000, 0);
    run(&H, phone_level, 0, 720000);
    const int n = bh_findings(&H, 720000, F, BH_MAX_FIND);
    dump(n);
    LS_CHECK(find(n, BH_F_PHONE) >= 0);
    LS_EQ_INT(count_kind(n, BH_F_CELL_PERIODIC), 0);
}

/* An audio bug on 433.92 MHz, and the 850 tower downlink spilling into the
   low part of B8 (880-894), wandering by +-8 dB. */
static int carrier_and_downlink(uint32_t khz, uint32_t t)
{
    if (khz == 433920) return -720 + rnd(20);
    if (khz >= 880000 && khz <= 894000) return -820 + rnd(160) - 80;
    return quiet(khz, t);
}
LS_CASE(steady_carrier_found_downlink_ignored)
{
    rnd_state = 5;
    bh_init(&H, BH_ROOM, BH_REG_BOTH, false, 240000, 0);
    run(&H, carrier_and_downlink, 0, 240000);
    const int n = bh_findings(&H, 240000, F, BH_MAX_FIND);
    dump(n);
    const int c = find(n, BH_F_CARRIER);
    LS_CHECK(c >= 0);
    if (c >= 0) {
        LS_EQ_UINT(F[c].khz, 433920u);
        LS_CHECK(F[c].conf >= 55);
        LS_CHECK(strstr(F[c].title, "433.92") != NULL);
    }
    for (int i = 0; i < n; i++)
        LS_CHECK_MSG(!(F[i].khz >= 880000 && F[i].khz <= 895000 && F[i].conf >= 30), "downlink flagged: %s", F[i].title);
}

static int one_loud(uint32_t khz, uint32_t t)
{
    if (khz == 915000 && t > 60000 && t < 80000) return -380;
    return quiet(khz, t);
}
LS_CASE(very_strong_signal_is_close)
{
    rnd_state = 9;
    bh_init(&H, BH_ROOM, BH_REG_BOTH, false, 240000, 0);
    run(&H, one_loud, 0, 120000);
    const int n = bh_findings(&H, 120000, F, BH_MAX_FIND);
    dump(n);
    const int s = find(n, BH_F_STRONG);
    LS_CHECK(s >= 0);
    if (s >= 0) LS_EQ_UINT(F[s].khz, 915000u);
}

/* ---- Bluetooth ---- */
static size_t airtag(uint8_t *ad, bool separated)
{
    size_t n = 0;
    if (separated) {
        ad[n++] = 0x1E; ad[n++] = 0xFF; ad[n++] = 0x4C; ad[n++] = 0x00; ad[n++] = 0x12; ad[n++] = 0x19; ad[n++] = 0x10;
        for (int i = 0; i < 22; i++) ad[n++] = (uint8_t)(0x40 + i);
        ad[n++] = 0x01; ad[n++] = 0x00;
    } else {
        ad[n++] = 0x07; ad[n++] = 0xFF; ad[n++] = 0x4C; ad[n++] = 0x00; ad[n++] = 0x12; ad[n++] = 0x02; ad[n++] = 0x24; ad[n++] = 0x00;
    }
    return n;
}
LS_CASE(tag_with_you_the_whole_car_sweep)
{
    bh_init(&H, BH_CAR, BH_REG_BOTH, false, 900000, 0);
    const uint8_t tag[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0xC6 };
    const uint8_t tile[6] = { 0x99, 0x88, 0x77, 0x66, 0x55, 0xD4 };
    const uint8_t phone[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x46 };
    const uint8_t tile_ad[] = { 0x03, 0x03, 0xED, 0xFE };
    const uint8_t phone_ad[] = { 0x0A, 0xFF, 0x4C, 0x00, 0x10, 0x05, 0x01, 0x18, 0x00, 0x00, 0x00 };
    uint8_t ad[40];
    const size_t an = airtag(ad, true);
    for (uint32_t t = 0; t <= 480000; t += 2000) {
        bh_ble_advert(&H, tag, 1, 3, -58 - (int)(t / 2000 % 4), ad, an, t);
        if (t < 30000) bh_ble_advert(&H, tile, 1, 0, -80, tile_ad, sizeof(tile_ad), t);
        bh_ble_advert(&H, phone, 1, 0, -50, phone_ad, sizeof(phone_ad), t);
        if (!(t % 10000)) bh_tick(&H, t);
    }
    const int n = bh_findings(&H, 480000, F, BH_MAX_FIND);
    dump(n);
    LS_CHECK(n >= 2);
    LS_EQ_INT(F[0].kind, BH_F_BLE_FOLLOW);
    LS_CHECK(F[0].conf >= 80);
    LS_CHECK(F[0].target == BH_T_BLE);
    LS_CHECK(!memcmp(F[0].addr, tag, 6));
    LS_CHECK(strstr(F[0].title, "AirTag") != NULL && strstr(F[0].title, "with you") != NULL);
    LS_CHECK(strstr(F[0].detail, "travelling with you") != NULL);
    const int t = find(n, BH_F_BLE_TAG);
    LS_CHECK(t >= 0);
    if (t >= 0) {
        LS_CHECK(strstr(F[t].title, "Tile") != NULL);
        LS_CHECK(F[t].conf < F[0].conf);
        LS_CHECK(strstr(F[t].detail, "Gone now") != NULL);
    }
    for (int i = 0; i < n; i++) LS_CHECK_MSG(memcmp(F[i].addr, phone, 6), "a phone is not a tag");
}
LS_CASE(owners_own_airtag_scores_lower)
{
    bh_init(&H, BH_ROOM, BH_REG_BOTH, false, 240000, 0);
    const uint8_t mine[6] = { 0x21, 0x22, 0x23, 0x24, 0x25, 0xC1 };
    uint8_t ad[40];
    const size_t an = airtag(ad, false);
    for (uint32_t t = 0; t <= 240000; t += 2000) bh_ble_advert(&H, mine, 1, 3, -55, ad, an, t);
    bh_tick(&H, 240000);
    const int n = bh_findings(&H, 240000, F, BH_MAX_FIND);
    dump(n);
    LS_CHECK(n == 1);
    LS_CHECK(F[0].conf < 70);
    LS_CHECK(strstr(F[0].detail, "near its owner") != NULL);
    LS_CHECK(strstr(F[0].title, "in range") != NULL);
}

/* ---- Wi-Fi ---- */
static uint8_t fr[64];
static size_t data_up(const uint8_t *sta, const uint8_t *ap)
{
    memset(fr, 0, sizeof(fr));
    fr[0] = 0x08; fr[1] = 0x01;                 /* data, to DS */
    memcpy(fr + 4, ap, 6); memcpy(fr + 10, sta, 6); memcpy(fr + 16, ap, 6);
    return 24 + 8;
}
static size_t beacon(const uint8_t *bssid)
{
    static const uint8_t bc[6] = { 255, 255, 255, 255, 255, 255 };
    memset(fr, 0, sizeof(fr));
    fr[0] = 0x80;
    memcpy(fr + 4, bc, 6); memcpy(fr + 10, bssid, 6); memcpy(fr + 16, bssid, 6);
    fr[32] = 0x64; fr[34] = 0x01;               /* interval, ESS */
    return 36;
}
LS_CASE(wifi_camera_streaming_and_camera_maker)
{
    bh_init(&H, BH_ROOM, BH_REG_BOTH, false, 240000, 0);
    const uint8_t ap[6] = { 0x02, 0xAA, 0, 0, 0, 1 };
    const uint8_t hik[6] = { 0x44, 0x19, 0xB6, 0x10, 0x20, 0x30 };
    const uint8_t dahua_ap[6] = { 0x90, 0x02, 0xA9, 0x01, 0x02, 0x03 };
    const uint8_t randphone[6] = { 0x5A, 0x11, 0x22, 0x33, 0x44, 0x55 };
    for (uint32_t t = 0; t < 120000; t += 10) {
        size_t n = data_up(hik, ap);
        bh_wifi_frame(&H, fr, n, 1400, -48, t);
        if (!(t % 3000)) { n = data_up(randphone, ap); bh_wifi_frame(&H, fr, n, 200, -60, t); }
        if (!(t % 1020)) { n = beacon(dahua_ap); bh_wifi_frame(&H, fr, n, 120, -70, t); }
    }
    const int n = bh_findings(&H, 120000, F, BH_MAX_FIND);
    dump(n);
    LS_EQ_INT(n, 2);
    LS_EQ_INT(F[0].kind, BH_F_WIFI_STREAM);
    LS_CHECK(F[0].conf >= 85);
    LS_CHECK(!memcmp(F[0].addr, hik, 6));
    LS_CHECK(strstr(F[0].title, "Hikvision") != NULL);
    LS_EQ_INT(F[1].kind, BH_F_WIFI_CAM);
    LS_CHECK(strstr(F[1].title, "Dahua") != NULL);
    LS_CHECK(bh_cam_vendor(hik) && !strcmp(bh_cam_vendor(hik), "Hikvision"));
    LS_CHECK(bh_cam_vendor(randphone) == NULL);
}

LS_CASE(ranking_puts_tracker_over_sensor_noise)
{
    rnd_state = 21;
    bh_init(&H, BH_CAR, BH_REG_BOTH, false, 900000, 0);
    run(&H, tracker, 0, 720000);
    const uint8_t ap[6] = { 0x02, 0xAA, 0, 0, 0, 1 }, esp[6] = { 0x24, 0x0A, 0xC4, 1, 2, 3 };
    for (uint32_t t = 0; t < 60000; t += 500) { const size_t n = data_up(esp, ap); bh_wifi_frame(&H, fr, n, 100, -70, t); }
    const int n = bh_findings(&H, 720000, F, BH_MAX_FIND);
    dump(n);
    LS_EQ_INT(F[0].kind, BH_F_CELL_PERIODIC);
    for (int i = 1; i < n; i++) LS_CHECK(F[i].conf <= F[i - 1].conf);
    LS_EQ_INT(count_kind(n, BH_F_WIFI_STREAM), 0);     /* a chatty smart plug is not a stream */
}

LS_CASE(hunt_direction_and_geiger)
{
    bh_dir_t d;
    bh_dir_reset(&d);
    LS_EQ_INT(bh_dir_covered(&d), 0);
    for (int a = 0; a < 360; a += 5) {
        const int diff = abs(((a - 120 + 540) % 360) - 180);
        bh_dir_add(&d, (float)a, -600 - diff * 2);
    }
    bh_dir_add(&d, -1.0f, -100);                        /* heading unknown: ignored */
    LS_EQ_INT(bh_dir_covered(&d), BH_DIRS);
    LS_EQ_INT(d.peak_bin, 12);
    LS_EQ_INT(d.peak, -600);
    LS_CHECK(bh_geiger_ms(-1000, -1000) >= 1000);
    LS_CHECK(bh_geiger_ms(-800, -1000) < bh_geiger_ms(-900, -1000));
    LS_CHECK(bh_geiger_ms(-300, -1000) <= 60);
    LS_CHECK(bh_geiger_ms(-1200, -1000) >= 1000);
}

LS_CASE(apple_devices_with_owners_make_one_card)
{
    bh_init(&H, BH_ROOM, BH_REG_BOTH, false, 240000, 0);
    uint8_t near_ad[40], sep_ad[40];
    const size_t nn = airtag(near_ad, false), sn = airtag(sep_ad, true);
    const uint8_t sep[6] = { 0x31, 0x32, 0x33, 0x34, 0x35, 0xC9 };
    for (uint32_t t = 0; t <= 240000; t += 2000) {
        for (int k = 0; k < 4; k++) {
            const uint8_t a[6] = { 0x40, 0x41, 0x42, 0x43, (uint8_t)k, 0xC2 };
            bh_ble_advert(&H, a, 1, 3, -60 - k * 5, near_ad, nn, t);
        }
        bh_ble_advert(&H, sep, 1, 3, -75, sep_ad, sn, t);
    }
    bh_tick(&H, 240000);
    const int n = bh_findings(&H, 240000, F, BH_MAX_FIND);
    dump(n);
    LS_EQ_INT(n, 2);
    LS_CHECK(!memcmp(F[0].addr, sep, 6));                /* the separated one leads */
    LS_CHECK(F[0].conf >= 80);
    LS_CHECK(strstr(F[1].title, "4 Apple devices") != NULL);
    LS_CHECK(F[1].conf < 30);
    LS_EQ_INT(F[1].level, -600);                         /* HUNT goes to the strongest */
}

/* Random phone traffic on many seeds: never a schedule. */
static uint32_t rnd_at[24];
static int rnd_level(uint32_t khz, uint32_t t)
{
    for (int i = 0; i < 24; i++)
        if (t >= rnd_at[i] && t < rnd_at[i] + 6000 && khz >= 815000 && khz <= 825000) return -720;
    return quiet(khz, t);
}
LS_CASE(random_bursts_never_look_scheduled)
{
    int periodic = 0;
    for (uint32_t seed = 1; seed <= 12; seed++) {
        rnd_state = seed * 7919u;
        uint32_t t = 5000;
        for (int i = 0; i < 24; i++) { rnd_at[i] = t; t += 12000u + (uint32_t)rnd(90) * 1000u; }
        bh_init(&H, BH_CAR, BH_REG_BOTH, false, 900000, 0);
        run(&H, rnd_level, 0, 900000);
        const int n = bh_findings(&H, 900000, F, BH_MAX_FIND);
        if (count_kind(n, BH_F_CELL_PERIODIC)) { periodic++; dump(n); }
    }
    LS_CHECK_MSG(periodic == 0, "%d of 12 random runs read as a tracker", periodic);
}

/* The board's own Wi-Fi and Bluetooth radio sits centimetres from the LoRa
   antenna: -27 dBm at 2.4 GHz is not a bug close by. */
static int own_wifi(uint32_t khz, uint32_t t)
{
    if (khz >= 2420000 && khz <= 2440000 && (t / 160) % 3 == 0) return -270;
    if (khz >= 2402000) return -900 + rnd(60) - 30;
    return quiet(khz, t);
}
LS_CASE(the_boards_own_wifi_is_not_a_strong_signal)
{
    rnd_state = 13;
    bh_init(&H, BH_ROOM, BH_REG_BOTH, true, 240000, 0);
    run(&H, own_wifi, 0, 120000);
    const int n = bh_findings(&H, 120000, F, BH_MAX_FIND);
    dump(n);
    for (int i = 0; i < n; i++) LS_CHECK_MSG(!(F[i].khz >= 2402000 && F[i].kind == BH_F_STRONG), "%s", F[i].title);
}
