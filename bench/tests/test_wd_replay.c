/* WARDRIVE REPLAY: a saved drive (the WiGLE CSV and GPX STOP writes) read
   back and played through the session. */
#include "wardrive/wd_core.h"
#include "wardrive/wd_feed.h"
#include "wardrive/wd_replay.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char dir[256];
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
/* Memory the way the board has it: the next few asks can fail while the 3D
   view and the map give theirs back, or nothing above a size fits. */
static int alloc_fail_next;
static size_t alloc_limit = (size_t)-1, alloc_first;
void *wd_feed_alloc(size_t bytes)
{
    if (!alloc_first) alloc_first = bytes;
    if (alloc_fail_next > 0) { alloc_fail_next--; return NULL; }
    if (bytes > alloc_limit) return NULL;
    return calloc(1, bytes);
}
void wd_feed_free(void *p) { free(p); }
bool wd_feed_card(void) { return true; }
const char *wd_feed_dir(void) { return dir; }
const char *wd_feed_board(void) { return "bench"; }
bool wd_feed_upload(const char *path) { (void)path; return false; }
bool wd_feed_hub(char *out, size_t cap, bool *busy) { (void)out; (void)cap; (void)busy; return false; }

static void put_file(const char *name, const char *text)
{
    char p[300];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

int main(void)
{
    /* text */
    assert(wd_replay_utc("2026-11-22 10:00:00") == 1795341600L);
    assert(wd_replay_utc("2026-11-22T10:00:05Z") == 1795341605L);
    assert(wd_replay_utc("garbage") < 0);
    char line[] = "aa:bb:cc:dd:ee:ff,\"Cafe, \"\"Free\"\"\",[WPA2-PSK-CCMP][ESS],2026-11-22 10:00:00,6,-61,43.1,-71.2,,,WIFI";
    char *fl[12];
    assert(wd_replay_csv_fields(line, fl, 12) == 11);
    assert(!strcmp(fl[1], "Cafe, \"Free\"") && !strcmp(fl[10], "WIFI") && !strcmp(fl[5], "-61"));
    assert(wd_replay_auth("[ESS]") == WD_AUTH_OPEN);
    assert(wd_sec_class(wd_replay_auth("[WPA2-PSK-CCMP][ESS]")) == WD_SEC_WPA2);
    assert(wd_sec_class(wd_replay_auth("[WPA3-SAE-CCMP][ESS]")) == WD_SEC_WPA3);

    /* a drive: four track points a minute, three networks, one written twice, a cell */
    snprintf(dir, sizeof(dir), "%s", "test_wd_replay_dir");
#ifdef _WIN32
    mkdir(dir);
#else
    mkdir(dir, 0777);
#endif
    put_file("2026-11-22_100000.gpx",
          "<?xml version=\"1.0\"?>\n<gpx>\n"
          "<wpt lat=\"43.1010000\" lon=\"-71.2000000\"><time>2026-11-22T10:01:00Z</time><name>EARFCN 5230 PCI 7</name>"
          "<desc>LTE cell, strongest -88 dBm, heard 3 times</desc><type>cell</type></wpt>\n"
          "<trk><name>x</name><trkseg>\n"
          "<trkpt lat=\"43.1000000\" lon=\"-71.2000000\"><time>2026-11-22T10:00:00Z</time></trkpt>\n"
          "<trkpt lat=\"43.1005000\" lon=\"-71.2000000\"><time>2026-11-22T10:01:00Z</time></trkpt>\n"
          "<trkpt lat=\"43.1010000\" lon=\"-71.2000000\"><time>2026-11-22T10:02:00Z</time></trkpt>\n"
          "<trkpt lat=\"43.1015000\" lon=\"-71.2000000\"><time>2026-11-22T10:03:00Z</time></trkpt>\n"
          "</trkseg></trk>\n</gpx>\n");
    put_file("2026-11-22_100000.csv",
          "WigleWifi-1.4,appRelease=WARDRIVE-1.0\nMAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type\n"
          "00:11:22:33:44:01,Home,[WPA2-PSK-CCMP][ESS],2026-11-22 10:00:00,6,-70,43.1000000,-71.2000000,,,WIFI\n"
          "00:11:22:33:44:02,Open,[ESS],2026-11-22 10:01:00,1,-60,43.1005000,-71.2000000,,,WIFI\n"
          "00:11:22:33:44:01,Home,[WPA2-PSK-CCMP][ESS],2026-11-22 10:00:00,6,-55,43.1010000,-71.2000000,,,WIFI\n"
          "aa:bb:cc:00:00:03,Tag,Misc [LE],2026-11-22 10:02:30,0,-80,43.1012000,-71.2000000,,,BLE\n");
    put_file("2026-11-21_090000.csv", "WigleWifi-1.4\n");

    char names[WD_REPLAY_LIST][WD_REPLAY_NAME], det[WD_REPLAY_LIST][40];
    const int n = wd_replay_list(names, det, WD_REPLAY_LIST);
    assert(n == 2 && !strcmp(names[0], "2026-11-22_100000") && strstr(det[0], "track + log"));

    char why[64];
    assert(!wd_replay_open("nope", 1000, why, sizeof(why)));
    assert(wd_replay_open(names[0], 1000, why, sizeof(why)));
    wd_replay_info_t ri;
    wd_replay_info(&ri);
    /* read a slice a frame, not at once */
    assert(ri.loading && !ri.on && wd_replay_on());
    for (int i = 0; i < 10 && (wd_replay_info(&ri), ri.loading); i++) wd_replay_tick(1000);
    wd_replay_info(&ri);
    assert(ri.on && ri.points == 4 && ri.rows == 4 && ri.marks == 1 && ri.len_s == 180);
    assert(wd_stats()->running && wd_stats()->replay);
    /* at the start only the first point and the first network */
    assert(wd_dev_count() == 1 && wd_stats()->origin);
    /* half way (90 s): two networks, the cell, part of the route */
    wd_replay_seek(0.5f, 1000);
    assert(wd_dev_count() == 3);
    const int half_trail = wd_trail_count();
    assert(half_trail >= 2);
    /* the clock the screen draws by follows the playback */
    assert(wd_replay_now(1000) == 1000 + 90000);
    /* play to the end at 64x */
    wd_replay_speed(+1, 1000); wd_replay_speed(+1, 1000);
    uint32_t t = 1000;
    for (int i = 0; i < 400 && !(wd_replay_info(&ri), ri.done); i++) wd_replay_tick(t += 100);
    assert(ri.done && wd_dev_count() == 4 && wd_trail_count() >= half_trail);
    /* the twice written network kept its strongest, where it was strongest */
    int home = -1;
    for (int i = 0; i < wd_dev_count(); i++) if (!strcmp(wd_dev_at(i)->name, "Home")) home = i;
    assert(home >= 0 && wd_dev_at(home)->rssi == -55 && fabs(wd_dev_at(home)->lat - 43.101) < 1e-6);
    assert(wd_stats()->total[WD_WIFI] == 2 && wd_stats()->total[WD_BLE] == 1 && wd_stats()->total[WD_CELL] == 1);
    assert(wd_stats()->dist_m > 150 && wd_stats()->dist_m < 200);
    /* back to the start: the session is begun again and fed up to there */
    wd_replay_seek(0, t);
    assert(wd_dev_count() == 1);
    wd_replay_close();
    assert(!wd_stats()->running && !wd_stats()->replay && !wd_replay_on());
    /* a live START is possible afterwards */
    const bool src[WD_KINDS] = { true, true, true, true };
    assert(wd_begin(5000, src, false));
    wd_end(6000);

    /* Short of memory for a moment (the 3D view and the map still giving
       theirs back): the replay waits for it, it does not refuse. */
    alloc_fail_next = 3;
    assert(wd_replay_open(names[0], 10000, why, sizeof(why)));
    wd_replay_info(&ri);
    assert(ri.loading && !ri.on);
    t = 10000;
    for (int i = 0; i < 20 && (wd_replay_info(&ri), ri.loading); i++) wd_replay_tick(t += 100);
    wd_replay_info(&ri);
    assert(ri.on && ri.points == 4 && ri.rows == 4 && ri.marks == 1 && !wd_replay_error()[0]);
    wd_replay_close();

    /* Never enough for the whole bound, enough for half: once the wait is
       over it plays what fits rather than nothing. */
    alloc_first = 0;
    alloc_limit = 0;
    assert(wd_replay_open(names[0], 20000, why, sizeof(why)));
    const size_t whole = alloc_first;
    assert(whole > 0);
    alloc_limit = whole * 6 / 10;
    t = 20000;
    for (int i = 0; i < 200 && (wd_replay_info(&ri), ri.loading); i++) wd_replay_tick(t += 100);
    wd_replay_info(&ri);
    assert(ri.on && ri.points == 4 && ri.rows == 4);
    wd_replay_close();

    /* Nothing at all: it says so once the wait is over, and is not left reading. */
    alloc_limit = 0;
    assert(wd_replay_open(names[0], 40000, why, sizeof(why)));
    t = 40000;
    for (int i = 0; i < 200 && (wd_replay_info(&ri), ri.loading); i++) wd_replay_tick(t += 100);
    wd_replay_info(&ri);
    assert(!ri.loading && !ri.on && !wd_replay_on());
    assert(strstr(wd_replay_error(), "memory"));
    alloc_limit = (size_t)-1;

    puts("PASS: replay reads the CSV and GPX, seeks both ways, plays to the end and hands back; waits for memory");
    return 0;
}
