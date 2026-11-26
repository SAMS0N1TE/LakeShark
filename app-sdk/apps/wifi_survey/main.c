/* WIFI SURVEY: the access points around, how crowded each channel is, and
   how each signal moves. Passive: it listens to scans and beacons and never
   joins, probes by name or transmits on its own.

   Two sources, either enough on its own:
   - lap_wifi_scan / lap_wifi_aps: SSID, channel, RSSI, open or secured.
     The C6 scans 2.4 GHz; a dual-band coprocessor (the Nano's C5) adds 5 GHz.
   - lap_wifi_capture_* (API 2.7), hopping channels: BSSIDs and the security
     suite from beacons, and per-channel frame counts and airtime. Without
     the C6 forwarder firmware this part says why and how to install it.
   CSV files go to /sdcard/wifi_survey.

   RECORD is the survey recorder that was LINK's SURVEY tab: every access
   point (by BSSID, from beacons) and Bluetooth device heard, with where the
   board was, as WiGLE CSV in /sdcard/lakeshark/survey (survey_rec.h). It
   keeps recording behind HOME as a background service. */
#include "lakeshark_app.h"
#include "survey_rec.h"
#include "lap_csv.h"
#include "lap_dot11.h"
#include "lakeshark_ui.h"
#include "lap_scroll.h"
#include "lap_art.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LAP_ICON_PX("....................", "....................", "..aaaaaaaaaaaaaaaa..", "..a..............a..",
            "..a.b............a..", "..a.b......c.....a..", "..a.b.....cc.....a..", "..a.bb....cc..e..a..",
            "..a.bb.c..cc..e..a..", "..a.bb.c.ccc.ee..a..", "..a.bbbc.ccc.ee.fa..", "..a.bbbcccccceeffa..",
            "..a..............a..", "..aaaaaaaaaaaaaaaa..", "....................", "....................",
            "....................", "....................");

#define APS 48
#define HISTN 64            /* RSSI history: one sample per SLOT_MS */
#define SLOT_MS 2000u
#define NOSIG (-128)
#define CHN 178             /* channel numbers 0..177 */

typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    bool has_bssid, secure, by_scan, by_capture;
    uint8_t channel, sec;
    int8_t rssi, rmin, rmax;
    uint32_t first_ms, last_ms, beacons, hslot;
    uint32_t flash_ms;          /* when its signal last pulsed on screen: at most every 1.5 s */
    int8_t hist[HISTN];
} ap_t;
typedef struct { uint32_t frames, air_us, listened_ms; } chan_t;

static ap_t *ap;
static chan_t *ch;
static int naps, over;
static int order[APS], nsorted;

/* RECORD's intake, defined with the recorder below. */
static void rec_wifi(const uint8_t bssid[6], const char *ssid, uint8_t channel, uint8_t sec, int rssi);
static bool rec_wants_wifi(void);
static uint32_t scan_every_ms(void);

/* ---- the table ------------------------------------------------------------------ */
static uint32_t slot_now(void) { return lap_millis() / SLOT_MS; }
static void sample(ap_t *a, int rssi)
{
    const uint32_t s = slot_now();
    if (a->hslot != s || a->hist[s % HISTN] == NOSIG) {
        for (uint32_t k = a->hslot + 1; k < s && k <= a->hslot + HISTN; k++) a->hist[k % HISTN] = NOSIG;
        a->hslot = s;
        a->hist[s % HISTN] = (int8_t)rssi;
    } else if (rssi > a->hist[s % HISTN]) a->hist[s % HISTN] = (int8_t)rssi;
    a->rssi = (int8_t)rssi;
    if (rssi < a->rmin) a->rmin = (int8_t)rssi;
    if (rssi > a->rmax) a->rmax = (int8_t)rssi;
    a->last_ms = lap_millis();
    if (!a->flash_ms || a->last_ms - a->flash_ms > 1500u) a->flash_ms = a->last_ms ? a->last_ms : 1;
}
static ap_t *fresh(void)
{
    ap_t *a;
    if (naps < APS) a = &ap[naps++];
    else {
        int o = 0;
        for (int i = 1; i < naps; i++) if (ap[i].last_ms < ap[o].last_ms) o = i;
        a = &ap[o];
        over++;
    }
    memset(a, 0, sizeof(*a));
    for (int i = 0; i < HISTN; i++) a->hist[i] = NOSIG;
    a->rmin = 127; a->rmax = -128;
    a->first_ms = lap_millis();
    a->hslot = slot_now();
    return a;
}
/* A scan result: matched by name and channel, as the scan has no BSSID. */
static void from_scan(const lap_wifi_ap_t *s)
{
    ap_t *a = NULL;
    for (int i = 0; i < naps && !a; i++)
        if (ap[i].channel == s->channel && !strcmp(ap[i].ssid, s->ssid)) a = &ap[i];
    if (!a) { a = fresh(); snprintf(a->ssid, sizeof(a->ssid), "%.32s", s->ssid); a->channel = s->channel; }
    a->by_scan = true;
    a->secure = s->secure;
    sample(a, s->rssi);
    if (a->has_bssid) rec_wifi(a->bssid, s->ssid, s->channel, a->sec, s->rssi);
}
/* A beacon or probe response: matched by BSSID, else it names a scan row. */
static void from_beacon(const lap_dot11_t *f, int rssi, uint8_t heard_on)
{
    const uint8_t c = f->channel ? f->channel : heard_on;
    char name[33];
    snprintf(name, sizeof(name), "%s", f->ssid_hidden ? "" : f->ssid);
    ap_t *a = NULL;
    for (int i = 0; i < naps && !a; i++) if (ap[i].has_bssid && !memcmp(ap[i].bssid, f->bssid, 6)) a = &ap[i];
    for (int i = 0; i < naps && !a; i++) if (!ap[i].has_bssid && ap[i].channel == c && !strcmp(ap[i].ssid, name)) a = &ap[i];
    if (!a) a = fresh();
    if (!a->has_bssid) { memcpy(a->bssid, f->bssid, 6); a->has_bssid = true; }
    if (name[0] || !a->ssid[0]) snprintf(a->ssid, sizeof(a->ssid), "%s", name);
    a->channel = c;
    a->by_capture = true;
    a->sec = f->security;
    a->secure = f->security > LAP_DOT11_SEC_OPEN;
    if (f->subtype == LAP_DOT11_BEACON) a->beacons++;
    sample(a, rssi);
}
static int by_rssi(const void *x, const void *y)
{
    const ap_t *a = &ap[*(const int *)x], *b = &ap[*(const int *)y];
    const bool sa = lap_millis() - a->last_ms < 30000u, sb = lap_millis() - b->last_ms < 30000u;
    if (sa != sb) return sa ? -1 : 1;           /* heard lately first */
    return b->rssi - a->rssi;
}
static void sort(void)
{
    nsorted = naps;
    for (int i = 0; i < naps; i++) order[i] = i;
    qsort(order, (size_t)naps, sizeof(order[0]), by_rssi);
}
static const char *security(const ap_t *a) { return a->sec ? lap_dot11_sec_name(a->sec) : a->secure ? "secured" : "open"; }

/* ---- the radio ----------------------------------------------------------------------- */
enum { PLAN_HOP13, PLAN_HOP3, PLAN_STAY };
static int h, reason, plan, chan;
static bool drop_link, fell_back, paused;
static char why[72];
static uint32_t lost, frames, scan_ms, seq, last_draw, scans;
static int choose_h, confirm_h;

static void start(void)
{
    if (h > 0) lap_capture_stop(LAP_CAPTURE_WIFI);
    h = 0; why[0] = 0; reason = 0; fell_back = false;
    for (int attempt = 0; attempt < 2; attempt++) {
        lap_wifi_capture_config_t c = {.size = sizeof(c), .version = 1, .flags = drop_link ? LAP_CAPTURE_DROP_LINK : 0, .dwell_ms = 250, .snap_bytes = 256};
        const int p = attempt ? PLAN_STAY : plan;
        if (p == PLAN_HOP13) for (; c.channel_count < 13; c.channel_count++) c.channels[c.channel_count] = (uint8_t)(c.channel_count + 1);
        if (p == PLAN_HOP3) { c.channel_count = 3; c.channels[0] = 1; c.channels[1] = 6; c.channels[2] = 11; c.dwell_ms = 400; }
        const int r = lap_wifi_capture_start(&c);
        lap_capture_status_t st;
        if (!lap_capture_status(LAP_CAPTURE_WIFI, &st)) { reason = st.reason; snprintf(why, sizeof(why), "%s", st.why); }
        if (r > 0) { h = r; fell_back = attempt == 1; return; }
        if (!(r == LAP_IO_EBUSY && reason == LAP_CAPTURE_LINK_IN_USE)) return;
    }
}
static void poll(uint32_t budget_ms)
{
    static uint8_t buf[LAP_CAPTURE_SNAP_MAX];
    const uint32_t now = lap_millis();
    /* the scanner: a new scan every few seconds (RECORD's SCAN EVERY), read when it finishes */
    if (!paused && (!scan_ms || now - scan_ms >= scan_every_ms())) { lap_wifi_scan(); scan_ms = now ? now : 1; }
    const uint32_t s = lap_wifi_seq();
    if (s != seq) {
        seq = s;
        lap_wifi_ap_t list[16];
        const int n = lap_wifi_aps(list, 16);
        if (!paused) for (int i = 0; i < n; i++) from_scan(&list[i]);
        scans++;
    }
    if (h <= 0) { last_draw = now; return; }
    lap_capture_status_t st;
    if (!lap_capture_status(LAP_CAPTURE_WIFI, &st)) {
        lost = st.dropped;
        if (st.channel) chan = st.channel;
        reason = st.reason;
    }
    if (!paused && last_draw && chan < CHN) ch[chan].listened_ms += now - last_draw;
    last_draw = now;
    lap_capture_meta_t m;
    for (int i = 0, n; i < 256 && lap_millis() - now < budget_ms && (n = lap_capture_read(h, buf, sizeof(buf), &m)) >= 0; i++) {
        frames++;
        lost = m.dropped;
        if (m.channel >= CHN) continue;
        if (!paused) {
            ch[m.channel].frames++;
            ch[m.channel].air_us += lap_dot11_airtime_us(m.original_length, m.rate_500kbps, m.channel);
        }
        if (paused && !rec_wants_wifi()) continue;      /* PAUSE holds the tabs, not a recording */
        lap_dot11_t f;
        if (lap_dot11_parse(buf, (size_t)n, &f) && f.type == LAP_DOT11_MGMT && !f.protect &&
            (f.subtype == LAP_DOT11_BEACON || (f.subtype == LAP_DOT11_PROBE_RESP && f.ssid_len))) {
            if (!paused) from_beacon(&f, m.rssi, m.channel);
            rec_wifi(f.bssid, f.ssid_hidden ? "" : f.ssid, f.channel ? f.channel : m.channel, f.security, m.rssi);
        }
    }
}

/* ---- CSV --------------------------------------------------------------------------------- */
static struct { int stage, job; char base[64]; } save;
static char csv[8192];
static char status[96];
static uint32_t status_ms;
static bool status_bad;
static void say(bool bad, const char *fmt, ...)
{
    va_list ap_;
    va_start(ap_, fmt);
    vsnprintf(status, sizeof(status), fmt, ap_);
    va_end(ap_);
    status_bad = bad;
    status_ms = lap_millis();
}
static bool field(size_t *used, const char *v, bool end) { return lap_csv_write(csv, sizeof(csv), used, v, strlen(v), end); }
static bool fieldf(size_t *used, bool end, const char *fmt, ...)
{
    char t[48];
    va_list a;
    va_start(a, fmt);
    vsnprintf(t, sizeof(t), fmt, a);
    va_end(a);
    return field(used, t, end);
}
static size_t csv_aps(void)
{
    size_t u = 0;
    static const char *const head[11] = {"ssid", "bssid", "channel", "band", "rssi", "rssi_min", "rssi_max", "security", "heard_by", "first_s", "last_s"};
    for (int i = 0; i < 11; i++) field(&u, head[i], i == 10);
    sort();
    for (int k = 0; k < nsorted; k++) {
        const ap_t *a = &ap[order[k]];
        char b[18] = "";
        if (a->has_bssid) lap_dot11_mac(b, sizeof(b), a->bssid);
        const size_t keep = u;
        const bool ok = field(&u, a->ssid, false) && field(&u, b, false) && fieldf(&u, false, "%u", a->channel) &&
                        field(&u, a->channel > 14 ? "5" : "2.4", false) && fieldf(&u, false, "%d", a->rssi) &&
                        fieldf(&u, false, "%d", a->rmin) && fieldf(&u, false, "%d", a->rmax) && field(&u, security(a), false) &&
                        field(&u, a->by_scan && a->by_capture ? "scan+capture" : a->by_scan ? "scan" : "capture", false) &&
                        fieldf(&u, false, "%u", (unsigned)(a->first_ms / 1000u)) && fieldf(&u, true, "%u", (unsigned)(a->last_ms / 1000u));
        if (!ok) { u = keep; csv[u] = 0; break; }
    }
    return u;
}
static void chan_aps(int c, int *aps, int *best)
{
    *aps = 0; *best = NOSIG;
    for (int i = 0; i < naps; i++)
        if (ap[i].channel == c) { (*aps)++; if (ap[i].rssi > *best) *best = ap[i].rssi; }
}
static size_t csv_channels(void)
{
    size_t u = 0;
    static const char *const head[8] = {"channel", "mhz", "aps", "strongest_dbm", "frames", "airtime_ms", "listened_ms", "busy_percent"};
    for (int i = 0; i < 8; i++) field(&u, head[i], i == 7);
    for (int c = 1; c < CHN; c++) {
        int n, best;
        chan_aps(c, &n, &best);
        if (c > 13 && !n && !ch[c].frames) continue;
        if (!lap_dot11_channel_mhz((unsigned)c)) continue;
        const unsigned busy = ch[c].listened_ms ? (unsigned)((uint64_t)ch[c].air_us / 10u / ch[c].listened_ms) : 0;
        char bs[8] = "";
        if (n) snprintf(bs, sizeof(bs), "%d", best);
        const size_t keep = u;
        if (!(fieldf(&u, false, "%d", c) && fieldf(&u, false, "%u", lap_dot11_channel_mhz((unsigned)c)) && fieldf(&u, false, "%d", n) &&
              field(&u, bs, false) && fieldf(&u, false, "%u", (unsigned)ch[c].frames) && fieldf(&u, false, "%u", (unsigned)(ch[c].air_us / 1000u)) &&
              fieldf(&u, false, "%u", (unsigned)ch[c].listened_ms) && fieldf(&u, true, "%u", busy > 100 ? 100u : busy))) { u = keep; csv[u] = 0; break; }
    }
    return u;
}
static void save_begin(void)
{
    if (save.stage) return;
    lap_time_t t;
    lap_time(&t);
    snprintf(save.base, sizeof(save.base), "/sdcard/wifi_survey/survey-%04d%02d%02d-%02d%02d%02d", t.year, t.month, t.day, t.hour, t.minute, t.second);
    save.job = lap_sd_mkdir("/sdcard/wifi_survey");
    save.stage = 1;
    say(false, "saving...");
}
static void save_step(void)
{
    if (!save.stage) return;
    if (save.job >= 0) {
        const int r = lap_sd_poll(save.job);
        if (r == LAP_SD_PENDING) return;
        if (r < 0 && save.stage > 1) { say(true, "save failed (%d): is there a card?", r); save.stage = 0; return; }
    }
    char path[80];
    size_t n = 0;
    if (save.stage == 1) { n = csv_aps(); snprintf(path, sizeof(path), "%s-aps.csv", save.base); }
    else if (save.stage == 2) { n = csv_channels(); snprintf(path, sizeof(path), "%s-channels.csv", save.base); }
    else { say(false, "saved %s-aps.csv and -channels.csv", save.base + 20); save.stage = 0; return; }
    save.job = lap_sd_write(path, 0, csv, (uint32_t)n);
    save.stage++;
    if (save.job < 0) { say(true, "save refused (%d)", save.job); save.stage = 0; }
}

/* ---- RECORD: the survey recorder ----------------------------------------------------------
   LINK's SURVEY tab until 2026-11-15, moved here with its files: one WiGLE
   CSV a walk in /sdcard/lakeshark/survey (YYYYMMDD_HHMMSS.csv, _NNN when
   taken) and a _summary.csv beside it with every device's centroid; KEEP
   SESSIONS removes the oldest. The rules are survey_rec.h's. What differs
   from LINK, because an app sees the radios through the SDK:
   - Wi-Fi rows come from beacons (frame capture), which carry the BSSID and
     the security suite; a scan result alone has neither, so an AP heard only
     by scan is not written. Scans add sightings of APs whose beacons were
     heard. The board's Wi-Fi link can stay up.
   - Times come from GPS, else from the board's clock once it is set (network
     or RTC); LINK waited for GPS time.
   - The card is written through lap_sd jobs, one at a time, every 5 s.
   - It runs while the app is open and, as a service, behind HOME. */
#define REC_DIR "/sdcard/lakeshark/survey"
#define REC_BUF 32768
#define REC_SBUF 16384
#define REC_LIST 256
#define REC_FLUSH_MS 5000u
#define REC_VERSION "1.3.1"
enum { R_OFF, R_STARTING, R_ON, R_STOPPING, R_RETAIN };
enum { J_NONE, J_MKDIR1, J_MKDIR2, J_LIST, J_HEAD, J_SHEAD, J_ROWS, J_SUM, J_RLIST, J_REMOVE };
typedef struct { uint8_t magic, wifi, ble, only_fix, scan_s, keep, sort, pad; } rec_opt_t;
static const uint8_t SCAN_STEPS[] = {3, 6, 10, 15, 30, 60, 120};
static const uint8_t KEEP_STEPS[] = {1, 3, 5, 10, 20, 50};
static const char *const SORTS[3] = {"STRONGEST", "NEWEST", "NAME"};
static rec_opt_t ropt = {0x5A, 1, 1, 1, 6, 10, 0, 0};
static struct {
    int phase, job, job_kind, nlist, ble_h, gps_lease, set_job;
    sr_session_t s;
    sr_entry_t *table;
    uint16_t *heads, *order;
    char *buf, *sbuf;
    lap_sd_entry_t *list;
    size_t used, sused;
    bool made1, made2, listed, named, shead, io_error, had_file, retain_failed, svc, gps_hw, ble_wait, opt_dirty;
    unsigned rows, lost, sum_k, order_n;
    uint32_t start_ms, end_ms, flush_ms, gps_ms, utc_base, utc_at, order_ms, ble_retry_ms, activity, order_seen;
    uint8_t gps_state, utc_src;     /* utc_src: 0 none, 1 GPS, 2 the board's clock */
    sr_fix_t fix;
    char path[80], spath[96], rm_sum[112], status[96];
} rec = {.job = -1, .set_job = -1};
static int rpick = -1;              /* table index of the device picked on RECORD */
static bool rdetail;
static int rchoose_h;

static bool rec_live(void) { return rec.phase == R_STARTING || rec.phase == R_ON; }
static bool rec_wants_wifi(void) { return rec_live() && ropt.wifi; }
static uint32_t scan_every_ms(void) { return (ropt.scan_s >= 3 && ropt.scan_s <= 120 ? ropt.scan_s : 6) * 1000u; }
static uint32_t rec_utc(void) { return rec.utc_src ? rec.utc_base + (lap_millis() - rec.utc_at) / 1000u : 0; }

static void rec_opts_load(void)
{
    rec_opt_t o;
    if (lap_settings_get("record", &o, sizeof(o)) == (int)sizeof(o) && o.magic == 0x5A && o.scan_s >= 3 && o.scan_s <= 120 &&
        o.keep >= 1 && o.keep <= 50 && o.sort < 3)
        ropt = o;
}
static void rec_opts_save(void) { rec.opt_dirty = true; }

/* The receiver and the clock, four times a second. */
static void rec_gps(void)
{
    const uint32_t now = lap_millis();
    if (rec.gps_ms && now - rec.gps_ms < 250u) return;
    rec.gps_ms = now ? now : 1;
    bool gps_utc = false;
    lap_gps_status_t g = {.size = sizeof(g), .version = 1};
    if (rec.gps_hw && !lap_gps_status(&g)) {
        rec.gps_state = g.state;
        rec.fix.fix = g.state == LAP_GPS_FIX && g.position.fix && g.fix_age_ms != UINT32_MAX;
        if (rec.fix.fix) {
            rec.fix.lat = g.position.lat; rec.fix.lon = g.position.lon;
            rec.fix.alt_m = g.position.alt_m; rec.fix.hdop = g.position.hdop;
            rec.fix.fix_ms = (int64_t)now - (int64_t)g.fix_age_ms;
        }
        if (g.position.utc > (int64_t)SR_UTC_MIN && g.sentence_age_ms < 3000u) {
            rec.utc_base = (uint32_t)g.position.utc; rec.utc_at = now - g.sentence_age_ms; rec.utc_src = 1;
            gps_utc = true;
        }
    } else {
        rec.gps_state = LAP_GPS_ABSENT;
        rec.fix.fix = false;
    }
    if (!gps_utc) {
        lap_time_t t;
        lap_time(&t);
        if (t.synced && t.unix > (int64_t)SR_UTC_MIN) { rec.utc_base = (uint32_t)t.unix; rec.utc_at = now; rec.utc_src = 2; }
        else rec.utc_src = 0;
    }
}

/* One sighting in: the table, and a row when the rules want one. */
static void rec_obs(const sr_obs_t *o)
{
    if (!rec_live() || (o->kind == SR_WIFI ? !ropt.wifi : !ropt.ble)) return;
    const uint32_t now = lap_millis(), utc = rec_utc();
    if (sr_note(&rec.s, o, &rec.fix, utc, now) == SR_IGNORED) return;
    sr_entry_t *e = &rec.table[rec.s.current];
    rec.activity++;
    if (!utc || rec.io_error || !sr_relog(e, o, &rec.fix, now)) return;
    sr_fix_t f = rec.fix;
    f.fix = sr_fix_fresh(&f, now);
    char line[256];
    const int n = sr_row(line, sizeof(line), o, &f, e->first_s);
    if (n <= 0) return;
    if (rec.used + (size_t)n > REC_BUF) { rec.lost++; return; }
    memcpy(rec.buf + rec.used, line, (size_t)n);
    rec.used += (size_t)n;
    sr_logged(e, o, &f, now);
    rec.rows++;
}
static void rec_wifi(const uint8_t bssid[6], const char *ssid, uint8_t channel, uint8_t sec, int rssi)
{
    if (!rec_wants_wifi()) return;
    sr_obs_t o = {.kind = SR_WIFI, .channel = channel, .sec = sec, .rssi = rssi};
    memcpy(o.addr, bssid, 6);
    snprintf(o.name, sizeof(o.name), "%.32s", ssid);
    rec_obs(&o);
}
static void rec_evicted(void *ctx, const sr_entry_t *e)
{
    (void)ctx;
    if (rpick >= 0 && &rec.table[rpick] == e) { rpick = -1; rdetail = false; }
    const int n = sr_summary(rec.sbuf + rec.sused, REC_SBUF - rec.sused, e);
    if (n > 0) rec.sused += (size_t)n;
    else rec.lost++;
}
/* A Bluetooth name from advertising data: complete, else shortened. */
static void ad_name(const uint8_t *d, size_t n, char out[33])
{
    out[0] = 0;
    for (size_t i = 0; i + 1 < n;) {
        const size_t len = d[i];
        if (!len || i + 1 + len > n) break;
        const uint8_t t = d[i + 1];
        if (t == 0x09 || (t == 0x08 && !out[0])) {
            const size_t k = len - 1 > 32 ? 32 : len - 1;
            memcpy(out, d + i + 2, k);
            out[k] = 0;
            if (t == 0x09) break;
        }
        i += 1 + len;
    }
}
static void rec_ble(void)
{
    if (!rec_live() || !ropt.ble) return;
    if (rec.ble_h <= 0) {
        if (rec.ble_wait && lap_millis() - rec.ble_retry_ms >= 1000u) {
            rec.ble_retry_ms = lap_millis();
            const int sh = lap_ble_scan_start();
            if (sh > 0) { rec.ble_h = sh; rec.ble_wait = false; }
        }
        return;
    }
    static uint8_t ad[LAP_BLE_AD_MAX];
    lap_ble_advert_t a;
    for (int i = 0, n; i < 24 && (n = lap_ble_advert_next(rec.ble_h, &a, ad, sizeof(ad))) >= 0; i++) {
        sr_obs_t o = {.kind = SR_BLE, .addr_type = a.addr_type, .rssi = a.rssi};
        for (int k = 0; k < 6; k++) o.addr[k] = a.addr[5 - k];      /* the SDK gives it least significant octet first */
        ad_name(ad, (size_t)n, o.name);
        rec_obs(&o);
    }
}

static void rec_release(void)
{
    if (rec.ble_h > 0) lap_ble_scan_stop(rec.ble_h);
    if (rec.gps_lease > 0) lap_gps_release(rec.gps_lease);
    rec.ble_h = rec.gps_lease = 0;
    rec.ble_wait = false;
}
static void svc_tick(void);
static void svc_stopped(void);
static void rec_start(void)
{
    if (rec.phase != R_OFF) return;
    lap_caps_t c;
    memset(&c, 0, sizeof(c));
    lap_caps(&c, sizeof(c));
    rec.gps_hw = (c.hardware & LAP_HW_GPS) != 0;
    const char *why = NULL;
    if (!ropt.wifi && !ropt.ble) why = "turn WI-FI ROWS or BLE ROWS on first";
    else if (!(c.hardware & LAP_HW_SD)) why = "no SD card to record to";
    else if (ropt.only_fix && !rec.gps_hw) why = "no GPS here: turn ONLY GPS FIX off";
    else if (ropt.ble && !(c.hardware & LAP_HW_BLE)) why = "no Bluetooth here: turn BLE ROWS off";
    if (!why && !rec.table) {
        rec.table = calloc(SR_CAP, sizeof(*rec.table));
        rec.heads = calloc(SR_BUCKETS, sizeof(*rec.heads));
        rec.order = calloc(SR_CAP, sizeof(*rec.order));
        rec.buf = malloc(REC_BUF);
        rec.sbuf = malloc(REC_SBUF);
        rec.list = calloc(REC_LIST, sizeof(*rec.list));
        if (!rec.table || !rec.heads || !rec.order || !rec.buf || !rec.sbuf || !rec.list) why = "not enough memory";
    }
    if (!why && rec.gps_hw) {
        rec.gps_lease = lap_gps_acquire(0);
        if (rec.gps_lease <= 0) { rec.gps_lease = 0; if (ropt.only_fix) why = "GPS would not start (ONLY GPS FIX is on)"; }
    }
    if (!why && ropt.ble) {
        const int sh = lap_ble_scan_start();
        if (sh > 0) rec.ble_h = sh;
        else if (sh == LAP_IO_EBUSY || sh == LAP_IO_EAGAIN) { rec.ble_wait = true; rec.ble_retry_ms = lap_millis(); }
        else why = "the Bluetooth scan would not start";
    }
    if (why) {
        rec_release();
        snprintf(rec.status, sizeof(rec.status), "Cannot record: %s", why);
        say(true, "cannot record: %s", why);
        return;
    }
    sr_init(&rec.s, rec.table, rec.heads, SR_CAP, ropt.only_fix != 0);
    rec.s.evicted = rec_evicted;
    rec.used = rec.sused = 0;
    rec.rows = rec.lost = rec.sum_k = rec.order_n = 0;
    rec.made1 = rec.made2 = rec.listed = rec.named = rec.shead = rec.io_error = rec.had_file = rec.retain_failed = false;
    rec.path[0] = rec.spath[0] = rec.rm_sum[0] = 0;
    rec.nlist = 0;
    rec.start_ms = rec.flush_ms = lap_millis();
    rec.gps_ms = 0;
    rpick = -1; rdetail = false;
    rec.phase = R_STARTING;
    rec_gps();
    const lap_service_t svc = {.tick = svc_tick, .stopped = svc_stopped, .period_ms = 100};
    rec.svc = lap_service_start(&svc) == LAP_SVC_OK;
    if (!rec.svc) lap_keep_awake(true);
    rec.status[0] = 0;
    say(false, "recording to %s", REC_DIR);
}
static void rec_stop(void)
{
    if (!rec_live()) return;
    rec_release();
    rec.end_ms = lap_millis();
    rec.sum_k = 0;
    rec.phase = R_STOPPING;
    snprintf(rec.status, sizeof(rec.status), "Saving the session");
}
static void rec_finish(void)
{
    snprintf(rec.status, sizeof(rec.status), "%s", rec.io_error ? "SD export failed / existing rows retained" :
             !rec.had_file ? "No UTC observations / no file written" :
             rec.retain_failed ? "Session saved / retention cleanup failed" : "Session saved / centroid segments include evictions");
    rec.phase = R_OFF;
    if (rec.svc) { rec.svc = false; lap_service_stop(); }
    else lap_keep_awake(false);
}
/* The app is going away mid-session (closed without the service, or the
   service stopped from SETTINGS): what is held goes to the card as one last
   append each. Jobs not started when the app unloads are dropped. */
static void rec_abort(void)
{
    if (rec.phase == R_OFF) return;
    if (rec.named) {
        if (rec.used) lap_sd_write(rec.path, LAP_SD_APPEND, rec.buf, (uint32_t)rec.used);
        size_t n = rec.sused;
        for (unsigned k = 0; k < rec.s.count; k++) {
            const int m = sr_summary(rec.sbuf + n, REC_SBUF - n, &rec.table[k]);
            if (m <= 0) break;
            n += (size_t)m;
        }
        if (n) lap_sd_write(rec.spath, LAP_SD_APPEND, rec.sbuf, (uint32_t)n);
    }
    rec.used = rec.sused = 0;
    rec_release();
    rec.phase = R_OFF;
}
static void rec_free(void)
{
    free(rec.table); free(rec.heads); free(rec.order); free(rec.buf); free(rec.sbuf); free(rec.list);
    rec.table = NULL; rec.heads = rec.order = NULL; rec.buf = rec.sbuf = NULL; rec.list = NULL;
}
static bool submit(int kind, int job)
{
    if (job < 0) return false;
    rec.job = job;
    rec.job_kind = kind;
    return true;
}
/* Retention: undated files first, then by name, which is by time. */
static bool older(const char *a, const char *b)
{
    const bool ua = *a == 'u', ub = *b == 'u';
    return ua != ub ? ua : strcmp(a, b) < 0;
}
/* The card, one job at a time: the folder, the names already there, the
   header, rows every 5 s, the summary at the end, then KEEP SESSIONS. */
static void rec_io(void)
{
    if (rec.set_job >= 0 && lap_sd_poll(rec.set_job) != LAP_SD_PENDING) rec.set_job = -1;
    if (rec.opt_dirty && rec.set_job < 0) {
        const int j = lap_settings_set("record", &ropt, sizeof(ropt));
        if (j >= 0) rec.set_job = j;
        if (j >= 0 || j != LAP_SD_EBUSY) rec.opt_dirty = false;     /* busy: the next frame tries again */
    }
    if (rec.phase == R_OFF) return;
    if (rec.job >= 0) {
        const int r = lap_sd_poll(rec.job);
        if (r == LAP_SD_PENDING) return;
        const int kind = rec.job_kind;
        rec.job = -1; rec.job_kind = J_NONE;
        if (kind == J_MKDIR1) rec.made1 = true;
        else if (kind == J_MKDIR2) { rec.made2 = true; if (r < 0) rec.io_error = true; }
        else if (kind == J_LIST || kind == J_RLIST) { rec.listed = true; rec.nlist = r > 0 ? r : 0; if (kind == J_RLIST && r < 0 && r != LAP_SD_ENOENT) rec.retain_failed = true; }
        else if (kind == J_HEAD) { if (r < 0) rec.io_error = true; else rec.named = rec.had_file = true; }
        else if (kind == J_SHEAD) { if (r < 0) rec.io_error = true; else rec.shead = true; }
        else if (kind == J_ROWS || kind == J_SUM) { if (r < 0) rec.io_error = true; }
        else if (kind == J_REMOVE) { if (r < 0 && r != LAP_SD_ENOENT) rec.retain_failed = true; }
    }
    const uint32_t now = lap_millis();
    if (rec.phase == R_STARTING) {
        if (!rec.made1) { submit(J_MKDIR1, lap_sd_mkdir("/sdcard/lakeshark")); return; }
        if (!rec.made2) { submit(J_MKDIR2, lap_sd_mkdir(REC_DIR)); return; }
        if (!rec.listed) { submit(J_LIST, lap_sd_list(REC_DIR, rec.list, REC_LIST)); return; }
        rec.phase = R_ON;
    }
    if (rec.phase == R_ON || rec.phase == R_STOPPING) {
        const bool stopping = rec.phase == R_STOPPING;
        if (rec.io_error) { if (stopping) rec_finish(); return; }
        const uint32_t utc = rec_utc();
        if (!rec.named && !stopping && utc) {
            char base[32], name[48], head[480];
            if (!sr_filename(base, sizeof(base), utc)) return;
            snprintf(name, sizeof(name), "%s", base);
            for (int i = 1; i < 1000; i++) {
                bool taken = false;
                for (int k = 0; k < rec.nlist && !taken; k++) taken = !strcmp(rec.list[k].name, name);
                if (!taken) break;
                snprintf(name, sizeof(name), "%.15s_%03d.csv", base, i);
            }
            snprintf(rec.path, sizeof(rec.path), REC_DIR "/%.40s", name);
            snprintf(rec.spath, sizeof(rec.spath), "%.*s_summary.csv", (int)strlen(rec.path) - 4, rec.path);
            const uint32_t v = lap_api_version();
            char board[24];
            snprintf(board, sizeof(board), "sdk-api-%u.%u", (unsigned)(v >> 16), (unsigned)(v & 0xFFFFu));
            const int n = sr_header(head, sizeof(head), "wifi_survey-" REC_VERSION, board);
            if (n > 0) submit(J_HEAD, lap_sd_write(rec.path, 0, head, (uint32_t)n));
            return;
        }
        if (rec.named && !rec.shead) {
            char head[96];
            const int n = sr_summary_header(head, sizeof(head));
            if (n > 0) submit(J_SHEAD, lap_sd_write(rec.spath, 0, head, (uint32_t)n));
            return;
        }
        if (!rec.named) {
            if (stopping) { rec.phase = R_RETAIN; rec.listed = false; }
            return;
        }
        const bool due = stopping || now - rec.flush_ms >= REC_FLUSH_MS;
        if (rec.used && (due || rec.used > REC_BUF / 2)) {
            if (submit(J_ROWS, lap_sd_write(rec.path, LAP_SD_APPEND, rec.buf, (uint32_t)rec.used))) rec.used = 0;
            return;
        }
        if (rec.sused && (due || rec.sused > REC_SBUF / 2)) {
            if (submit(J_SUM, lap_sd_write(rec.spath, LAP_SD_APPEND, rec.sbuf, (uint32_t)rec.sused))) rec.sused = 0;
            return;
        }
        if (!stopping) { if (due) rec.flush_ms = now; return; }
        if (rec.sum_k < rec.s.count) {      /* every device still in the table, a buffer at a time */
            while (rec.sum_k < rec.s.count) {
                const int m = sr_summary(rec.sbuf + rec.sused, REC_SBUF - rec.sused, &rec.table[rec.sum_k]);
                if (m <= 0) break;
                rec.sused += (size_t)m;
                rec.sum_k++;
            }
            return;
        }
        rec.phase = R_RETAIN;
        rec.listed = false;
    }
    if (rec.phase == R_RETAIN) {
        if (!rec.had_file) { rec_finish(); return; }
        if (!rec.listed) { submit(J_RLIST, lap_sd_list(REC_DIR, rec.list, REC_LIST)); return; }
        if (rec.rm_sum[0]) { if (submit(J_REMOVE, lap_sd_remove(rec.rm_sum))) rec.rm_sum[0] = 0; return; }
        const char *mine = strrchr(rec.path, '/');
        mine = mine ? mine + 1 : rec.path;
        int count = 0, oldest = -1;
        for (int i = 0; i < rec.nlist; i++) {
            if (rec.list[i].dir || !sr_session_name(rec.list[i].name)) continue;
            count++;
            if (!strcmp(rec.list[i].name, mine)) continue;
            if (oldest < 0 || older(rec.list[i].name, rec.list[oldest].name)) oldest = i;
        }
        if (count <= ropt.keep || oldest < 0) { rec_finish(); return; }
        char victim[96];
        snprintf(victim, sizeof(victim), REC_DIR "/%.63s", rec.list[oldest].name);
        if (submit(J_REMOVE, lap_sd_remove(victim))) {
            snprintf(rec.rm_sum, sizeof(rec.rm_sum), "%.*s_summary.csv", (int)strlen(victim) - 4, victim);
            rec.list[oldest].name[0] = 0;
        }
    }
}
static void rec_step(void)
{
    if (rec_live()) { rec_gps(); rec_ble(); }
    rec_io();
}
/* Behind HOME: the radio, the recorder and the card, within the tick's budget. */
static void svc_tick(void)
{
    if (!ap) return;
    poll(2u);
    rec_step();
}
static void svc_stopped(void) { rec_abort(); }

/* ---- drawing -------------------------------------------------------------------------------- */
/* The screen, top down: the title; a header with the radar and the key facts
   (hh rows, dropped when the screen is short); the tabs (the < name > keys on
   the handheld, a row of names on a desk); a framed panel the tab scrolls in;
   the board's keys. Panel contents are laid out from X0, IW columns wide. */
static int W, H, tab, scroll, smax, sel, clip0, clip1, kr, tr, hh, ty, X0, IW;
static bool desk, dragging, show_sel;
static lap_scroll_t drag;
enum { T_APS, T_CHANNELS, T_TRAFFIC, T_HISTORY, T_RECORD, TABS };
static const char *const tabs[TABS] = {"ACCESS POINTS", "CHANNELS", "TRAFFIC", "HISTORY", "RECORD"};
#define FPSN 128
static float fps[FPSN];         /* frames heard per second, oldest first */
static int fps_n;
static uint32_t fps_at, fps_frames;

#define FAINT LAP_UI_FAINT
#define DIM LAP_UI_DIM
#define TEXT LAP_UI_TEXT
static uint8_t fg(uint8_t hue) { return LAP_ATTR(hue, LAP_BLACK); }
static bool vis(int y) { return y >= clip0 && y < clip1; }

static void put(int x, int y, int wd, uint8_t attr, const char *fmt, ...)
{
    char t[200];
    va_list a;
    if (wd <= 0 || y < 0 || y >= H || x >= W || x < 0) return;
    va_start(a, fmt);
    vsnprintf(t, sizeof(t), fmt, a);
    va_end(a);
    if (x + wd > W) wd = W - x;
    if ((size_t)wd < sizeof(t)) t[wd] = 0;
    lap_put_str(x, y, t, attr);
}
static void wrap(int x, int *y, int wd, uint8_t attr, const char *t)
{
    if (wd < 8) wd = 8;
    while (*t) {
        while (*t == ' ') t++;
        if (!*t) break;
        int n = (int)strlen(t);
        if (n > wd) { n = wd; while (n > 0 && t[n] != ' ') n--; if (!n) n = wd; }
        if (vis(*y)) put(X0 + x, *y, wd, attr, "%.*s", n, t);
        (*y)++;
        t += n;
    }
}
static void row(int *y, int x, uint8_t attr, const char *fmt, ...)
{
    char t[200];
    va_list a;
    va_start(a, fmt);
    vsnprintf(t, sizeof(t), fmt, a);
    va_end(a);
    if (vis(*y)) put(X0 + x, *y, IW - x, attr, "%s", t);
    (*y)++;
}
static void centre(int *y, uint8_t attr, const char *t)
{
    const int n = (int)strlen(t);
    if (vis(*y)) put(X0 + (IW - n > 0 ? (IW - n) / 2 : 0), *y, IW, attr, "%s", t);
    (*y)++;
}
static uint8_t signal_colour(int rssi)
{
    return rssi >= -55 ? LAP_GREEN | LAP_BRIGHT : rssi >= -67 ? LAP_GREEN : rssi >= -75 ? LAP_YELLOW : rssi >= -85 ? LAP_RED | LAP_BRIGHT : LAP_RED;
}
static float frac_of(int rssi)
{
    const float f = (float)(rssi + 100) / 70.0f;
    return f < 0 ? 0 : f > 1 ? 1 : f;
}
/* Each AP keeps a colour of its own on the CHANNELS arcs. */
static uint8_t ap_hue(const ap_t *a)
{
    static const uint8_t pal[6] = {LAP_CYAN, LAP_GREEN, LAP_YELLOW, LAP_MAGENTA, LAP_BLUE, LAP_RED};
    return pal[(a - ap) % 6];
}
/* Heard within the last 450 ms (at most every 1.5 s): its signal pulses. */
static int pulse(const ap_t *a)
{
    const uint32_t age = lap_millis() - a->flash_ms;
    return a->flash_ms && age < 450u ? 1 + (int)(age / 150u) : 0;
}
/* A signal bar: -100 dBm empty, -30 full; it rises in when the AP is first
   heard, and the waves beside it go out each time it is heard again. */
static void meter(int x, int y, int wd, const ap_t *a, bool on)
{
    if (!vis(y) || wd < 3) return;
    float f = frac_of(a->rssi);
    const uint32_t age = lap_millis() - a->first_ms;
    if (age < 800u) f = f * (float)age / 800.0f;
    lap_ui_meter(X0 + x, y, wd, f, on ? LAP_WHITE | LAP_BRIGHT : signal_colour(a->rssi));
}
static void waves(int x, int y, const ap_t *a, int n)
{
    if (!vis(y)) return;
    const int p = pulse(a);
    for (int i = 0; i < n; i++) put(X0 + x + i, y, 1, p > i ? fg(LAP_UI_LIVE) : FAINT, "%c", p > i ? ')' : i ? ' ' : '.');
}
static const char *ago(char *b, size_t n, uint32_t ms)
{
    const uint32_t s = (lap_millis() - ms) / 1000u;
    if (s < 60) snprintf(b, n, "%us", (unsigned)s);
    else if (s < 3600) snprintf(b, n, "%um", (unsigned)(s / 60));
    else snprintf(b, n, "%uh", (unsigned)(s / 3600));
    return b;
}

/* ---- the header: a radar sweeping the air, and what it has found ---------------------------- */
static const int8_t dir16[16][2] = {{0, -100}, {38, -92}, {71, -71}, {92, -38}, {100, 0}, {92, 38}, {71, 71}, {38, 92},
                                    {0, 100}, {-38, 92}, {-71, 71}, {-92, 38}, {-100, 0}, {-92, -38}, {-71, -71}, {-38, -92}};
static int ap_angle(const ap_t *a)
{
    unsigned hsh = a->channel * 3u;
    if (a->has_bssid) for (int i = 0; i < 6; i++) hsh = hsh * 31u + a->bssid[i];
    else for (const char *c = a->ssid; *c; c++) hsh = hsh * 31u + (uint8_t)*c;
    return (int)(hsh & 15u);
}
static void radar(int x, int y)
{
    static const char *const face[5] = {"  .-=======-.  ", " /     :     \\ ", "|  -  -+-  -  |", " \\     :     / ", "  '-=======-'  "};
    const int cx = x + 7, cy = y + 2, rx = 5, ry = 1;   /* inside the rim */
    for (int r = 0; r < 5; r++) put(x, y + r, 15, fg(LAP_UI_FRAME | LAP_BRIGHT), "%s", face[r]);
    for (int r = 1; r < 4; r++)               /* the crosshair in the faint colour */
        for (int c = 1; c < 14; c++)
            if (face[r][c] != ' ') lap_put_cell(x + c, y + r, face[r][c], FAINT);
    const int k = paused ? 2 : (int)((lap_millis() / 120u) & 15u);
    /* the beam and its afterglow */
    for (int g = 2; g >= 0; g--) {
        const int d = (k - g) & 15;
        const char ch = d == 0 || d == 8 ? '|' : d == 4 || d == 12 ? '-' : (d > 0 && d < 4) || (d > 8 && d < 12) ? '/' : '\\';
        for (int t = 1; t <= 3; t++) {
            const int px = cx + (dir16[d][0] * rx * t + (dir16[d][0] >= 0 ? 150 : -150)) / 300;
            const int py = cy + (dir16[d][1] * ry * t + (dir16[d][1] >= 0 ? 150 : -150)) / 300;
            if (px == cx && py == cy) continue;
            lap_put_cell(px, py, ch, g == 0 ? fg(paused ? LAP_UI_WARN : LAP_UI_LIVE) : g == 1 ? fg(LAP_GREEN) : FAINT);
        }
    }
    lap_put_cell(cx, cy, 'o', fg(paused ? LAP_UI_WARN : LAP_UI_LIVE));
    /* the access points as blips: nearer the middle the stronger; lit as the beam passes */
    for (int n = 0; n < nsorted && n < 12; n++) {
        const ap_t *a = &ap[order[n]];
        if (lap_millis() - a->last_ms >= 30000u) continue;
        const int d = ap_angle(a);
        int rr = (-30 - a->rssi) * 100 / 70;
        if (rr < 35) rr = 35;
        if (rr > 100) rr = 100;
        const int px = cx + dir16[d][0] * rx * rr / 10000, py = cy + dir16[d][1] * ry * rr / 10000;
        if ((px == cx && py == cy) || py < y || py > y + 4) continue;
        const bool lit = ((k - d) & 15) <= 2 && !paused;
        lap_put_cell(px, py, lit ? '*' : '.', lit ? fg(ap_hue(a) | LAP_BRIGHT) : fg(ap_hue(a)));
    }
}
static void header(int y)
{
    radar(1, y);
    const int x = 18, w = W - x - 1;
    int now = 0, ch24[13] = {0}, top = 1;
    const ap_t *best = NULL;
    for (int i = 0; i < naps; i++) {
        const ap_t *a = &ap[i];
        if (lap_millis() - a->last_ms < 30000u) now++;
        if (a->channel >= 1 && a->channel <= 13) { ch24[a->channel - 1]++; if (ch24[a->channel - 1] > top) top = ch24[a->channel - 1]; }
        if (!best || a->rssi > best->rssi) best = a;
    }
    char t[40];
    const int n = snprintf(t, sizeof(t), "%d access point%s", naps, naps == 1 ? "" : "s");
    put(x, y, w, TEXT, "%s", t);
    put(x + n, y, w - n, DIM, "  %d heard now", now);
    if (best) put(x, y + 1, w, fg(signal_colour(best->rssi)), "%.*s  %d dBm  ch %u", w > 22 ? w - 18 : 4,
                  best->ssid[0] ? best->ssid : "<hidden>", best->rssi, best->channel);
    else {
        static const char *const dots[4] = {"", ".", "..", "..."};
        put(x, y + 1, w, fg(LAP_UI_FRAME), "listening%s", dots[(lap_millis() / 400u) & 3u]);
    }
    char mode[12];
    if (paused) snprintf(mode, sizeof(mode), "PAUSED");
    else if (h <= 0) snprintf(mode, sizeof(mode), "SCAN ONLY");
    else snprintf(mode, sizeof(mode), "%s ch %u", fell_back || plan == PLAN_STAY ? "LINK" : "HOP", chan);
    const lap_chip_t left[2] = {{mode, true, paused || h <= 0 ? LAP_UI_WARN : LAP_UI_LIVE},
                                {"FRAMES", h > 0 && !paused, LAP_UI_LIVE}};
    const lap_chip_t right[1] = {{"SAVE", save.stage != 0, LAP_UI_VALUE}};
    lap_ui_rail(x, y + 2, w, left, 2, right, 1);
    put(x, y + 3, w, DIM, "%u scans  %u frames%s", (unsigned)scans, (unsigned)frames, lost ? "  LOST" : "");
    if (lost) put(x + w - 4, y + 3, 4, fg(LAP_UI_ALARM), "LOST");
    /* how crowded 1..13 is, two columns a channel, the channel being heard marked */
    int two[26];
    for (int i = 0; i < 26; i++) two[i] = ch24[i / 2];
    put(x, y + 4, 2, DIM, "1");
    lap_ui_sparkline(x + 2, y + 4, 26, 1, two, 26, top, h > 0 && chan >= 1 && chan <= 13 ? 2 * (chan - 1) : -1);
    put(x + 29, y + 4, w - 29, DIM, "13");
}

/* What the capture half can and cannot do now. */
static void capture_note(int *y)
{
    const uint8_t warn = fg(LAP_UI_WARN), tip = fg(LAP_UI_FRAME);
    if (h > 0) {
        if (fell_back) wrap(0, y, IW, warn, "The board is on a Wi-Fi network: listening on its channel only. MORE > ALLOW LINK DROP to hop.");
        return;
    }
    wrap(0, y, IW, warn, "No frame capture: the list comes from scans, without BSSIDs or traffic.");
    wrap(0, y, IW, DIM, why[0] ? why : "Wi-Fi capture did not start");
    if (reason == LAP_CAPTURE_NO_TRANSPORT) {
        wrap(0, y, IW, tip, "Wi-Fi capture needs the C6 monitor firmware. The board installs it by itself a minute after it boots on Wi-Fi, or now from:");
        row(y, 2, TEXT, "SYSTEM > UPDATE > C6 RADIO FW > UPDATE");
    } else if (reason == LAP_CAPTURE_LINK_IN_USE) wrap(0, y, IW, tip, "The board is on a Wi-Fi network and hopping would drop it. MORE > ALLOW LINK DROP.");
    else if (reason == LAP_CAPTURE_BUSY) wrap(0, y, IW, tip, "Another app holds the Wi-Fi capture. Close it, then MORE > RESTART CAPTURE.");
}
/* Nothing heard yet: a mast sending out rings. */
static void listening_art(int *y, const char *say1, const char *say2)
{
    const int p = (int)((lap_millis() / 250u) & 3u);
    char l[24], r[24];
    snprintf(l, sizeof(l), "%*s%.*s", 3 - p, "", p, "(((");
    snprintf(r, sizeof(r), "%.*s%*s", p, ")))", 3 - p, "");
    char line[64];
    (*y)++;
    snprintf(line, sizeof(line), "%s  \\ | /  %s", l, r);
    centre(y, fg(LAP_UI_FRAME), line);
    snprintf(line, sizeof(line), "%s -- o -- %s", l, r);
    centre(y, fg(LAP_UI_LIVE), line);
    centre(y, fg(LAP_UI_FRAME), "      / | \\      ");
    centre(y, fg(LAP_UI_FRAME), "        |        ");
    centre(y, fg(LAP_UI_FRAME), "       /|\\       ");
    centre(y, fg(LAP_UI_FRAME), "      /_|_\\      ");
    (*y)++;
    centre(y, TEXT, say1);
    centre(y, DIM, say2);
}
static void aps_tab(int y)
{
    const int first = y;
    y -= scroll;
    if (h <= 0 || fell_back) { capture_note(&y); y++; }
    if (!naps) {
        listening_art(&y, "Scanning: nothing heard yet.", paused ? "Paused: MORE > RESUME." : "A scan every 6 s; beacons as they come.");
        smax = y + scroll - first - (clip1 - first);
        return;
    }
    if (desk) row(&y, 0, fg(LAP_UI_VALUE), "  %-32s %-17s %4s %4s %4s %9s %-8s %-12s %4s  %s", "SSID", "BSSID", "Ch", "Band", "dBm", "min/max", "Security", "Heard by", "Age", "Signal");
    int sel_y = -1;
    for (int k = 0; k < nsorted; k++) {
        const ap_t *a = &ap[order[k]];
        char b[18] = "(scan: no BSSID)", age[8], name[40];
        if (a->has_bssid) lap_dot11_mac(b, sizeof(b), a->bssid);
        snprintf(name, sizeof(name), "%s", a->ssid[0] ? a->ssid : "<hidden>");
        const bool on = k == sel, stale = lap_millis() - a->last_ms >= 30000u;
        const uint8_t at = on ? LAP_ATTR(LAP_BLACK, LAP_YELLOW) : stale ? FAINT : !a->secure ? fg(LAP_UI_WARN) : TEXT;
        const char *heard = a->by_scan && a->by_capture ? "scan+frames" : a->by_scan ? "scan" : "frames";
        if (on) sel_y = y;
        if (desk) {
            if (on && vis(y)) lap_fill(X0, y, IW, 1, ' ', at);
            if (vis(y)) put(X0, y, IW, at, "  %-32.32s %-17s %4u %4s %4d %4d/%-4d %-8s %-12s %4s", name, b, a->channel, a->channel > 14 ? "5" : "2.4",
                            a->rssi, a->rmin, a->rmax, security(a), heard, ago(age, sizeof(age), a->last_ms));
            if (!on) waves(0, y, a, 2);
            meter(106, y, IW - 106 < 30 ? IW - 106 : 30, a, on);
            y++;
        } else {
            if (on && vis(y)) lap_fill(X0, y, IW, 1, ' ', at);
            if (on && vis(y + 1)) lap_fill(X0, y + 1, IW, 1, ' ', at);
            if (vis(y)) put(X0, y, IW, at, "%-22.22s %-8s ch%-3u %4d", name, security(a), a->channel, a->rssi);
            y++;
            if (vis(y)) put(X0 + 1, y, 18, on ? at : FAINT, "%s", b);
            if (vis(y)) put(X0 + 19, y, 11, on ? at : FAINT, "%-6s %4s", a->by_capture ? "frames" : "scan", ago(age, sizeof(age), a->last_ms));
            if (!on) waves(31, y, a, 3);
            meter(35, y, IW - 35, a, on);
            y++;
        }
    }
    if (over) row(&y, 0, fg(LAP_UI_WARN), "%u more were heard; the oldest made room", (unsigned)over);
    smax = y + scroll - first - (clip1 - first);
    /* a selection moved by a key or a tap comes into view; a dragged list stays where the finger left it */
    if (show_sel && !dragging && sel_y >= 0 && (sel_y < clip0 || sel_y + (desk ? 1 : 2) > clip1))
        scroll += sel_y < clip0 ? sel_y - clip0 : sel_y + (desk ? 1 : 2) - clip1;
    if (!dragging) show_sel = false;
}
/* 2.4 GHz as the classic overlap plot: each AP a hump over its channel and
   two either side, as tall as it is strong:
        __NAME__
       /        \
       |        |       flat across +/-1 channel, falling to the feet at +/-2

   Every hump is drawn, faint; the strongest few carry their names on their
   own peaks, in a colour each, placed strongest first and never over one
   another; the AP picked on ACCESS POINTS is white with its name lit. ZOOM
   spreads the channels (x2..x4, centred where the view is or on the channel
   tapped) and a sideways drag on the plot pans; a tap on a channel lists
   its APs under the plot. */
#define ZOOMS 4
#define HIT_CH 1000             /* hit[row]: HIT_CH + channel, or HIT_AP + ap index */
#define HIT_AP 2000
#define HITN 256
static int zm, vl, pick;        /* zoom 1..ZOOMS; the view's left edge in tenths of a channel; the channel tapped, 0 none */
static int16_t hit[HITN];       /* what a tap on each screen row picks, as the panel was drawn */
/* Where the plot was drawn last frame, for the finger: screen rows r0..r1-1
   (plot, axis and channel numbers), columns x0..x0+w-1. */
static struct { bool live; int r0, r1, x0, w, slots, top; } plot;
/* A sideways drag on the plot: it owns its touch (x0, y0) from when it
   moves a cell sideways until a moment after it lifts, so the swipe it ends
   with and any tap are not taken for a tab change or a pick. */
static struct { bool held, inside, on; int16_t x0, y0; int base; uint32_t up_ms; } pan;
static int pv_x0, pv_w, pv_sp;  /* this frame's plot: first column, width, tenths of a channel across it */

static int fdiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
static int view_span(void) { return plot.slots * 10 / (zm > 0 ? zm : 1); }
static void view_clamp(void)
{
    const int full = plot.slots * 10, sp = view_span();
    if (zm <= 1) { vl = -15; return; }
    if (vl > -15 + full - sp) vl = -15 + full - sp;
    if (vl < -15) vl = -15;
}
/* A new zoom, centred on the channel tapped, else on the middle of the view. */
static void zoom_set(int z)
{
    if (z < 1) z = 1;
    if (z > ZOOMS) z = ZOOMS;
    const int centre = pick ? pick * 10 : vl + view_span() / 2;
    zm = z;
    vl = centre - view_span() / 2;
    view_clamp();
}
static void pan_by(int tenths) { vl += tenths; view_clamp(); }
static const char *zoom_name(void)
{
    static const char *const n[ZOOMS + 1] = {"ALL", "ALL", "x2", "x3", "x4"};
    return n[zm >= 1 && zm <= ZOOMS ? zm : 1];
}
/* The channel under screen column col of the plot, or 0. */
static int channel_at(int col)
{
    if (plot.w <= 0) return 0;
    const int c10 = vl + fdiv((col - plot.x0) * view_span() + view_span() / 2, plot.w);
    const int c = fdiv(c10 + 5, 10);
    return c >= 1 && c <= plot.top ? c : 0;
}
static bool pan_owns_touch(void)
{
    lap_touch_t t;
    return pan.on && lap_touch(&t) && t.x0 == pan.x0 && t.y0 == pan.y0 && (t.down || lap_millis() - pan.up_ms < 300u);
}
/* Once a draw: a finger that went down on the zoomed plot and moves more
   sideways than up or down drags the channels with it. True while it does. */
static bool pan_follow(void)
{
    lap_touch_t t;
    if (!lap_touch(&t)) return false;
    int cw = 0, chh = 0;
    lap_cell_px(&cw, &chh);
    if (cw <= 0) cw = 10;
    if (chh <= 0) chh = 16;
    if ((t.down && !pan.held) || t.x0 != pan.x0 || t.y0 != pan.y0) {      /* a new touch, maybe a whole one between frames */
        pan.x0 = t.x0; pan.y0 = t.y0;
        pan.on = false;
        pan.base = vl;
        pan.inside = tab == T_CHANNELS && plot.live && zm > 1 && plot.w > 0 && !drag.dragging &&
                     t.y0 >= plot.r0 * chh && t.y0 < plot.r1 * chh && t.x0 >= plot.x0 * cw && t.x0 < (plot.x0 + plot.w) * cw;
    }
    if (pan.held && !t.down) pan.up_ms = lap_millis();
    pan.held = t.down;
    if (!pan.inside || (drag.dragging && !pan.on)) return false;
    const int dx = t.x - t.x0, dy = t.y - t.y0, adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (!pan.on && adx >= cw && adx > ady) pan.on = true;
    if (!pan.on) return false;
    vl = pan.base - dx * view_span() / (plot.w * cw);      /* finger right: the lower channels come in */
    view_clamp();
    return t.down;
}

static int chx(int c10) { return pv_x0 + fdiv((c10 - vl) * pv_w, pv_sp); }     /* column of a channel in tenths */
static void pcell(int x, int y, int16_t c, uint8_t attr)
{
    if (vis(y) && x >= pv_x0 && x < pv_x0 + pv_w) lap_put_cell(x, y, c, attr);
}
static int hump_top(const ap_t *a, int base, int bh) { return base - 1 - (int)(frac_of(a->rssi) * (float)(bh - 3)); }
/* One AP's hump: its flat top, then each side sloping out to the feet
   (a '/' where it steps out, '|' where it falls straight, '_' along a
   shallow run), whatever the zoom. */
static void hump(const ap_t *a, int base, int bh, uint8_t at, char top_ch)
{
    const int c10 = a->channel * 10, rtop = hump_top(a, base, bh);
    const int xl = chx(c10 - 10), xr = chx(c10 + 10), fl = chx(c10 - 20), fr = chx(c10 + 20);
    for (int x = xl; x <= xr; x++) pcell(x, rtop, top_ch, at);
    const int rows = base - rtop;
    int dl = xl - 1 - fl, dr = fr - xr - 1;
    if (dl < 0) dl = 0;
    if (dr < 0) dr = 0;
    for (int r = rtop + 1, pa = xl, pb = xr; r <= base; r++) {
        const int k = r - rtop - 1;
        const int xa = xl - 1 - (rows > 1 ? (dl * k + (rows - 1) / 2) / (rows - 1) : dl);
        const int xb = xr + 1 + (rows > 1 ? (dr * k + (rows - 1) / 2) / (rows - 1) : dr);
        for (int x = xa + 1; x < pa; x++) pcell(x, r - 1, '_', at);
        for (int x = pb + 1; x < xb; x++) pcell(x, r - 1, '_', at);
        pcell(xa, r, xa < pa ? '/' : '|', at);
        pcell(xb, r, xb > pb ? '\\' : '|', at);
        pa = xa; pb = xb;
    }
}
static void channels_tab(int y)
{
    const int first = y;
    y -= scroll;
    int prim[15] = {0}, spread[15] = {0}, best[15];
    int top = 13;
    for (int c = 0; c < 15; c++) best[c] = NOSIG;
    for (int i = 0; i < naps; i++) {
        const int c = ap[i].channel;
        if (c < 1 || c > 14) continue;
        if (c == 14) top = 14;
        prim[c]++;
        if (ap[i].rssi > best[c]) best[c] = ap[i].rssi;
        for (int d = c - 2; d <= c + 2; d++) if (d >= 1 && d <= 14) spread[d]++;
    }
    /* the view: channel -1.5 .. top+2.5 across the plot at ALL, a part of it zoomed */
    const int slots = top + 4;
    const bool wide = IW >= 80;
    plot.slots = slots; plot.top = top;
    if (zm < 1 || zm > ZOOMS) zm = 1;
    view_clamp();
    pv_sp = view_span();
    pv_x0 = X0 + 5;
    pv_w = IW - 5;
    if (pv_w > slots * 8) pv_w = slots * 8;
    if (pv_w < 8) pv_w = 8;
    const int cpc = pv_w * zm / slots;              /* columns a channel */
    const ap_t *sa = nsorted && sel < nsorted ? &ap[order[sel]] : NULL;
    const int si = sa ? (int)(sa - ap) : -1;
    const uint8_t hl = LAP_ATTR(LAP_BLACK, LAP_YELLOW);
    /* what is in view; the AP picked on ACCESS POINTS */
    int lo = 0, hi = 0;
    for (int c = 1; c <= top; c++)
        if (chx(c * 10) >= pv_x0 && chx(c * 10) < pv_x0 + pv_w) { if (!lo) lo = c; hi = c; }
    if (vis(y)) {
        int x = X0;
        put(x, y, IW, fg(LAP_UI_VALUE), "2.4 GHz");
        x += 9;
        x = lap_ui_chip(x, y, zm > 1 ? "ZOOM" : "ALL", zm > 1, LAP_UI_FRAME);
        if (zm > 1) put(x, y, X0 + IW - x, TEXT, "%s ch %d-%d", zoom_name(), lo, hi);
        else put(x, y, X0 + IW - x, DIM, "ch 1-%d", top);
        if (zm > 1 && IW >= 44) put(X0 + IW - 14, y, 14, DIM, "< drag: pan >");
    }
    y++;
    char sl[64] = "";
    if (sa) snprintf(sl, sizeof(sl), " %s  ch %u  %d dBm%s ", sa->ssid[0] ? sa->ssid : "<hidden>", sa->channel, sa->rssi,
                     sa->channel > 14 ? "  (5 GHz)" : "");
    if (wide) {
        if (sa && vis(y - 1)) put(X0 + IW - 16 - (int)strlen(sl), y - 1, (int)strlen(sl), hl, "%s", sl);
    } else {
        if (sa && vis(y)) { put(X0, y, 1, TEXT, ">"); put(X0 + 1, y, IW - 1, hl, "%s", sl); }
        y++;
    }
    /* the plot fills the panel: under it only its axis, the channel numbers, a row for the zoom and,
       when a channel is tapped, its list (so the tap shows its answer without a scroll) */
    const int listed = pick >= 1 && pick <= top ? 1 + (prim[pick] ? (prim[pick] < 9 ? prim[pick] : 9) : 1) : 0;
    int bh = clip1 - clip0 - (wide ? 1 : 2) - 3 - listed;
    if (bh > 36) bh = 36;
    if (bh < 8) bh = 8;
    const int base = y + bh - 1;
    /* the scale: every 10 dB when there is room, its rows where a hump that strong would top out */
    for (int v = -30; v >= -100; v -= bh >= 24 ? 10 : 35) {
        const int r = base - 1 - (int)(frac_of(v) * (float)(bh - 3));
        if (vis(r)) put(X0, r, 4, FAINT, "%4d", v);
    }
    /* 1, 6 and 11 (the channels that do not overlap) as faint guides; the tapped one lit */
    for (int r = y; r < base; r += 2) { pcell(chx(10), r, ':', FAINT); pcell(chx(60), r, ':', FAINT); pcell(chx(110), r, ':', FAINT); }
    if (pick) for (int r = y + 1; r < base; r += 2) pcell(chx(pick * 10), r, ':', fg(LAP_UI_VALUE));
    /* the 2.4 GHz APs, strongest (and heard lately) first */
    int idx[APS], n = 0;
    for (int k = 0; k < nsorted; k++) if (ap[order[k]].channel >= 1 && ap[order[k]].channel <= 14) idx[n++] = order[k];
    /* names: the picked AP's, then the strongest, each on its own peak, as many as fit without touching */
    static char ltxt[APS][28];
    static int lx[APS], ly[APS], llen[APS];
    static bool named[APS];
    memset(named, 0, sizeof(named));
    int nl = 0, placed[APS];
    const int most = pv_w * zm / 12 > 4 ? pv_w * zm / 12 : 4;    /* a few names, more as the channels spread */
    for (int m = -1; m < n && nl < most; m++) {
        const int i = m < 0 ? si : idx[m];
        if (i < 0 || (m >= 0 && i == si) || ap[i].channel < 1 || ap[i].channel > 14) continue;
        const ap_t *a = &ap[i];
        const int c10 = a->channel * 10, cx = chx(c10);
        if (cx < pv_x0 || cx >= pv_x0 + pv_w) continue;            /* its peak is out of view */
        int room = chx(c10 + 10) - chx(c10 - 10) + 5;
        if (room < 6) room = 6;
        if (room > 24) room = 24;
        int len = snprintf(ltxt[i], sizeof(ltxt[i]), "%.*s", room, a->ssid[0] ? a->ssid : "<hidden>");
        if (len + 4 <= room) len += snprintf(ltxt[i] + len, sizeof(ltxt[i]) - (size_t)len, " %d", a->rssi);
        if (len > pv_w) continue;
        int x = cx - len / 2;
        if (x < pv_x0) x = pv_x0;
        if (x + len > pv_x0 + pv_w) x = pv_x0 + pv_w - len;
        const int r = hump_top(a, base, bh);
        bool free_ = true;
        for (int p = 0; p < nl && free_; p++) {
            const int q = placed[p];
            /* a column between names on a row, and a free row between names above one another */
            free_ = ly[q] > r + 1 || ly[q] < r - 1 || x > lx[q] + llen[q] || lx[q] > x + len;
        }
        if (!free_) continue;
        lx[i] = x; ly[i] = r; llen[i] = len;
        named[i] = true;
        placed[nl++] = i;
    }
    /* the humps: all faint, then the named ones and those on the tapped channel in colour, then the picked AP in white */
    for (int m = n - 1; m >= 0; m--)
        if (!named[idx[m]] && ap[idx[m]].channel != pick && idx[m] != si) hump(&ap[idx[m]], base, bh, FAINT, '_');
    for (int m = n - 1; m >= 0; m--) {
        const ap_t *a = &ap[idx[m]];
        if ((named[idx[m]] || a->channel == pick) && idx[m] != si) hump(a, base, bh, fg(ap_hue(a) | (pulse(a) ? LAP_BRIGHT : 0)), '_');
    }
    if (si >= 0 && sa->channel >= 1 && sa->channel <= 14) hump(sa, base, bh, fg(LAP_WHITE | LAP_BRIGHT), '=');
    for (int p = nl - 1; p >= 0; p--) {
        const int i = placed[p];
        if (vis(ly[i])) put(lx[i], ly[i], llen[i], i == si ? hl : fg(ap_hue(&ap[i]) | LAP_BRIGHT), "%s", ltxt[i]);
    }
    if (!n) {
        /* an empty band: the noise floor shimmering */
        const uint32_t t = lap_millis() / 200u;
        for (int x = pv_x0; x < pv_x0 + pv_w; x++) pcell(x, base, (x * 7 + (int)t) % 5 ? '_' : '.', FAINT);
        if (vis(y + bh / 2 - 1)) put(pv_x0 + 2, y + bh / 2 - 1, pv_w - 2, DIM, "no 2.4 GHz access points yet");
    }
    plot.r0 = y;
    y = base + 1;
    /* the axis; ^ the channel being heard */
    if (vis(y)) {
        lap_hline(pv_x0, y, pv_w, '-', fg(LAP_UI_FRAME));
        if (h > 0 && chan >= 1 && chan <= top) pcell(chx(chan * 10), y, '^', fg(LAP_UI_LIVE));
    }
    y++;
    /* channel numbers, following the pan */
    if (vis(y)) {
        for (int c = 1; c <= top; c++) {
            if (cpc < 3 && !(c % 2) && c != 6 && c != pick) continue;
            const int x = chx(c * 10) - (c > 9 ? 1 : 0), len = c > 9 ? 2 : 1;
            if (x < pv_x0 || x + len > pv_x0 + pv_w) continue;
            put(x, y, len, c == pick ? hl : c == 1 || c == 6 || c == 11 ? TEXT : DIM, "%d", c);
            if (cpc >= 9 && x + len + 6 <= pv_x0 + pv_w) put(x + len + 1, y, 4, FAINT, "%u", lap_dot11_channel_mhz((unsigned)c));
        }
    }
    y++;
    plot.r1 = y;
    plot.x0 = pv_x0; plot.w = pv_w;
    plot.live = true;
    /* zoomed: where the view sits in 1..top; at ALL, what the finger can do */
    if (vis(y)) {
        if (zm > 1) {
            const int ow = pv_x0 + pv_w + 3 <= X0 + IW ? pv_w : pv_w - 3;      /* room for the top channel after it */
            put(X0, y, 4, DIM, "%4d", 1);
            for (int i = 0; i < ow; i++) {
                const int c10 = -15 + i * slots * 10 / ow;
                const bool in = c10 >= vl && c10 < vl + pv_sp;
                const bool edge = in && (i == 0 || -15 + (i - 1) * slots * 10 / ow < vl || i == ow - 1 || -15 + (i + 1) * slots * 10 / ow >= vl + pv_sp);
                lap_put_cell(pv_x0 + i, y, edge ? (c10 < vl + pv_sp / 2 ? '[' : ']') : in ? '=' : '.', in ? fg(LAP_UI_FRAME | LAP_BRIGHT) : FAINT);
            }
            put(pv_x0 + ow + 1, y, 2, DIM, "%d", top);
        } else put(pv_x0, y, pv_w, DIM, pv_w >= 41 ? "ZOOM to spread; tap a channel for its APs" : "ZOOM to spread; tap a channel");
    }
    y++;
    /* the tapped channel's APs, strongest first; tap one to pick it */
    if (pick >= 1 && pick <= top) {
        if (vis(y)) {
            put(X0, y, IW, hl, " CH %d ", pick);
            put(X0 + 6 + (pick > 9), y, IW - 7, DIM, " %u MHz  %d AP%s on it, %d over it", lap_dot11_channel_mhz((unsigned)pick), prim[pick],
                prim[pick] == 1 ? "" : "s", spread[pick] - prim[pick]);
            if (y < HITN) hit[y] = (int16_t)(HIT_CH + pick);
        }
        y++;
        for (int m = 0; m < n; m++) {
            const ap_t *a = &ap[idx[m]];
            if (a->channel != pick) continue;
            if (vis(y)) {
                const bool on = idx[m] == si;
                if (on) lap_fill(X0, y, IW, 1, ' ', hl);
                put(X0, y, 2, on ? hl : fg(ap_hue(a) | LAP_BRIGHT), "%c", on ? '>' : '#');
                put(X0 + 2, y, IW - 2, on ? hl : TEXT, "%-22.22s %4d %-8s", a->ssid[0] ? a->ssid : "<hidden>", a->rssi, security(a));
                if (IW - 39 >= 3) meter(39, y, IW - 39 > 24 ? 24 : IW - 39, a, on);
                if (y < HITN) hit[y] = (int16_t)(HIT_AP + idx[m]);
            }
            y++;
        }
        if (!prim[pick]) row(&y, 2, DIM, "none on it");
    }
    y++;
    wrap(0, &y, IW, DIM, "Each hump is one AP: its 20 MHz over its channel and two either side, as tall as it is strong. "
                         "The strongest carry their names; the AP picked on ACCESS POINTS is white. 1, 6 and 11 do not overlap. "
                         "^: the channel being heard. ZOOM, then drag sideways to pan; tap a channel or a row below for its APs.");
    y++;
    row(&y, 0, fg(LAP_UI_VALUE), "%4s %5s %8s %10s", "Ch", "APs", "Overlap", "Strongest");
    for (int c = 1; c <= top; c++) {
        if (!prim[c] && !spread[c]) continue;
        char b[12] = "-";
        if (best[c] > NOSIG) snprintf(b, sizeof(b), "%d", best[c]);
        if (vis(y) && y < HITN) hit[y] = (int16_t)(HIT_CH + c);
        row(&y, 0, c == pick ? fg(LAP_UI_VALUE) : prim[c] ? TEXT : DIM, "%4d %5d %8d %10s%s", c, prim[c], spread[c], b, c == pick ? "  <" : "");
    }
    y++;
    row(&y, 0, fg(LAP_UI_VALUE), "5 GHz");
    bool any = false;
    for (int c = 32; c < CHN; c++) {
        int k, b;
        chan_aps(c, &k, &b);
        if (!k) continue;
        any = true;
        row(&y, 0, TEXT, "%4d %5d %8s %10d", c, k, "", b);
    }
    if (!any) wrap(1, &y, IW - 2, FAINT, "None heard. The T-Display's C6 scans 2.4 GHz only; a dual-band coprocessor (the Nano's C5) reports 5 GHz.");
    smax = y + scroll - first - (clip1 - first);
}
static void traffic_tab(int y)
{
    const int first = y;
    y -= scroll;
    capture_note(&y);
    if (h > 0) {
        char t[120];
        snprintf(t, sizeof(t), "%s, now on ch %u. %u frames, %u dropped by the radio.", fell_back || plan == PLAN_STAY ? "On the link channel" :
                 plan == PLAN_HOP3 ? "Hopping 1/6/11, 400 ms each" : "Hopping 1-13, 250 ms each", chan, (unsigned)frames, (unsigned)lost);
        wrap(0, &y, IW, lost ? fg(LAP_UI_ALARM) : DIM, t);
        y++;
    }
    /* frames a second, the last two minutes in braille */
    if (h > 0 || fps_n) {
        float pk = 10;
        for (int i = 0; i < fps_n; i++) if (fps[i] > pk) pk = fps[i];
        const int gw = IW - 7 > 64 ? 64 : IW - 7, gh = 3;
        if (vis(y)) put(X0, y, 6, fg(LAP_UI_FRAME), "%5.0f", pk);
        if (vis(y + gh - 1)) put(X0, y + gh - 1, 6, fg(LAP_UI_FRAME), "%5s", "0/s");
        if (vis(y) && vis(y + gh - 1)) {
            if (fps_n) lap_ui_history(X0 + 6, y, gw, gh, fps, fps_n, 0, pk, LAP_UI_LIVE);
            else lap_hline(X0 + 6, y + gh - 1, gw, '.', FAINT);
        }
        y += gh;
        row(&y, 6, FAINT, "frames a second, newest at the right");
        y++;
    }
    uint32_t maxf = 1;
    for (int c = 1; c < CHN; c++) if (ch[c].frames > maxf) maxf = ch[c].frames;
    const int bw = desk ? 40 : IW - 40;
    row(&y, 0, fg(LAP_UI_VALUE), "%3s %7s %8s %8s %5s %s", "Ch", "Frames", "Air ms", "Heard ms", "Busy", desk ? "Frames" : "");
    for (int c = 1; c < CHN; c++) {
        if (c > 13 && !ch[c].frames) continue;
        if (!lap_dot11_channel_mhz((unsigned)c)) continue;
        const unsigned busy = ch[c].listened_ms ? (unsigned)((uint64_t)ch[c].air_us / 10u / ch[c].listened_ms) : 0;
        char bz[8] = "-";
        if (ch[c].listened_ms) snprintf(bz, sizeof(bz), "%u%%", busy > 100 ? 100u : busy);
        const uint8_t at = busy >= 50 ? fg(LAP_UI_ALARM) : busy >= 20 ? fg(LAP_UI_WARN) : c == chan && h > 0 ? fg(LAP_UI_LIVE) : DIM;
        if (vis(y)) {
            put(X0, y, 39, at, "%3d %7u %8u %8u %5s", c, (unsigned)ch[c].frames, (unsigned)(ch[c].air_us / 1000u), (unsigned)ch[c].listened_ms, bz);
            if (bw > 4) lap_ui_bargraph(X0 + 39, y, bw, 1, (float)ch[c].frames / (float)maxf, -1);
        }
        y++;
    }
    y++;
    wrap(0, &y, IW, FAINT, "Air ms: the time the frames heard would take on air at their rates (6 Mb/s when the radio does not say). Busy: air time over the time spent listening on that channel.");
    smax = y + scroll - first - (clip1 - first);
}
static void history_tab(int y)
{
    const int first = y;
    y -= scroll;
    if (!nsorted) {
        /* an empty trace waiting for something to draw */
        y++;
        const int t = (int)((lap_millis() / 150u) % 24u);
        char line[40];
        for (int i = 0; i < 24; i++) line[i] = i == t ? '*' : i < t ? '_' : '.';
        line[24] = 0;
        centre(&y, fg(LAP_UI_FRAME), "-30 |                        ");
        centre(&y, fg(LAP_UI_FRAME), "    |                        ");
        char l2[48];
        snprintf(l2, sizeof(l2), "    |%s", line);
        centre(&y, fg(LAP_UI_LIVE), l2);
        centre(&y, fg(LAP_UI_FRAME), "-100+------------------------");
        y++;
        centre(&y, TEXT, "Pick an access point on the first tab.");
        smax = y + scroll - first - (clip1 - first);
        return;
    }
    if (sel >= nsorted) sel = nsorted - 1;
    const ap_t *a = &ap[order[sel]];
    char b[18] = "(scan: no BSSID)";
    if (a->has_bssid) lap_dot11_mac(b, sizeof(b), a->bssid);
    row(&y, 0, TEXT, "%s", a->ssid[0] ? a->ssid : "<hidden>");
    row(&y, 0, DIM, "%s  ch %u (%s GHz)  %s", b, a->channel, a->channel > 14 ? "5" : "2.4", security(a));
    if (vis(y)) {
        int x = lap_ui_chip(X0, y, a->channel > 14 ? "5 GHz" : "2.4 GHz", true, LAP_UI_FRAME);
        x = lap_ui_chip(x, y, security(a), true, a->secure ? LAP_UI_LIVE : LAP_UI_WARN);
        x = lap_ui_chip(x, y, a->by_capture ? "FRAMES" : "SCAN", true, LAP_UI_VALUE);
        lap_ui_chip(x, y, "HEARD", pulse(a) != 0, LAP_UI_LIVE);
    }
    y++;
    row(&y, 0, fg(signal_colour(a->rssi)), "now %d dBm   min %d   max %d", a->rssi, a->rmin, a->rmax);
    if (vis(y)) lap_ui_meter(X0, y, IW < 40 ? IW : 40, frac_of(a->rssi), signal_colour(a->rssi));
    y += 2;
    /* braille, two slots a cell, newest at the right; -30 dBm at the top, -100 at the bottom */
    const int gh = desk ? 8 : 7, x0 = X0 + 5;
    int cols = (IW - 6) < HISTN / 2 ? IW - 6 : HISTN / 2;
    if (cols < 1) cols = 1;
    const int n = cols * 2;
    float v[HISTN];
    const uint32_t s = slot_now();
    bool heard[HISTN];
    for (int i = 0; i < n; i++) {
        const uint32_t k = s - (uint32_t)(n - 1 - i);
        const int x = k > a->hslot || a->hslot - k >= HISTN || k > s ? NOSIG : a->hist[k % HISTN];
        heard[i] = x != NOSIG;
        v[i] = x == NOSIG ? -100.0f : (float)x;
    }
    if (vis(y)) put(X0, y, 4, FAINT, "-30");
    if (vis(y + gh / 2)) put(X0, y + gh / 2, 4, FAINT, "-65");
    if (vis(y + gh - 1)) put(X0, y + gh - 1, 4, FAINT, "-100");
    if (vis(y) && vis(y + gh - 1)) lap_ui_history(x0, y, cols, gh, v, n, -100.0f, -30.0f, signal_colour(a->rssi));
    y += gh;
    if (vis(y))                 /* dots: slots in which it was not heard */
        for (int c = 0; c < cols; c++) lap_put_cell(x0 + c, y, !heard[2 * c] && !heard[2 * c + 1] ? '.' : '-', !heard[2 * c] && !heard[2 * c + 1] ? FAINT : fg(LAP_UI_FRAME));
    y++;
    if (vis(y)) {
        put(x0, y, 12, FAINT, "-%us", (unsigned)((n - 1) * SLOT_MS / 1000u));
        put(x0 + cols - 3, y, 3, FAINT, "now");
    }
    y += 2;
    wrap(0, &y, IW, FAINT, "One dot column every 2 s, the strongest reading in it. Dots under the trace: not heard. UP and DOWN pick another access point.");
    smax = y + scroll - first - (clip1 - first);
}

/* RECORD: a cassette that turns while it records, what it has found, how
   the card is doing, and every device in the table (tap one for its
   detail). The recorder itself is above, with the radio. */
static int16_t rhit[HITN];          /* table index + 1 each screen row picks, as drawn */
static int sort_key;
static int by_rec(const void *x, const void *y)
{
    return sr_compare(&rec.table[*(const uint16_t *)x], &rec.table[*(const uint16_t *)y], sort_key);
}
static void rec_sort(void)
{
    if (!rec.table) { rec.order_n = 0; return; }
    const uint32_t now = lap_millis();
    if (rec.order_n == rec.s.count && (rec.order_seen == rec.activity || now - rec.order_ms < 500u)) return;
    rec.order_n = rec.s.count;
    for (unsigned i = 0; i < rec.order_n; i++) rec.order[i] = (uint16_t)i;
    sort_key = ropt.sort < 3 ? ropt.sort : 0;
    qsort(rec.order, rec.order_n, sizeof(rec.order[0]), by_rec);
    rec.order_ms = now;
    rec.order_seen = rec.activity;
}
static void printable(char *out, size_t n, const char *in, const char *empty)
{
    snprintf(out, n, "%s", in[0] ? in : empty);
    for (char *c = out; *c; c++) if ((unsigned char)*c < 32 || (unsigned char)*c > 126) *c = '?';
}
static void cassette(int x, int *y)
{
    static const char *const art[5] = {
        " .------------------------------. ",
        " |  .-.  .--------------.  .-.  | ",
        " | ( o )=|              |=( o ) | ",
        " |  '-'  '--------------'  '-'  | ",
        " '-------[o]----------[o]-------' "};
    const bool live = rec_live(), saving = rec.phase == R_STOPPING || rec.phase == R_RETAIN;
    const uint32_t t = lap_millis();
    static const char spin[4] = {'|', '/', '-', '\\'};
    /* recording, the reels turn; saving, they rewind fast; idle, they drift */
    const char reel = spin[(live ? t / 120u : saving ? t / 50u : t / 700u) & 3u];
    const uint8_t reel_at = live ? fg(LAP_UI_LIVE) : saving ? fg(LAP_UI_WARN) : FAINT;
    for (int r = 0; r < 5; r++, (*y)++) {
        if (!vis(*y)) continue;
        put(x, *y, 34, fg(LAP_UI_FRAME | LAP_BRIGHT), "%s", art[r]);
        if (r == 2) {
            lap_put_cell(x + 5, *y, reel, reel_at);
            lap_put_cell(x + 28, *y, reel, reel_at);
            const uint32_t el = (live ? t - rec.start_ms : rec.end_ms - rec.start_ms) / 1000u;
            char win[16];
            if (live && rec.phase == R_STARTING) snprintf(win, sizeof(win), " starting...  ");
            else if (live) snprintf(win, sizeof(win), " REC %02u:%02u:%02u ", (unsigned)(el / 3600u % 100u), (unsigned)(el / 60u % 60u), (unsigned)(el % 60u));
            else if (saving) snprintf(win, sizeof(win), "  SAVING...   ");
            else if (rec.had_file) snprintf(win, sizeof(win), "    SAVED     ");
            else snprintf(win, sizeof(win), (t / 600u) & 1u ? "    READY_    " : "    READY     ");
            put(x + 10, *y, 14, live ? TEXT : saving ? fg(LAP_UI_WARN) : DIM, "%s", win);
            if (live && rec.phase == R_ON && ((t / 500u) & 1u)) put(x + 11, *y, 3, fg(LAP_UI_ALARM), "REC");
        }
        if (r == 3 && (live || saving)) {   /* the tape running between the reels */
            for (int k = 0; k < 14; k++)
                if ((k + (int)(t / (saving ? 50u : 150u))) % 4 == 0) lap_put_cell(x + 10 + (saving ? k : 13 - k), *y, '=', fg(LAP_UI_FRAME));
        }
    }
}
/* Idle: someone walking a street of access points, each lighting as they
   pass and leaving a mark where a row would be written. */
static void walker(int *y)
{
    const int pw = IW - 4 > 44 ? 44 : IW - 4, x0 = X0 + (IW - pw) / 2;
    if (pw < 12) return;
    const int t = (int)(lap_millis() / 260u), pos = t % (pw - 3), stride = t & 1;
    for (int r = 0; r < 3; r++, (*y)++) {
        if (!vis(*y)) continue;
        for (int c = 0; c < pw; c++) {
            const bool ap_at = c % 9 == 6;
            const bool near_ap = ap_at && c >= pos - 2 && c <= pos + 4;
            if (r == 0 && ap_at) lap_put_cell(x0 + c, *y, near_ap ? '*' : '.', near_ap ? fg(LAP_UI_LIVE) : FAINT);
            if (r == 1 && ap_at) lap_put_cell(x0 + c, *y, '|', FAINT);
            if (r == 2) lap_put_cell(x0 + c, *y, c < pos && c % 3 == 0 ? 'x' : '.', c < pos && c % 3 == 0 ? fg(LAP_UI_FRAME) : FAINT);
        }
        if (r == 0) lap_put_cell(x0 + pos + 1, *y, 'o', TEXT);
        if (r == 1) { lap_put_cell(x0 + pos, *y, '/', TEXT); lap_put_cell(x0 + pos + 1, *y, '|', TEXT); lap_put_cell(x0 + pos + 2, *y, '\\', TEXT); }
        if (r == 2) {
            if (stride) { lap_put_cell(x0 + pos, *y, '/', TEXT); lap_put_cell(x0 + pos + 2, *y, '\\', TEXT); }
            else lap_put_cell(x0 + pos + 1, *y, '|', TEXT);
        }
    }
}
static void big_count(int x, int y, int w, unsigned n, uint8_t hue)
{
    char num[12];
    snprintf(num, sizeof(num), "%u", n);
    const int digits = (int)strlen(num);
    if (y < clip0 || y + 5 > clip1) return;
    if (digits <= (w - 1) / 4)
        for (int i = 0; i < digits; i++) lap_big_char(x + i * 4, y, num[i], fg(hue | LAP_BRIGHT));
    else put(x, y + 2, w, TEXT, "%s", num);
}
static void rec_tab(int y)
{
    const int first = y;
    y -= scroll;
    for (int i = 0; i < HITN; i++) rhit[i] = 0;
    const bool live = rec_live(), saving = rec.phase == R_STOPPING || rec.phase == R_RETAIN;
    const bool wide = IW >= 84;
    const bool session = rec.table && (live || saving || rec.s.count || rec.had_file);
    const uint32_t utc = rec_utc();
    /* the cassette; on a wide screen the counters stand beside it */
    const int top = y;
    cassette(wide ? X0 : X0 + (IW > 34 ? (IW - 34) / 2 : 0), &y);
    const int cx = wide ? X0 + 38 : X0, cw = wide ? (IW - 38) / 2 : IW / 2;
    if (session) {
        int cy = wide ? top : y + 1;
        if (vis(cy)) {
            put(cx, cy, cw, fg(LAP_CYAN | LAP_BRIGHT), "WI-FI APs");
            put(cx + cw, cy, cw, fg(LAP_MAGENTA | LAP_BRIGHT), "BLUETOOTH");
        }
        big_count(cx, cy + 1, cw, rec.s.totals[0], LAP_CYAN);
        big_count(cx + cw, cy + 1, cw, rec.s.totals[1], LAP_MAGENTA);
        if (!wide) y += 8;
        else y = top + 7;
    }
    /* the lamps */
    if (vis(y)) {
        const bool fresh = live && sr_fix_fresh(&rec.fix, lap_millis());
        const char *gps = !live ? "GPS" : fresh ? "GPS FIX" : rec.gps_state == LAP_GPS_ABSENT ? "NO GPS" : "GPS WAIT";
        const char *ut = !live ? "UTC" : rec.utc_src == 1 ? "UTC GPS" : rec.utc_src == 2 ? "UTC CLOCK" : "UTC WAIT";
        const lap_chip_t left[3] = {{"REC", live, LAP_UI_ALARM},
                                    {gps, live, fresh ? LAP_UI_LIVE : LAP_UI_WARN},
                                    {ut, live, utc ? LAP_UI_LIVE : LAP_UI_WARN}};
        const lap_chip_t right[2] = {{"WI-FI", ropt.wifi != 0, LAP_UI_VALUE}, {"BLE", ropt.ble && (!live || rec.ble_h > 0), LAP_UI_VALUE}};
        lap_ui_rail(X0, y, IW, left, 3, right, 2);
    }
    y += 2;
    if (!session) {
        wrap(0, &y, IW, TEXT, "RECORD writes every Wi-Fi access point (by BSSID) and Bluetooth device it hears, with where you were, "
                              "to /sdcard/lakeshark/survey as WiGLE CSV: the files LINK's SURVEY tab wrote, read the same way.");
        y++;
        walker(&y);
        y++;
        row(&y, 0, DIM, "WI-FI ROWS %-4s BLE ROWS %-4s ONLY GPS FIX %s", ropt.wifi ? "ON" : "OFF", ropt.ble ? "ON" : "OFF", ropt.only_fix ? "ON" : "OFF");
        row(&y, 0, DIM, "SCAN EVERY %u s   KEEP %u SESSIONS", ropt.scan_s, ropt.keep);
        y++;
        if (ropt.only_fix) wrap(0, &y, IW, FAINT, "ONLY GPS FIX: nothing is taken in without a fix under 10 s old. Turn it off (MORE) to record indoors.");
        wrap(0, &y, IW, FAINT, "Wi-Fi rows need a BSSID, which comes from the beacons frame capture hears. Times come from GPS, else from the board's clock once set.");
        y++;
        if (rec.status[0]) wrap(0, &y, IW, !strncmp(rec.status, "Cannot", 6) ? fg(LAP_UI_WARN) : DIM, rec.status);
        centre(&y, fg(LAP_UI_VALUE), "REC starts a walk; MORE sets the options.");
        smax = y + scroll - first - (clip1 - first);
        return;
    }
    unsigned m[2];
    sr_minute(&rec.s, lap_millis(), m);
    const unsigned most = m[0] > m[1] ? (m[0] > 10 ? m[0] : 10) : (m[1] > 10 ? m[1] : 10);
    const int mw = IW / 2;
    for (int k = 0; k < 2; k++) {
        if (vis(y)) {
            put(X0 + k * mw, y, 9, DIM, "+%u/min", m[k]);
            lap_ui_meter(X0 + k * mw + 9, y, mw - 11, live ? (float)m[k] / (float)most : 0.0f, k ? LAP_MAGENTA : LAP_CYAN);
        }
    }
    y += 2;
    row(&y, 0, TEXT, "%u rows  %u placed  %u in table  %u evicted", rec.rows, rec.s.positioned, rec.s.count, rec.s.dropped);
    if (rec.lost) row(&y, 0, fg(LAP_UI_ALARM), "%u rows lost: the card fell behind", rec.lost);
    const char *file = strrchr(rec.path, '/');
    row(&y, 0, DIM, "file %s", rec.path[0] ? (file ? file + 1 : rec.path) : live ? "(named when the time is known)" : "(none)");
    if (live) {
        if (rec.io_error) wrap(0, &y, IW, fg(LAP_UI_ALARM), "SD write failed / stop and check card");
        else if (!utc) wrap(0, &y, IW, fg(LAP_UI_WARN), "Waiting for UTC (GPS, or the board's clock once set) / no rows written");
        else wrap(0, &y, IW, DIM, "Passive / accuracy estimate = HDOP x 5 m");
        if (ropt.only_fix && !sr_fix_fresh(&rec.fix, lap_millis())) wrap(0, &y, IW, fg(LAP_UI_WARN), "ONLY GPS FIX: waiting for a fix; nothing is taken in until then.");
        if (ropt.wifi && h <= 0) {
            wrap(0, &y, IW, fg(LAP_UI_WARN), "No frame capture: Wi-Fi rows need the BSSIDs beacons carry, so no access point is written.");
            if (reason == LAP_CAPTURE_NO_TRANSPORT) row(&y, 2, TEXT, "SYSTEM > UPDATE > C6 RADIO FW > UPDATE");
        }
        if (rec.ble_wait) wrap(0, &y, IW, fg(LAP_UI_WARN), "Bluetooth is busy elsewhere: waiting for the scanner.");
        wrap(0, &y, IW, FAINT, rec.svc ? "Keeps recording behind HOME. STOP here, or in SETTINGS > SERVICES."
                                       : "Closing WIFI SURVEY ends the session (it may not run in the background).");
    } else if (rec.status[0]) wrap(0, &y, IW, saving ? fg(LAP_UI_WARN) : DIM, rec.status);
    y++;
    /* every device in the table: two rows each in a hand, one on a desk */
    rec_sort();
    if (vis(y)) {
        put(X0, y, IW, fg(LAP_UI_VALUE), "SEEN %u  by %s", rec.order_n, SORTS[ropt.sort < 3 ? ropt.sort : 0]);
        if (IW >= 46) put(X0 + IW - 18, y, 18, FAINT, "tap again: detail");
    }
    y++;
    if (desk) row(&y, 0, fg(LAP_UI_VALUE), "  %-24s %-17s %-14s %7s %-3s %6s %4s", "NAME", "ADDRESS", "SECURITY", "SIGNAL", "FIX", "SEEN", "AGE");
    if (!rec.order_n) {
        char dots[8] = "      ";
        dots[(lap_millis() / 300u) % 6u] = '.';
        row(&y, 2, FAINT, "nothing heard yet %s", live ? dots : "");
    }
    const uint8_t hl = LAP_ATTR(LAP_BLACK, LAP_YELLOW);
    const int per = desk ? 1 : 2;
    int pick_y = -1, pick_h = per;
    for (unsigned k = 0; k < rec.order_n; k++) {
        const int idx = rec.order[k];
        const sr_entry_t *e = &rec.table[idx];
        const bool on = idx == rpick, open = on && rdetail;
        const int hgt = per + (open ? 4 : 0);
        if (on) { pick_y = y; pick_h = hgt; }
        if (y + hgt <= clip0 || y >= clip1) { y += hgt; continue; }
        char name[40], addr[18], age[8], sec[24];
        printable(name, sizeof(name), e->name, e->kind == SR_WIFI ? "<hidden>" : "(no name)");
        snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x", e->addr[0], e->addr[1], e->addr[2], e->addr[3], e->addr[4], e->addr[5]);
        if (e->kind == SR_WIFI) snprintf(sec, sizeof(sec), "%s ch%u", lap_dot11_sec_name(e->sec), e->channel);
        else snprintf(sec, sizeof(sec), "%s", e->addr_type ? "random" : "public");
        const uint8_t tag = fg((e->kind == SR_WIFI ? LAP_CYAN : LAP_MAGENTA) | LAP_BRIGHT);
        for (int r = 0; r < hgt; r++) {
            if (on && vis(y + r)) lap_fill(X0, y + r, IW, 1, ' ', hl);
            if (vis(y + r) && y + r < HITN) rhit[y + r] = (int16_t)(idx + 1);
        }
        if (vis(y)) put(X0, y, 2, on ? hl : tag, "%c", e->kind == SR_WIFI ? 'W' : 'B');
        if (desk) {
            if (vis(y)) put(X0 + 2, y, IW - 2, on ? hl : TEXT, "%-24.24s %-17s %-14.14s %3d dBm %-3s %5lux %4s", name, addr, sec, e->rssi,
                            e->positioned ? "GPS" : "--", (unsigned long)e->seen, ago(age, sizeof(age), e->last_ms));
        } else {
            if (vis(y)) put(X0 + 2, y, IW - 2, on ? hl : TEXT, "%-24.24s %4d dBm %s", name, e->rssi, e->positioned ? "GPS" : "--");
            if (vis(y + 1))
                put(X0 + 2, y + 1, IW - 2, on ? hl : FAINT, "%s %-12.12s %4lux %s", addr, sec, (unsigned long)e->seen, ago(age, sizeof(age), e->last_ms));
        }
        if (open) {
            const int d = y + per;
            char when[20] = "";
            double clat, clon;
            if (vis(d)) {
                if (e->first_s && sr_utc_text(when, e->first_s)) put(X0 + 2, d, IW - 2, hl, "first %s UTC", when);
                else put(X0 + 2, d, IW - 2, hl, "first seen before the time was known");
            }
            if (vis(d + 1)) {
                if (e->positioned) put(X0 + 2, d + 1, IW - 2, hl, "best %d dBm at %.6f,%.6f", e->rssi, e->lat, e->lon);
                else put(X0 + 2, d + 1, IW - 2, hl, "best %d dBm, without a fresh fix", e->rssi);
            }
            if (vis(d + 2)) {
                if (sr_centroid(e, &clat, &clon)) put(X0 + 2, d + 2, IW - 2, hl, "centre %.6f,%.6f from %lu placed", clat, clon, (unsigned long)e->samples);
                else put(X0 + 2, d + 2, IW - 2, hl, "no centre: never heard with a fix");
            }
            if (vis(d + 3))
                put(X0 + 2, d + 3, IW - 2, hl, "%s", e->kind == SR_BLE && e->addr_type ? "random addresses rotate: one device, many rows"
                                                : e->kind == SR_WIFI ? "the BSSID names this access point" : "a public address stays the same");
        }
        y += hgt;
    }
    y++;
    wrap(0, &y, IW, FAINT, "A position is where the board heard the strongest signal, not where the device is. "
                           "Rows: the first sighting, 6 dB stronger, or 50 m on. Keep names, addresses and places out of public examples.");
    smax = y + scroll - first - (clip1 - first);
    /* a pick moved by a key comes into view; a dragged list stays where the finger left it */
    if (show_sel && !dragging && pick_y >= 0 && (pick_y < clip0 || pick_y + pick_h > clip1))
        scroll += pick_y < clip0 ? pick_y - clip0 : pick_y + pick_h - clip1;
    if (!dragging) show_sel = false;
}

static void rec_toggle(void)
{
    if (rec_live()) rec_stop();
    else if (rec.phase == R_OFF) rec_start();
}
/* RECORD's options: LINK's SURVEY OPTIONS, changed while nothing records. */
static void rec_more(void)
{
    static const char *items[9];
    static char t[6][32];
    snprintf(t[0], sizeof(t[0]), "WI-FI ROWS: %s", ropt.wifi ? "ON" : "OFF");
    snprintf(t[1], sizeof(t[1]), "BLE ROWS: %s", ropt.ble ? "ON" : "OFF");
    snprintf(t[2], sizeof(t[2]), "ONLY GPS FIX: %s", ropt.only_fix ? "ON" : "OFF");
    snprintf(t[3], sizeof(t[3]), "SCAN EVERY: %u S", ropt.scan_s);
    snprintf(t[4], sizeof(t[4]), "KEEP SESSIONS: %u", ropt.keep);
    snprintf(t[5], sizeof(t[5]), "SORT: %s", SORTS[ropt.sort < 3 ? ropt.sort : 0]);
    items[0] = rec_live() ? "STOP RECORDING" : rec.phase != R_OFF ? "SAVING..." : "START RECORDING";
    for (int i = 0; i < 6; i++) items[1 + i] = t[i];
    items[7] = "SURVEY MENU";
    items[8] = "CLOSE";
    if (!rchoose_h && !choose_h) rchoose_h = lap_choose(items, 9);
}
static uint8_t next_step(const uint8_t *steps, int n, uint8_t now)
{
    for (int i = 0; i < n; i++) if (steps[i] > now) return steps[i];
    return steps[0];
}
static void survey_more(void);
static void rec_menu_pick(int v)
{
    if (v == 0) { rec_toggle(); return; }
    if (v == 7) { survey_more(); return; }
    if (v == 8) { lap_exit(); return; }
    if (v >= 1 && v <= 5 && rec.phase != R_OFF) { say(true, "stop recording to change the options"); return; }
    if (v == 1) ropt.wifi = !ropt.wifi;
    else if (v == 2) ropt.ble = !ropt.ble;
    else if (v == 3) ropt.only_fix = !ropt.only_fix;
    else if (v == 4) { ropt.scan_s = next_step(SCAN_STEPS, (int)sizeof(SCAN_STEPS), ropt.scan_s); scan_ms = 0; }
    else if (v == 5) ropt.keep = next_step(KEEP_STEPS, (int)sizeof(KEEP_STEPS), ropt.keep);
    else if (v == 6) { ropt.sort = (uint8_t)((ropt.sort + 1) % 3); rec.order_n = 0; }
    else return;
    rec_opts_save();
    rec_more();                 /* the list again, showing the new value */
}

static void more(void);
static void draw(void)
{
    if (!ap) return;
    W = lap_cols(); H = lap_rows();
    desk = W >= 90 && H >= 24;
    poll(12u);
    rec_step();
    save_step();
    if (choose_h > 0) {
        int v = -1;
        char none[4];
        const int r = lap_dialog_poll(choose_h, none, sizeof(none), &v);
        if (r) {
            choose_h = 0;
            if (r == 1) {
                if (v == 0) { lap_wifi_scan(); scan_ms = lap_millis(); say(false, "scanning"); }
                else if (v == 1) { plan = (plan + 1) % 3; start(); if (h <= 0) say(true, "Wi-Fi: %s", why); }
                else if (v == 2) { if (drop_link) { drop_link = false; start(); } else if (!confirm_h) confirm_h = lap_confirm("Hopping channels drops the board's Wi-Fi link (LINK, remote screens, network apps). Allow it?"); }
                else if (v == 3) save_begin();
                else if (v == 4) { paused = !paused; say(false, "%s", paused ? "paused" : "surveying"); }
                else if (v == 5) { naps = over = 0; memset(ch, 0, sizeof(chan_t) * CHN); frames = 0; fps_n = 0; fps_frames = 0; sel = scroll = pick = 0; say(false, "cleared"); }
                else if (v == 6) start();
                else if (v == 7) rec_more();
                else if (v == 8) lap_exit();
            }
        }
    }
    if (rchoose_h > 0) {
        int v = -1;
        char none[4];
        const int r = lap_dialog_poll(rchoose_h, none, sizeof(none), &v);
        if (r) { rchoose_h = 0; if (r == 1) rec_menu_pick(v); }
    }
    if (confirm_h > 0) {
        int v = 0;
        char none[4];
        const int r = lap_dialog_poll(confirm_h, none, sizeof(none), &v);
        if (r) { confirm_h = 0; drop_link = r == 1 && v == 1; if (drop_link) start(); }
    }
    /* frames a second for the TRAFFIC trace */
    const uint32_t now = lap_millis();
    if (frames < fps_frames) fps_frames = frames;
    if (!fps_at) fps_at = now ? now : 1;
    for (int i = 0; i < FPSN && now - fps_at >= 1000u; i++) {
        if (fps_n == FPSN) { memmove(fps, fps + 1, sizeof(fps) - sizeof(fps[0])); fps_n--; }
        fps[fps_n++] = (float)(frames - fps_frames);
        fps_frames = frames;
        fps_at += 1000u;
    }
    if (now - fps_at >= 1000u) fps_at = now;
    /* a sideways drag on the zoomed plot pans it; the swipe it ends with is its own, not a tab change */
    const bool panning = pan_follow();
    int sw = lap_swipe();
    if ((sw == LAP_SWIPE_LEFT || sw == LAP_SWIPE_RIGHT) && pan_owns_touch()) sw = LAP_SWIPE_NONE;
    if (sw == LAP_SWIPE_LEFT) { tab = (tab + 1) % TABS; scroll = 0; }
    else if (sw == LAP_SWIPE_RIGHT) { tab = (tab + TABS - 1) % TABS; scroll = 0; }
    if (sw == LAP_SWIPE_LEFT || sw == LAP_SWIPE_RIGHT) lap_scroll_stop(&drag);
    kr = lap_ui_keys_rows(4);
    tr = desk ? 1 : lap_ui_keys_rows(3);
    hh = H - 1 - tr - kr - 2 - 5 >= 12 ? 5 : 0;
    if (tab == T_CHANNELS && H - 1 - tr - kr - 2 - hh < 24) hh = 0;     /* a short screen gives the plot the header's rows */
    ty = 1 + hh;
    clip0 = ty + tr + 1; clip1 = H - kr - 1;
    X0 = 2; IW = W - 4;
    dragging = panning || lap_scroll_follow(&drag, &scroll, smax, clip0, clip1);
    if (scroll > smax) scroll = smax > 0 ? smax : 0;
    sort();
    if (sel >= nsorted) sel = nsorted ? nsorted - 1 : 0;

    lap_clear(LAP_ATTR(LAP_WHITE, LAP_BLACK));
    const uint8_t tb = LAP_ATTR(LAP_BLACK, LAP_CYAN);
    lap_fill(0, 0, W, 1, ' ', tb);
    char st[32];
    if (paused) snprintf(st, sizeof(st), " PAUSED ");
    else if (h <= 0) snprintf(st, sizeof(st), " SCAN ONLY ");
    else snprintf(st, sizeof(st), " %s ch %u ", fell_back || plan == PLAN_STAY ? "LINK" : "HOP", chan);
    put(1, 0, W - 1, tb, "WIFI SURVEY  %d APs", naps);
    if (rec_live() && W >= 40) put(20 + (naps > 9) + (naps > 99), 0, 6, (lap_millis() / 500u) & 1u ? LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_RED) : tb, " REC ");
    put(W - (int)strlen(st), 0, (int)strlen(st), h <= 0 ? LAP_ATTR(LAP_BLACK, LAP_YELLOW) : paused ? LAP_ATTR(LAP_BLACK, LAP_YELLOW) : LAP_ATTR(LAP_BLACK, LAP_GREEN), "%s", st);
    if (hh) header(1);
    if (desk)
        for (int i = 0, x = 1; i < TABS; i++) {
            put(x, ty, W - x, i == tab ? LAP_ATTR(LAP_BLACK, LAP_YELLOW) : LAP_ATTR(LAP_WHITE, LAP_BLACK), " %s ", tabs[i]);
            x += (int)strlen(tabs[i]) + 3;
        }
    else {
        char n[16];
        snprintf(n, sizeof(n), "%d/%d", tab + 1, TABS);
        art_strip(ty, W, tr, tabs[tab], n, false);   /* one board key bar a frame: see lap_art.h */
    }
    /* the panel the tab scrolls in, its name on the rule */
    char title[48];
    if (tab == T_APS) snprintf(title, sizeof(title), "ACCESS POINTS %d", naps);
    else if (tab == T_CHANNELS) snprintf(title, sizeof(title), "CHANNELS");
    else if (tab == T_TRAFFIC) snprintf(title, sizeof(title), "TRAFFIC %u FRAMES", (unsigned)frames);
    else if (tab == T_RECORD) snprintf(title, sizeof(title), "RECORD %s", rec_live() ? "LIVE" : rec.phase != R_OFF ? "SAVING" : "");
    else snprintf(title, sizeof(title), "HISTORY");
    lap_box(0, clip0 - 1, W, clip1 - clip0 + 2, title, fg(LAP_UI_FRAME));
    memset(hit, 0, sizeof(hit));
    plot.live = false;
    if (tab == T_APS) aps_tab(clip0);
    else if (tab == T_CHANNELS) channels_tab(clip0);
    else if (tab == T_TRAFFIC) traffic_tab(clip0);
    else if (tab == T_RECORD) rec_tab(clip0);
    else history_tab(clip0);
    if (smax < 0) smax = 0;
    /* more above or below: marks on the frame, never over the list */
    if (scroll > 0) lap_put_cell(W - 1, clip0, '^', fg(LAP_UI_VALUE));
    if (scroll < smax) lap_put_cell(W - 1, clip1 - 1, 'v', fg(LAP_UI_VALUE));
    /* A note for four seconds over the title; the keys stay where they are. */
    if (status[0] && lap_millis() - status_ms < 4000u) {
        const uint8_t a = status_bad ? LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_RED) : LAP_ATTR(LAP_BLACK, LAP_WHITE);
        lap_fill(0, 0, W, 1, ' ', a);
        put(1, 0, W - 2, a, "%s", status);
    }
    /* on CHANNELS the third key zooms, on RECORD it records (SCAN stays on MORE and S) */
    static char rtime[12];
    if (rec_live()) {
        const uint32_t el = (lap_millis() - rec.start_ms) / 1000u;
        snprintf(rtime, sizeof(rtime), "%u:%02u", (unsigned)(el / 60u), (unsigned)(el % 60u));
    }
    const lap_btn_t third = tab == T_CHANNELS ? (lap_btn_t){"ZOOM", zoom_name(), 'z', zm > 1, false, false} :
                            tab == T_RECORD ? (lap_btn_t){rec_live() ? "STOP" : rec.phase != R_OFF ? "SAVING" : "REC", rec_live() ? rtime : NULL, 'r',
                                                          rec_live(), rec.phase != R_OFF && !rec_live(), false} :
                            (lap_btn_t){"SCAN", NULL, 's', false, false, false};
    const lap_btn_t k[4] = {{"UP", NULL, 0, false, false, false}, {"DOWN", NULL, 0, false, false, false}, third,
                            {"MORE", tab == T_RECORD ? "options" : NULL, 'm', false, false, false}};
    lap_ui_keys(0, H - kr, W, kr, k, 4, -1);
}

static void survey_more(void)
{
    static const char *items[9];
    static char hop[32], drop[32];
    snprintf(hop, sizeof(hop), "CHANNELS: %s", plan == PLAN_HOP13 ? "HOP 1-13" : plan == PLAN_HOP3 ? "HOP 1/6/11" : "STAY");
    snprintf(drop, sizeof(drop), "ALLOW LINK DROP: %s", drop_link ? "YES" : "NO");
    items[0] = "SCAN NOW"; items[1] = hop; items[2] = drop; items[3] = "SAVE CSV";
    items[4] = paused ? "RESUME" : "PAUSE"; items[5] = "CLEAR"; items[6] = "RESTART CAPTURE"; items[7] = "RECORD OPTIONS"; items[8] = "CLOSE";
    if (!choose_h && !rchoose_h) choose_h = lap_choose(items, 9);
}
static void more(void)
{
    if (tab == T_RECORD) rec_more();
    else survey_more();
}
/* RECORD: UP and DOWN walk the devices in the order shown. */
static void rec_step_pick(int by)
{
    if (!rec.order_n) return;
    int k = 0;
    if (rpick >= 0) for (unsigned i = 0; i < rec.order_n; i++) if (rec.order[i] == rpick) { k = (int)i + (by > 0 ? 1 : -1); break; }
    if (k < 0) k = 0;
    if (k >= (int)rec.order_n) k = (int)rec.order_n - 1;
    rpick = rec.order[k];
    rdetail = false;
    show_sel = true;
}
static void step(int by)
{
    if (tab == T_RECORD) { rec_step_pick(by); return; }
    if (tab == T_APS || tab == T_HISTORY) {
        sel += by > 0 ? 1 : -1;
        if (sel >= nsorted) sel = nsorted - 1;
        if (sel < 0) sel = 0;
        show_sel = true;
        return;
    }
    scroll += by;
    if (scroll > smax) scroll = smax;
    if (scroll < 0) scroll = 0;
}
static void go_tab(int t)
{
    tab = (t + TABS) % TABS;
    scroll = 0;
    show_sel = true;
    lap_scroll_stop(&drag);
}
static bool key(lap_key_t k, char c)
{
    const int page = clip1 - clip0 > 2 ? clip1 - clip0 - 1 : 1;
    status[0] = 0;
    switch (k) {
    case LAP_KEY_LEFT: go_tab(tab - 1); return true;
    case LAP_KEY_RIGHT: case LAP_KEY_TAB: go_tab(tab + 1); return true;
    case LAP_KEY_UP: case LAP_KEY_WHEEL_UP: step(-1); return true;
    case LAP_KEY_DOWN: case LAP_KEY_WHEEL_DOWN: step(1); return true;
    case LAP_KEY_PGUP: scroll = scroll > page ? scroll - page : 0; return true;
    case LAP_KEY_PGDN: scroll = scroll + page > smax ? smax : scroll + page; return true;
    case LAP_KEY_ENTER:
        if (tab == T_APS) go_tab(T_HISTORY);
        else if (tab == T_RECORD && rpick >= 0) { rdetail = !rdetail; show_sel = true; }
        else more();
        return true;
    case LAP_KEY_MENU: more(); return true;
    case LAP_KEY_CHAR: break;
    default: return false;
    }
    switch (c >= 'A' && c <= 'Z' ? c + 32 : c) {
    case 'w': save_begin(); return true;
    case 'r': rec_toggle(); return true;
    case 's': lap_wifi_scan(); scan_ms = lap_millis(); return true;
    case 'p': case ' ': paused = !paused; return true;
    case 'h': plan = (plan + 1) % 3; start(); return true;
    case 'm': more(); return true;
    case 'z': zoom_set(zm % ZOOMS + 1); return true;
    case '+': case '=': zoom_set(zm + 1); return true;
    case '-': zoom_set(zm - 1); return true;
    case ',': case '<': pan_by(-10); return true;
    case '.': case '>': pan_by(10); return true;
    default:
        if (c >= '1' && c <= '0' + TABS) { go_tab(c - '1'); return true; }
        return false;
    }
}
static bool touch(int col, int r)
{
    if (r >= H - kr) {
        if (dragging) return true;
        const int b = lap_ui_key_hit(col, r);
        if (b < 0) return true;
        if (b == 2 && tab == T_CHANNELS) { zoom_set(zm % ZOOMS + 1); return true; }
        if (b == 2 && tab == T_RECORD) { rec_toggle(); return true; }
        if (b == 2) { lap_wifi_scan(); scan_ms = lap_millis(); say(false, "scanning"); return true; }
        const int page = clip1 - clip0 > 2 ? clip1 - clip0 - 1 : 1;
        const bool picks = tab == T_APS || tab == T_HISTORY || (tab == T_RECORD && rec.order_n);
        if (b == 0) { if (picks) step(-1); else scroll = scroll > page ? scroll - page : 0; }
        else if (b == 1) { if (picks) step(1); else scroll = scroll + page > smax ? smax : scroll + page; }
        else more();
        return true;
    }
    if (r >= ty && r < ty + tr) {
        if (desk) {
            for (int i = 0, x = 1; i < TABS; i++) {
                const int k = (int)strlen(tabs[i]) + 2;
                if (col >= x && col < x + k) { go_tab(i); return true; }
                x += k + 1;
            }
        } else {
            const int t = col * 3 / (W > 0 ? W : 1) - 1;    /* the strip's three keys: < name > */
            if (t) go_tab(tab + t);
        }
        return true;
    }
    if (r >= 1 && r < ty) return true;                  /* the header: nothing to press */
    if (r == 0) {
        if (status[0] && lap_millis() - status_ms < 4000u) status[0] = 0;
        else more();
        return true;
    }
    if (r < clip0 || r >= clip1 || dragging) return true;
    if (tab == T_CHANNELS) {
        if (pan_owns_touch()) return true;              /* the end of a pan, not a tap */
        int c = 0;
        if (plot.live && r >= plot.r0 && r < plot.r1 && col >= plot.x0 && col < plot.x0 + plot.w) c = channel_at(col);
        else if (r < HITN && hit[r] >= HIT_AP && hit[r] - HIT_AP < naps) {
            for (int k = 0; k < nsorted; k++) if (order[k] == hit[r] - HIT_AP) sel = k;
            return true;
        } else if (r < HITN && hit[r] >= HIT_CH && hit[r] < HIT_AP) c = hit[r] - HIT_CH;
        if (c) pick = pick == c ? 0 : c;
        return true;
    }
    if (tab == T_APS) {
        /* rows as aps_tab laid them out: a note on top, a header on desks */
        int y = clip0 - scroll;
        if (h <= 0 || fell_back) {
            /* the note's height is not kept; count rows the same way */
            const int keep0 = clip0, keep1 = clip1;
            clip0 = clip1 = -1;
            capture_note(&y);
            clip0 = keep0; clip1 = keep1;
            y++;
        }
        if (desk) y++;
        const int per = desk ? 1 : 2, k = r >= y ? (r - y) / per : -1;
        if (k >= 0 && k < nsorted) { if (k == sel) go_tab(T_HISTORY); else sel = k; }
        return true;
    }
    if (tab == T_RECORD) {
        /* a device: the first tap picks it, the second opens its detail */
        if (r < HITN && rhit[r] > 0) {
            const int idx = rhit[r] - 1;
            if (idx == rpick) rdetail = !rdetail;
            else { rpick = idx; rdetail = false; }
        }
        return true;
    }
    /* Every tap on this screen is the app's: one left unused reaches the
       board as UP, ENTER or DOWN by where it fell, and ENTER acts. */
    return true;
}

static void enter(void)
{
    if (ap) {
        /* Back to a survey that kept recording behind HOME (the service):
           everything is as it was; only the screen starts afresh. */
        memset(&drag, 0, sizeof(drag));
        dragging = false;
        choose_h = confirm_h = rchoose_h = 0;
        if (rec.phase != R_OFF) go_tab(T_RECORD);
        return;
    }
    rec_opts_load();
    rpick = -1; rdetail = false; rchoose_h = 0;
    ap = calloc(APS, sizeof(*ap));
    ch = calloc(CHN, sizeof(*ch));
    if (!ap || !ch) { lap_log("no memory"); lap_exit(); return; }
    naps = over = nsorted = 0;
    tab = scroll = smax = sel = 0;
    zm = 1; vl = -15; pick = 0;
    memset(&plot, 0, sizeof(plot));
    plot.slots = 17; plot.top = 13;
    memset(&pan, 0, sizeof(pan));
    pan.x0 = pan.y0 = -1;
    memset(hit, 0, sizeof(hit));
    memset(&drag, 0, sizeof(drag));
    dragging = show_sel = false;
    fps_n = 0; fps_at = 0; fps_frames = 0;
    kr = 1; tr = 1; hh = 0; ty = 1; X0 = 2; IW = 1;
    paused = drop_link = false;
    frames = lost = scans = 0;
    scan_ms = 0; seq = lap_wifi_seq() - 1u; last_draw = 0;
    status[0] = 0;
    memset(&save, 0, sizeof(save));
    choose_h = confirm_h = 0;
    chan = 0;
    plan = PLAN_HOP13;
    start();
    if (h <= 0 && reason == LAP_CAPTURE_NO_TRANSPORT) say(true, "scan only: frame capture needs the C6 monitor firmware");
}
static void leave(void)
{
    /* Recording with the service: the app stays loaded behind HOME and the
       tick carries on (svc_tick); nothing is freed. */
    if (rec.phase != R_OFF && rec.svc && lap_service_running()) return;
    rec_abort();
    if (rec.svc) { rec.svc = false; lap_service_stop(); }
    if (h > 0) lap_capture_stop(LAP_CAPTURE_WIFI);
    h = 0;
    free(ap); free(ch);
    ap = NULL; ch = NULL;
    rec_free();
}

LAP_APP(.id = "wifi_survey", .name = "WIFI SURVEY", .sub = "APs, channels, record", .version = REC_VERSION, .author = "LakeShark",
        .icon = LAP_ICON_WAVE, .color = LAP_GREEN, .flags = LAP_CAP_RADIO_RX | LAP_CAP_SERVICE,
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
