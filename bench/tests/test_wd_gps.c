/* WARDRIVE's GPS gate (wd_core.c wd_fix_note), from the SX drive
   WD_20261125_015737: a cold GPS walked 170 m north across the river at HDOP
   4.5 in its first minute, then came back. Weak fixes must not move the
   track, a new position must settle before it counts, and a jump no car
   makes is dropped. */
#include "wardrive/wd_core.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wardrive/wd_feed.h"

/* the feed: nothing heard, no card */
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

static wd_fix_t fx(double lat, double lon, float hdop, uint8_t sats, float speed)
{
    wd_fix_t f;
    memset(&f, 0, sizeof(f));
    f.fix = true; f.lat = lat; f.lon = lon; f.alt_m = 28; f.hdop = hdop; f.sats = sats;
    f.speed_mps = speed; f.course_deg = -1; f.compass_deg = -1; f.utc = 1795000000u;
    return f;
}

int main(void)
{
    bool src[WD_KINDS];
    for (int k = 0; k < WD_KINDS; k++) src[k] = true;
    uint32_t t = 1000;
    assert(wd_begin(t, src, false));
    /* the cold start: twenty fixes at HDOP 4.5 walking north 170 m */
    for (int i = 0; i < 20; i++) {
        wd_fix_t f = fx(43.44564 + i * 0.000078, -71.64414, 4.5f, 7, 5.0f);
        wd_fix_note(&f, t += 1000);
    }
    assert(wd_stats()->fix_weak);
    assert(wd_trail_count() == 0);                  /* nothing kept from the walk */
    assert(!wd_stats()->origin);
    /* the GPS settles at home: HDOP 1.6, standing */
    for (int i = 0; i < 6; i++) {
        wd_fix_t f = fx(43.44570 + (i % 2) * 0.00001, -71.64460, 1.6f, 9, 0.1f);
        wd_fix_note(&f, t += 1000);
    }
    assert(!wd_stats()->fix_weak && wd_stats()->origin);
    assert(fabs(wd_stats()->lat0 - 43.44570) < 0.0002);  /* the origin is home, not across the river */
    assert(wd_stats()->dist_m < 30.0f);
    /* a 2 km jump in a second is a bad fix */
    const int before = wd_trail_count();
    wd_fix_t j = fx(43.46370, -71.64460, 1.2f, 10, 15.0f);
    wd_fix_note(&j, t += 1000);
    assert(wd_trail_count() == before && wd_stats()->dist_m < 30.0f);
    /* driving at 25 m/s keeps settling and adds the road */
    for (int i = 1; i <= 10; i++) {
        wd_fix_t f = fx(43.44570 + i * 0.000225, -71.64460, 1.2f, 10, 25.0f);
        wd_fix_note(&f, t += 1000);
    }
    assert(wd_stats()->dist_m > 200.0f && wd_trail_count() > before);
    printf("PASS: weak fixes do not move the track, a new fix settles first, a 2 km jump is dropped, driving still counts\n");
    return 0;
}
