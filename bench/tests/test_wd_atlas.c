/* WARDRIVE ATLAS: the memory across drives. Runs of one route are
   recognised and numbered, devices come back REGULAR or NEWHERE, regulars
   not heard are MISSING, a moved one is MOVED; a cut log loses only its
   partial tail; two atlases trade records both ways and agree, and the same
   records twice change nothing; old drives on the card IMPORT once. */
#include "wardrive/wd_atlas.h"
#include "wardrive/wd_core.h"
#include "wardrive/wd_feed.h"
#include <assert.h>
#include <direct.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char root[200], dir[256];
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
bool wd_feed_card(void) { return true; }
const char *wd_feed_dir(void) { return dir; }
const char *wd_feed_board(void) { return "bench"; }
bool wd_feed_upload(const char *path) { (void)path; return false; }
bool wd_feed_hub(char *out, size_t cap, bool *busy) { (void)out; (void)cap; (void)busy; return false; }

/* ---- the drive ---- */
#define LAT0 43.0005
#define LON0 -71.0
#define SPAN 0.0625                 /* 40 grid cells east, about 5 km */
#define NDEV 60
typedef struct { double lat, lon; uint8_t mac[6]; } dev_t_;
static dev_t_ DEVS[NDEV];
static uint32_t clk = 1000;
/* wd_atlas_locate checked halfway along a drive when set: the error, metres */
static bool g_locate;
static double g_loc_err = -1;
static int g_loc_used;

static void use_dir(const char *leaf)
{
    snprintf(dir, sizeof(dir), "%s/%s", root, leaf);
    _mkdir(dir);
    wd_atlas_reload();
}

static void settle(void)
{
    for (int i = 0; i < 20000 && wd_atlas_pending(); i++) { clk += 250; wd_atlas_step(clk); }
    assert(!wd_atlas_pending());
}

static void build(void)
{
    for (int i = 0; i < NDEV; i++) {
        DEVS[i].lat = LAT0 + 0.0003;
        DEVS[i].lon = LON0 + (i % 50 + 0.5) * SPAN / 50;
        DEVS[i].mac[0] = 0x10; DEVS[i].mac[1] = 0x20; DEVS[i].mac[2] = 0x30;
        DEVS[i].mac[3] = 0; DEVS[i].mac[4] = (uint8_t)(i >> 8); DEVS[i].mac[5] = (uint8_t)i;
    }
}

/* One drive: east along the route (north = true: north from its start),
   hearing the devices in `heard` within 200 m. `moved`: one device heard at
   the far end instead of its place. Returns the info just before STOP. */
static wd_atlas_info_t drive(bool north, const bool heard[NDEV], int moved)
{
    const bool src[WD_KINDS] = { true, true, true, true, true, true, true };
    clk += 60000;
    assert(wd_begin(clk, src, false));
    const int steps = 400;
    for (int s = 0; s <= steps; s++) {
        clk += 1000;
        wd_fix_t f;
        memset(&f, 0, sizeof(f));
        f.fix = true;
        f.lat = north ? LAT0 + SPAN * s / steps : LAT0;
        f.lon = north ? LON0 : LON0 + SPAN * s / steps;
        f.alt_m = 50; f.hdop = 1; f.speed_mps = 13; f.course_deg = 90; f.compass_deg = -1; f.sats = 9;
        f.utc = 1795000000u + clk / 1000;
        wd_fix_note(&f, clk);
        for (int i = 0; i < NDEV; i++) {
            if (!heard[i]) continue;
            double lat = DEVS[i].lat, lon = DEVS[i].lon;
            if (i == moved) lon = LON0 + SPAN * 0.95;
            const double dy = (lat - f.lat) * 111195, dx = (lon - f.lon) * 111195 * cos(lat * M_PI / 180), d = hypot(dx, dy);
            if (d > 200) continue;
            wd_obs_t o;
            memset(&o, 0, sizeof(o));
            o.kind = WD_WIFI;
            memcpy(o.addr, DEVS[i].mac, 6);
            o.auth = WD_AUTH_WPA2;
            o.channel = 6;
            o.rssi = (int8_t)(-40 - 20 * log10(d < 1 ? 1 : d));
            snprintf(o.name, sizeof(o.name), "AP-%02d", i);
            wd_note(&o, clk);
        }
        if (g_locate && s == steps / 2) {
            /* where the access points heard here say the board is */
            wd_obs_t ob[NDEV];
            int k = 0;
            for (int i = 0; i < NDEV; i++) {
                const double dy = (DEVS[i].lat - f.lat) * 111195, dx = (DEVS[i].lon - f.lon) * 111195 * cos(f.lat * M_PI / 180);
                const double d = hypot(dx, dy);
                if (d > 200) continue;
                memset(&ob[k], 0, sizeof(ob[k]));
                ob[k].kind = WD_WIFI;
                memcpy(ob[k].addr, DEVS[i].mac, 6);
                ob[k].rssi = (int8_t)(-40 - 20 * log10(d < 1 ? 1 : d));
                k++;
            }
            double la, lo;
            float err;
            if (wd_atlas_locate(ob, k, &la, &lo, &err, &g_loc_used)) {
                const double dy = (la - f.lat) * 111195, dx = (lo - f.lon) * 111195 * cos(f.lat * M_PI / 180);
                g_loc_err = hypot(dx, dy);
                printf("locate: %d access points, %.0f m from the GPS (says +-%.0f m)\n", g_loc_used, g_loc_err, (double)err);
            }
        }
        wd_atlas_step(clk);
    }
    wd_atlas_info_t in;
    wd_atlas_info(&in);
    wd_end(clk);
    settle();
    return in;
}

static const wd_dev_t *dev_of(int i)
{
    for (int k = 0; k < wd_dev_count(); k++) {
        const wd_dev_t *e = wd_dev_at(k);
        if (!memcmp(e->addr, DEVS[i].mac, 6)) return e;
    }
    return NULL;
}

/* ---- the device index as the format says ---- */
typedef struct { char magic[4]; uint32_t ver, count, seq, log_off, kind[8], crc; } dev_hdr_t;
typedef struct {
    uint8_t kind, radios, nlen, sub, addr[6];
    uint16_t tags;
    uint32_t first, last, name_utc;
    int32_t lat, lon, alat, alon;
    float w;
    uint32_t seen;
    uint16_t drives;
    int8_t rssi;
    uint8_t flags;
    uint32_t last_run;
    char name[32];
} adev_t;
static bool idx_find(const char *leaf, const uint8_t mac[6], adev_t *out, uint32_t *count)
{
    char p[300];
    snprintf(p, sizeof(p), "%s/%s/atlas/dev.idx", root, leaf);
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    dev_hdr_t h;
    bool found = false;
    if (fread(&h, sizeof(h), 1, f) == 1) {
        if (count) *count = h.count;
        adev_t d;
        for (uint32_t i = 0; i < h.count && fread(&d, sizeof(d), 1, f) == 1; i++)
            if (!memcmp(d.addr, mac, 6)) { *out = d; found = true; }
    }
    fclose(f);
    return found;
}

static long fsize(const char *p)
{
    struct stat st;
    return stat(p, &st) ? -1 : (long)st.st_size;
}

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
    setvbuf(stdout, NULL, _IONBF, 0);
    snprintf(root, sizeof(root), "%s/test_wd_atlas", getenv("TEMP") ? getenv("TEMP") : ".");
    _mkdir(root);
    /* fresh directories every run */
    static const char *const leaves[] = { "a", "b", "c", "d" };
    for (int i = 0; i < 4; i++) {
        static const char *const files[] = { "log.bin", "state.bin", "state.tmp", "dev.idx", "dev.tmp", "atlas.id" };
        for (int k = 0; k < 6; k++) { char p[300]; snprintf(p, sizeof(p), "%s/%s/atlas/%s", root, leaves[i], files[k]); remove(p); _rmdir(p); }
        char p[300]; snprintf(p, sizeof(p), "%s/%s/WD_20261101_080000.gpx", root, leaves[i]); remove(p);
        snprintf(p, sizeof(p), "%s/%s/WD_20261101_080000.csv", root, leaves[i]); remove(p);
    }
    build();
    bool heard[NDEV];
    wd_atlas_info_t in;
    wd_atlas_route_t rt;

    /* ---- record format: a record survives its own framing; the same bytes twice merge once ---- */
    use_dir("a");
    settle();
    wd_atlas_info(&in);
    assert(in.state == WD_ATLAS_READY && in.board && in.runs == 0 && in.routes == 0);
    const uint32_t board_a = in.board;

    /* ---- run 1: a new route ---- */
    for (int i = 0; i < NDEV; i++) heard[i] = i < 50;
    in = drive(false, heard, -1);
    assert(in.live && in.run_dev == 50);
    assert(in.route == -1);                         /* nothing known yet */
    wd_atlas_info(&in);
    assert(in.runs == 1 && in.routes == 1 && in.devices == 50 && in.tail == 0);
    assert(wd_atlas_route_info(0, &rt) && rt.runs == 1 && rt.best.ndev == 50 && rt.cells >= 38);
    printf("run 1: %u devices, route %s %u cells\n", in.devices, rt.name, rt.cells);

    /* ---- run 2: the same route, ten gone, ten new ---- */
    for (int i = 0; i < NDEV; i++) heard[i] = i < 40 || i >= 50;
    in = drive(false, heard, -1);
    printf("run 2 live: route %d run %u dev %u new %u reg %u known %d cells %u gain %u\n", in.route, in.run_num, in.run_dev,
           in.run_new, in.run_reg, in.known_ready, in.run_cells, in.run_gain);
    {
        wd_atlas_item_t it[32];
        const int k = wd_atlas_list(WD_ATLAS_L_NEW, it, 32);
        for (int i = 0; i < k; i++) printf(" new: %s\n", it[i].name);
    }
    assert(in.route == 0 && in.run_num == 2);       /* recognised while driving */
    assert(in.run_new == 10);
    assert(dev_of(55) && (dev_of(55)->tags & WD_T_NEWHERE));
    assert(dev_of(3) && !(dev_of(3)->tags & (WD_T_NEWHERE | WD_T_REGULAR)));   /* one earlier drive is not regular yet */
    wd_atlas_info(&in);
    assert(in.runs == 2 && in.routes == 1 && in.devices == 60);
    assert(wd_atlas_route_info(0, &rt) && rt.runs == 2 && rt.last.num == 2);

    /* ---- run 3: regulars, and a position from them ---- */
    g_locate = true;
    for (int i = 0; i < NDEV; i++) heard[i] = i < 30;
    in = drive(false, heard, -1);
    g_locate = false;
    assert(g_loc_err >= 0 && g_loc_err < 60 && g_loc_used >= 3);   /* known access points place the board */
    assert(dev_of(3) && (dev_of(3)->tags & WD_T_REGULAR));
    assert(in.run_reg == 30 && in.run_new == 0);
    adev_t d;
    uint32_t cnt = 0;
    assert(idx_find("a", DEVS[3].mac, &d, &cnt) && d.drives == 3 && cnt == 60);
    assert(idx_find("a", DEVS[45].mac, &d, NULL) && d.drives == 1);
    assert(!strncmp(d.name, "AP-45", 5));
    assert(wd_atlas_route_info(0, &rt) && !strcmp(rt.name, "COMMUTE") && rt.runs == 3);

    /* ---- run 4: ten regulars missing, one moved ---- */
    for (int i = 0; i < NDEV; i++) heard[i] = i < 20;
    in = drive(false, heard, 5);
    printf("run 4 live: route %d run %u dev %u new %u reg %u missing %u moved %u\n", in.route, in.run_num, in.run_dev,
           in.run_new, in.run_reg, in.run_missing, in.run_moved);
    {
        wd_atlas_item_t it[64];
        const int k = wd_atlas_list(WD_ATLAS_L_REGULAR, it, 64);
        printf(" regulars seen %d:", k);
        for (int i = 0; i < k; i++) printf(" %s", it[i].name);
        printf("\n");
    }
    assert(in.run_missing == 10);
    assert(in.run_moved == 1);
    wd_atlas_item_t items[32];
    int n = wd_atlas_list(WD_ATLAS_L_MISSING, items, 32);
    assert(n == 10 && items[0].drives == 3);
    n = wd_atlas_list(WD_ATLAS_L_MOVED, items, 32);
    assert(n == 1 && !memcmp(items[0].addr, DEVS[5].mac, 6) && items[0].dist_m > 2000);
    assert(idx_find("a", DEVS[5].mac, &d, NULL) && d.drives == 4);

    /* ---- run 5: a different road is a new route ---- */
    for (int i = 0; i < NDEV; i++) heard[i] = false;
    in = drive(true, heard, -1);
    wd_atlas_info(&in);
    assert(in.routes == 2 && in.runs == 5);
    printf("five runs: %u devices, %d routes, %u cells, %u km, seq %u\n", in.devices, in.routes, in.cells, in.km, in.seq);

    /* ---- crash-safe: a partial record at the end is dropped, the next write goes over it ---- */
    char lp[300];
    snprintf(lp, sizeof(lp), "%s/a/atlas/log.bin", root);
    const uint32_t seq_a = in.seq;
    const long size_a = fsize(lp);
    {
        FILE *f = fopen(lp, "ab");
        static const uint8_t junk[] = { 0xA7, 3, 60, 0x99, 0x99, 0, 0, 1, 2, 3, 4, 5 };   /* a DEV record cut short */
        fwrite(junk, 1, sizeof(junk), f);
        fclose(f);
    }
    wd_atlas_reload();
    settle();
    wd_atlas_info(&in);
    assert(in.state == WD_ATLAS_READY && in.seq == seq_a && in.runs == 5 && in.devices == 60);
    /* lose the snapshot and the index too: the log alone rebuilds them */
    {
        char p[300];
        snprintf(p, sizeof(p), "%s/a/atlas/state.bin", root); remove(p);
        snprintf(p, sizeof(p), "%s/a/atlas/dev.idx", root); remove(p);
    }
    wd_atlas_reload();
    settle();
    wd_atlas_info(&in);
    assert(in.seq == seq_a && in.runs == 5 && in.routes == 2 && in.devices == 60);
    assert(idx_find("a", DEVS[3].mac, &d, NULL) && d.drives == 4);
    /* a cut in the middle of the last record: that record is gone, nothing else */
    {
        FILE *f = fopen(lp, "rb");
        long sz = fsize(lp);
        uint8_t *all = malloc((size_t)sz);
        assert(fread(all, 1, (size_t)sz, f) == (size_t)sz);
        fclose(f);
        f = fopen(lp, "wb");
        fwrite(all, 1, (size_t)size_a - 5, f);   /* the junk and five bytes of the last record */
        fclose(f);
        free(all);
    }
    wd_atlas_reload();
    settle();
    wd_atlas_info(&in);
    /* the cut record was run 5's end: the run is closed where it stopped (one new record) */
    assert(in.state == WD_ATLAS_READY && in.seq == seq_a && in.runs == 5 && in.routes == 2);
    printf("crash tail: seq %u -> %u, %u runs\n", seq_a, in.seq, in.runs);
    /* and it keeps working: drive again, then reload */
    for (int i = 0; i < NDEV; i++) heard[i] = i < 10;
    drive(false, heard, -1);
    wd_atlas_reload();
    settle();
    wd_atlas_info(&in);
    const uint32_t runs_a = in.runs, seq_a2 = in.seq;
    assert(in.state == WD_ATLAS_READY && runs_a >= 5);

    /* ---- a log the card will not open right now (a flaky card, no free
       file handle) is not a missing log: nothing is wiped, and the next
       try finds every run again ---- */
    {
        char bak[300], sp[300], ip[300];
        snprintf(bak, sizeof(bak), "%s/a/atlas/log.bak", root);
        snprintf(sp, sizeof(sp), "%s/a/atlas/state.bin", root);
        snprintf(ip, sizeof(ip), "%s/a/atlas/dev.idx", root);
        assert(fsize(sp) > 0 && fsize(ip) > 0);
        remove(bak);
        assert(rename(lp, bak) == 0);
        assert(_mkdir(lp) == 0);                 /* fopen fails, and not with ENOENT */
        wd_atlas_reload();
        for (int i = 0; i < 40; i++) { clk += 250; wd_atlas_step(clk); }
        wd_atlas_info(&in);
        assert(in.state != WD_ATLAS_READY);
        assert(fsize(sp) > 0 && fsize(ip) > 0);  /* the snapshot and the index are still there */
        assert(_rmdir(lp) == 0);
        assert(rename(bak, lp) == 0);
        wd_atlas_reload();
        settle();
        wd_atlas_info(&in);
        assert(in.state == WD_ATLAS_READY && in.runs == runs_a && in.seq == seq_a2);
    }

    /* ---- sync: B drives its own run, then A and B trade both ways ---- */
    use_dir("b");
    settle();
    wd_atlas_info(&in);
    const uint32_t board_b = in.board;
    assert(board_b && board_b != board_a);
    for (int i = 0; i < NDEV; i++) heard[i] = i >= 30;
    drive(false, heard, -1);
    wd_atlas_info(&in);
    assert(in.runs == 1 && in.devices == 30);
    const uint32_t seq_b = in.seq;

    /* pull A's records in small pieces, the way a radio link would */
    static uint8_t chunks[2000][300];
    static size_t clen[2000];
    int nch = 0;
    use_dir("a");
    for (uint32_t since = 0, next = 0;; since = next) {
        assert(nch < 2000);
        clen[nch] = wd_atlas_delta(since, chunks[nch], 300, &next);
        if (!clen[nch]) break;
        assert(next > since);
        nch++;
    }
    assert(nch > 10);
    use_dir("b");
    int merged = 0;
    for (int i = 0; i < nch; i++) { const int m = wd_atlas_apply(chunks[i], clen[i], board_a); assert(m >= 0); merged += m; }
    settle();
    wd_atlas_info(&in);
    printf("A -> B: %d chunks, %d records merged, B now %u runs %u devices\n", nch, merged, in.runs, in.devices);
    assert(in.runs == 1 + runs_a && in.devices == 60);
    /* the same chunks again: nothing changes */
    const uint32_t seq_b2 = in.seq;
    for (int i = 0; i < nch; i++) assert(wd_atlas_apply(chunks[i], clen[i], board_a) == 0);
    settle();
    wd_atlas_info(&in);
    assert(in.seq == seq_b2 && in.runs == 1 + runs_a);
    /* a malformed buffer is refused whole */
    {
        uint8_t bad[300];
        memcpy(bad, chunks[0], clen[0]);
        bad[clen[0] / 2] ^= 0x5A;
        assert(wd_atlas_apply(bad, clen[0], board_a) == -1);
        assert(wd_atlas_apply((const uint8_t *)"nonsense", 8, board_a) == -1);
    }
    /* B's whole log back to A: only B's own run is new there */
    nch = 0;
    for (uint32_t since = 0, next = 0;; since = next) {
        clen[nch] = wd_atlas_delta(since, chunks[nch], 300, &next);
        if (!clen[nch]) break;
        nch++;
    }
    use_dir("a");
    merged = 0;
    for (int i = 0; i < nch; i++) merged += wd_atlas_apply(chunks[i], clen[i], board_b);
    settle();
    wd_atlas_info(&in);
    printf("B -> A: %d records merged (B made %u), A now %u runs\n", merged, seq_b, in.runs);
    assert((uint32_t)merged == seq_b);
    assert(in.runs == runs_a + 1 && in.seq == seq_a2 + (uint32_t)merged);
    /* both hold the same totals for a device both drove past */
    adev_t da, db;
    assert(idx_find("a", DEVS[35].mac, &da, NULL) && idx_find("b", DEVS[35].mac, &db, NULL));
    printf("AP-35 a: %u drives %u seen %u..%u  b: %u drives %u seen %u..%u\n", da.drives, da.seen, da.first, da.last,
           db.drives, db.seen, db.first, db.last);
    assert(da.drives == db.drives && da.seen == db.seen && da.first == db.first && da.last == db.last);
    assert(da.lat == db.lat && da.lon == db.lon && da.drives >= 3);
    assert(idx_find("a", DEVS[3].mac, &da, NULL) && idx_find("b", DEVS[3].mac, &db, NULL) && da.drives == db.drives && da.seen == db.seen);
    printf("converged: AP-03 %u drives %u sightings on both\n", da.drives, da.seen);

    /* ---- IMPORT: an old drive on the card counts once ---- */
    use_dir("c");
    settle();
    {
        char gpx[40000], csv[8000];
        int g = snprintf(gpx, sizeof(gpx), "<?xml version=\"1.0\"?>\n<gpx version=\"1.1\"><trk><name>x</name><trkseg>\n");
        for (int s = 0; s <= 200; s++)
            g += snprintf(gpx + g, sizeof(gpx) - (size_t)g, "<trkpt lat=\"%.7f\" lon=\"%.7f\"><time>2026-11-01T08:%02d:%02dZ</time></trkpt>\n",
                          LAT0, LON0 + SPAN * s / 200, s * 2 / 60, s * 2 % 60);
        g += snprintf(gpx + g, sizeof(gpx) - (size_t)g, "</trkseg></trk>\n<wpt lat=\"%.6f\" lon=\"%.6f\"><name>LTE 5230 PCI 100</name><type>cell</type><desc>strongest -71 dBm</desc></wpt>\n</gpx>\n", LAT0, LON0);
        put_file("WD_20261101_080000.gpx", gpx);
        int c = snprintf(csv, sizeof(csv), "WigleWifi-1.4,appRelease=1\nMAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type\n");
        for (int i = 0; i < 12; i++)
            c += snprintf(csv + c, sizeof(csv) - (size_t)c, "10:20:30:00:00:%02x,AP-%02d,[WPA2-PSK-CCMP][ESS],2026-11-01 08:00:%02d,6,-6%d,%.7f,%.7f,50,5,WIFI\n",
                          i, i, i, i % 10, DEVS[i].lat, DEVS[i].lon);
        put_file("WD_20261101_080000.csv", csv);
    }
    wd_atlas_request(WD_ATLAS_REQ_COUNT);
    settle();
    wd_atlas_info(&in);
    assert(in.import_left == 1);
    wd_atlas_request(WD_ATLAS_REQ_IMPORT);
    settle();
    wd_atlas_info(&in);
    printf("import: %s; %u runs %u devices %d routes\n", in.note, in.runs, in.devices, in.routes);
    assert(in.runs == 1 && in.devices == 13 && in.routes == 1 && in.import_left == 0);
    assert(wd_atlas_route_info(0, &rt) && rt.best.ndev == 13 && rt.cells >= 38);
    wd_atlas_request(WD_ATLAS_REQ_IMPORT);
    settle();
    wd_atlas_info(&in);
    assert(in.runs == 1);
    /* ...and a live drive of the same road is its run 2 */
    for (int i = 0; i < NDEV; i++) heard[i] = i < 12;
    in = drive(false, heard, -1);
    assert(in.route == 0 && in.run_num == 2 && in.run_new == 0);
    assert(dev_of(1) && !(dev_of(1)->tags & WD_T_NEWHERE));

    /* ---- a full device table: what is heard still goes to the log about
       once a minute, not on every pass (which wrote a record per device
       per pass and synced the card four times a second) ---- */
    {
        use_dir("d");
        settle();
        const bool src[WD_KINDS] = { true, true, true, true, true, true, true };
        clk += 60000;
        assert(wd_begin(clk, src, false));
        wd_fix_t f;
        memset(&f, 0, sizeof(f));
        f.fix = true; f.lat = LAT0; f.lon = LON0; f.alt_m = 50; f.hdop = 1; f.compass_deg = -1; f.sats = 9; f.course_deg = -1;
        wd_obs_t o;
        memset(&o, 0, sizeof(o));
        o.kind = WD_WIFI; o.auth = WD_AUTH_WPA2; o.channel = 6; o.rssi = -70;
        for (int k = 0; k < WD_CAP; k++) {
            o.addr[0] = 0x50; o.addr[4] = (uint8_t)(k >> 8); o.addr[5] = (uint8_t)k;
            wd_note(&o, clk);
            if (k % 256 == 255) { clk += 250; f.utc = 1795000000u + clk / 1000; wd_fix_note(&f, clk); wd_atlas_step(clk); }
        }
        assert(wd_dev_count() == WD_CAP);
        for (int pass = 0; pass < 8; pass++) { clk += 250; f.utc = 1795000000u + clk / 1000; wd_fix_note(&f, clk); wd_atlas_step(clk); }
        char dp[300];
        snprintf(dp, sizeof(dp), "%s/d/atlas/log.bin", root);
        const long s0 = fsize(dp);
        for (int pass = 0; pass < 40; pass++) {
            clk += 250;
            f.utc = 1795000000u + clk / 1000;
            wd_fix_note(&f, clk);
            for (int k = 0; k < 10; k++) { o.addr[4] = 0; o.addr[5] = (uint8_t)k; wd_note(&o, clk); }
            wd_atlas_step(clk);
        }
        const long s1 = fsize(dp);
        printf("full table: log grew %ld bytes over 40 passes\n", s1 - s0);
        assert(s1 - s0 < 2000);
        wd_end(clk);
        settle();
    }

    printf("test_wd_atlas: all passed\n");
    return 0;
}
