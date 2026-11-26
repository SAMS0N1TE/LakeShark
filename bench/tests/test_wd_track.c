/* WARDRIVE tracker (wd_track.h): a drive east at 54 km/h past a few
   hundred access points and phones, with a car that pulls in behind at
   minute 2 (its hotspot, its phone key, four TPMS sensors) and turns off at
   minute 20, a tag in the bag the whole way, then twenty minutes parked
   beside a house. Followers and the car must be found; nothing stationary. */
#include "wardrive/wd_core.h"
#include "wardrive/wd_feed.h"
#include "wardrive/wd_sig.h"
#include "wardrive/wd_track.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void wd_feed_start(const bool src[WD_KINDS], int pace) { (void)src; (void)pace; }
void wd_feed_stop(void) {}
void wd_feed_pace(int pace) { (void)pace; }
bool wd_feed_has(int kind) { (void)kind; return true; }
bool wd_feed_fix(wd_fix_t *out, uint32_t now) { (void)now; memset(out, 0, sizeof(*out)); return false; }
int wd_feed_poll(wd_obs_t *out, int cap, uint32_t now, unsigned *scans) { (void)out; (void)cap; (void)now; (void)scans; return 0; }
bool wd_feed_spawn(void) { return false; }
bool wd_feed_worker_alive(void) { return false; }
void wd_lock(void) {}
void wd_unlock(void) {}
void *wd_feed_alloc(size_t bytes) { return calloc(1, bytes); }
void wd_feed_free(void *p) { free(p); }
bool wd_feed_card(void) { return false; }
const char *wd_feed_dir(void) { return ""; }
const char *wd_feed_board(void) { return "bench"; }
bool wd_feed_upload(const char *path) { (void)path; return false; }
bool wd_feed_hub(char *out, size_t cap, bool *busy) { (void)out; (void)cap; (void)busy; return false; }

#define LAT0 43.0
#define LON0 (-71.0)
#define MPD 111194.9266
#define SPEED 15.0
#define N_AP 300
#define N_PH 120

static uint32_t rs = 7;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }

static void fix_at(double x, float speed, uint32_t now)
{
    wd_fix_t f;
    memset(&f, 0, sizeof(f));
    f.fix = true;
    f.lat = LAT0;
    f.lon = LON0 + x / (MPD * cos(LAT0 * M_PI / 180));
    f.alt_m = 50; f.hdop = 1; f.sats = 9;
    f.speed_mps = speed; f.course_deg = 90; f.compass_deg = -1;
    f.utc = 1795096800u + now / 1000;
    wd_fix_note(&f, now);
}

static void hear(int kind, uint32_t id, const char *name, int rssi, uint16_t tags, uint32_t now)
{
    wd_obs_t o;
    memset(&o, 0, sizeof(o));
    o.kind = (uint8_t)kind;
    o.addr[0] = (uint8_t)(0x02 | (kind << 4));
    o.addr[2] = (uint8_t)(id >> 24); o.addr[3] = (uint8_t)(id >> 16); o.addr[4] = (uint8_t)(id >> 8); o.addr[5] = (uint8_t)id;
    o.rssi = (int8_t)rssi;
    o.tags = tags;
    snprintf(o.name, sizeof(o.name), "%s", name);
    wd_note(&o, now);
}

static int find(int kind, uint32_t id)
{
    for (int i = 0; i < wd_dev_count(); i++) {
        const wd_dev_t *e = wd_dev_at(i);
        if (e->kind == kind && e->addr[5] == (uint8_t)id && e->addr[4] == (uint8_t)(id >> 8) && e->addr[3] == (uint8_t)(id >> 16))
            return i;
    }
    return -1;
}

static bool follows(int kind, uint32_t id) { const int i = find(kind, id); return i >= 0 && (wd_dev_at(i)->tags & WD_T_FOLLOW); }

enum { HOT = 0xC0FFEE01, KEY = 0xC0FFEE02, TAG = 0xBA9BA901, TPMS0 = 0x1A2B3C00, HOUSE_AP = 0x40000001, HOUSE_TV = 0x40000002 };

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    wd_sig_load(NULL, 0);
    bool src[WD_KINDS];
    for (int k = 0; k < WD_KINDS; k++) src[k] = true;
    uint32_t now = 1000;
    fix_at(0, 0, now);
    assert(wd_begin(now, src, false));
    double ap_x[N_AP], ph_x[N_PH];
    for (int i = 0; i < N_AP; i++) ap_x[i] = (double)(rnd() % 22000);
    for (int i = 0; i < N_PH; i++) ph_x[i] = (double)(rnd() % 22000);
    double x = 0;
    int follow_alerts = 0, veh_alerts = 0, sig_alerts = 0;
    uint16_t car_num = 0;
    const uint32_t t0 = now;
    for (uint32_t t = 0; t < 25 * 60; t++) {
        now = t0 + t * 1000;
        x = SPEED * t;
        fix_at(x, SPEED, now);
        /* the road: access points every 3 s within 250 m, phones within 80 m */
        if (t % 3 == 0)
            for (int i = 0; i < N_AP; i++) {
                const double d = fabs(ap_x[i] - x);
                if (d < 250) hear(WD_WIFI, (uint32_t)i, "NETGEAR", (int)(-40 - d / 6), 0, now);
            }
        for (int i = 0; i < N_PH; i++) {
            const double d = fabs(ph_x[i] - x);
            if (d < 80) hear(WD_BLE, 0x10000u + (uint32_t)i, "", (int)(-50 - d / 3), 0, now);
        }
        /* the tag in the bag, most seconds */
        if (rnd() % 10 < 7) hear(WD_BLE, TAG, "", -45, 0, now);
        /* the car behind, minutes 2 to 20 */
        if (t >= 120 && t < 1200) {
            if (t % 3 == 0) hear(WD_WIFI, HOT, "myChevrolet 2C3D", -60, 0, now);
            if (rnd() % 10 < 6) hear(WD_BLE, KEY, "S0123456789abcdefC", -70, 0, now);
            for (int s = 0; s < 4; s++)
                if ((t + (uint32_t)s * 17) % 60 == 0) hear(WD_RF, TPMS0 + (uint32_t)s, "", -65, WD_T_VEHICLE, now);
        }
        wd_track_tick(now);
        wd_alert_t a;
        while (wd_track_alert(&a)) {
            if (a.why == WD_AL_FOLLOW) follow_alerts++;
            if (a.why == WD_AL_VEHICLE) veh_alerts++;
            if (a.why == WD_AL_SIG) sig_alerts++;
        }
        if (t == 15 * 60) {
            const int k = find(WD_WIFI, HOT);
            wd_trk_t ti;
            assert(k >= 0 && wd_track_info(k, &ti));
            car_num = ti.veh;
            printf("minute 15: hotspot span %.0f m, %u places, %u steps, vehicle %u\n", (double)ti.span_m, ti.places, ti.steps, ti.veh);
            assert(car_num);
        }
    }
    printf("driven %.1f km; %d follow, %d vehicle, %d signature alerts; %d following, %d vehicles\n",
           (double)wd_stats()->dist_m / 1000, follow_alerts, veh_alerts, sig_alerts, wd_track_following(), wd_track_vehicles());

    /* nothing beside the road follows */
    for (int i = 0; i < N_AP; i++) assert(!follows(WD_WIFI, (uint32_t)i));
    for (int i = 0; i < N_PH; i++) assert(!follows(WD_BLE, 0x10000u + (uint32_t)i));
    /* everything in the car and the bag does */
    assert(follows(WD_WIFI, HOT) && follows(WD_BLE, KEY) && follows(WD_BLE, TAG));
    for (int s = 0; s < 4; s++) assert(follows(WD_RF, TPMS0 + (uint32_t)s));
    assert(follow_alerts >= 7);
    /* the phone key matched its signature and said so */
    assert(sig_alerts >= 1);

    /* the car is one vehicle, its number kept, the tag not in it */
    const wd_veh_t *car = wd_track_vehicle_num(car_num);
    assert(car);
    printf("vehicle %u: %u members, together %.1f km, tags %s\n", car->num, car->n, (double)car->with_m / 1000, wd_tag_word(car->tags));
    assert(car->n == 6);
    for (int m = 0; m < car->n; m++) {
        const wd_dev_t *e = wd_dev_at(car->idx[m]);
        assert(e && !(e->kind == WD_BLE && e->addr[5] == (uint8_t)TAG));
    }
    assert(car->with_m > 14000 && car->with_m < 18000);
    /* the strip: heard up to minute 20 (step 120 of 149), not since */
    int heard = 0;
    for (int i = WD_VEH_TRAIL - 28; i < WD_VEH_TRAIL; i++) heard += car->trail[i] > -128;
    assert(heard == 0 && car->trail[0] == -60 && car->trail[1] == -60);
    {
        const int k = find(WD_BLE, TAG);
        wd_trk_t ti;
        assert(wd_track_info(k, &ti) && ti.veh != car_num);
    }
    assert(veh_alerts >= 1);

    /* parked twenty minutes beside a house: its Wi-Fi and TV never follow */
    for (uint32_t t = 0; t < 20 * 60; t++) {
        now += 1000;
        fix_at(x + (t % 7) * 0.5, 0, now);
        if (t % 3 == 0) hear(WD_WIFI, HOUSE_AP, "HOME-1234", -50, 0, now);
        hear(WD_BLE, HOUSE_TV, "[TV] Samsung", -60, 0, now);
        wd_track_tick(now);
    }
    assert(!follows(WD_WIFI, HOUSE_AP) && !follows(WD_BLE, HOUSE_TV));
    assert(wd_track_vehicle_num(car_num) && wd_track_vehicle_num(car_num)->num == car_num);

    /* a new session forgets */
    wd_end(now);
    assert(wd_begin(now + 1000, src, false));
    assert(wd_track_vehicles() == 0 && wd_track_following() == 0 && wd_track_hits() == 0);
    printf("test_wd_track: ok\n");
    return 0;
}
