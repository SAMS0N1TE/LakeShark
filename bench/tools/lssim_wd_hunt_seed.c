/* lssim: targets for WARDRIVE's HUNT page beside the simulated drive
   (lssim_wardrive.c). A Flock camera and its Penguin battery at the first
   corner, a parked police car (Cradlepoint router, Axon body cam) further
   on, a Ring doorbell, a Flipper, camera glasses and a card skimmer by the
   road, and riding along the whole way a Tesla's phone key and a SmartTag
   in the bag. lssim_wardrive.c's wd_feed_poll adds these to what it heard:

       k += lssim_wd_hunt_obs(out + k, cap - k, now, e, n);

   with the car at (e, n) metres east and north of the start. Each is heard
   once a second when the car is within its range. Every name and address
   prefix here is one the signature table knows (data/wardrive/sigs.txt). */
#include "wardrive/wd_core.h"
#include "wardrive/wd_sig.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    double e, n;                /* where it is; NAN: in the car */
    float range;
    uint8_t kind;
    uint8_t mac[6];
    const char *name;
} spot_t;

static const spot_t SPOT[] = {
    { 640, 70, 160, WD_WIFI, { 0x02, 0x1b, 0x5a, 0x0b, 0x11, 0x01 }, "Flock-5A0B11" },
    { 650, 60, 90, WD_BLE, { 0xc4, 0x11, 0x22, 0x33, 0x44, 0x55 }, "Penguin-4027719305" },
    { 1190, 450, 90, WD_BLE, { 0x00, 0x25, 0xdf, 0x41, 0x0a, 0x7c }, "" },
    { 1200, 485, 200, WD_WIFI, { 0x00, 0x30, 0x44, 0x1c, 0x22, 0x9e }, "IBR600-29E" },
    { 1240, -120, 120, WD_WIFI, { 0x9c, 0x76, 0x13, 0x5e, 0x01, 0x2a }, "Ring-3F2A1C" },
    { 860, -600, 70, WD_BLE, { 0xd2, 0x80, 0x61, 0x13, 0x77, 0x0c }, "Flipper Kujota" },
    { 520, -580, 70, WD_BLE, { 0xe1, 0x52, 0x09, 0x3d, 0x62, 0x18 }, "Ray-Ban Meta 0A1B" },
    { 300, -545, 60, WD_BLE, { 0x98, 0xd3, 0x31, 0xf0, 0x12, 0x05 }, "HC-05" },
    { NAN, 0, 0, WD_BLE, { 0xd8, 0x4a, 0x2b, 0x91, 0x0e, 0x37 }, "S1f0c8a33d2e47b65C" },
    { NAN, 0, 0, WD_BLE, { 0xe4, 0x0d, 0x36, 0x2c, 0x18, 0xa0 }, "Galaxy SmartTag2" },
};
#define NSPOT ((int)(sizeof(SPOT) / sizeof(SPOT[0])))

int lssim_wd_hunt_obs(wd_obs_t *out, int cap, uint32_t now, double e, double n)
{
    static uint32_t last;
    static bool sigs;
    if (!sigs) { sigs = true; wd_sig_init(); }   /* the seed drives before WARDRIVE's enter() loads them */
    if (now - last < 1000 || cap <= 0) return 0;
    last = now;
    int k = 0;
    for (int i = 0; i < NSPOT && k < cap; i++) {
        const spot_t *s = &SPOT[i];
        double d = 3;
        if (!isnan(s->e)) {
            d = hypot(s->e - e, s->n - n);
            if (d > s->range) continue;
        } else if (((now / 1000) * 2654435761u >> 29) == 0) continue;   /* the ones in the car miss a second now and then */
        wd_obs_t *o = &out[k++];
        memset(o, 0, sizeof(*o));
        o->kind = s->kind;
        memcpy(o->addr, s->mac, 6);
        o->rssi = (int8_t)lround(-38 - 22 * log10(d < 1 ? 1 : d) - (now / 1000 + (uint32_t)i) % 5);
        o->channel = s->kind == WD_WIFI ? 6 : 0;
        o->auth = s->kind == WD_WIFI ? WD_AUTH_WPA2 : 0;
        snprintf(o->name, sizeof(o->name), "%s", s->name);
    }
    return k;
}
