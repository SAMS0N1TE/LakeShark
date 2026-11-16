/* BUGHUNT: one app for trackers and bugs. Receive only.

   A sweep listens three ways at once: the LoRa radio steps across the
   cellular uplink bands where GPS trackers transmit and the ISM bands (RSSI,
   max-hold per step and per time slot), Bluetooth names tracker tags, and
   the Wi-Fi capture watches for camera makers and steady uploads. bh_core.c
   turns that into ranked verdicts; HUNT parks on one and leads the way with
   a strength bar, a Geiger tick or a rising tone, and a turn-in-a-circle
   direction plot from the compass.

   Between and through sweeps, the tag watch (bh_watch.c, what was the BLE
   WATCH app) keeps every Bluetooth device heard since the app opened: TAGS
   lists them, follows rotating addresses, logs to the card and raises the
   "a tag is following you" alert on any tab.

   Nothing here transmits. */
#include "lakeshark_app.h"
#include "lakeshark_ui.h"
#include "lap_scroll.h"
#include "bh_core.h"
#include "bh_watch.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROWS(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* ---- settings ---- */
typedef struct {
    uint8_t magic, region, room_i, car_i, sound, hop;
    /* 1.1, from BLE WATCH (it kept none on the card): the following alert's
       minutes, possible trackers alert too, the card log, HUNT's sound */
    uint8_t follow_min, possible, log, tone;
} cfg_t;
#define CFG_MAGIC 0xB6u
#define CFG_V10 6                       /* bytes BUGHUNT 1.0 saved */
enum { TONE_AUTO, TONE_TICKS, TONE_TONE };
static cfg_t cfg;
static const uint8_t ROOM_MIN[] = { 2, 4, 8 };
static const uint8_t CAR_MIN[] = { 10, 15, 30 };
static const char *const REGION_NAMES[] = { "", "US", "ARGENTINA", "US + AR" };
static const char *const REGION_SHORT[] = { "", "US", "AR", "US+AR" };
static const char *const TONE_NAMES[] = { "AUTO", "TICKS", "TONE" };

/* lap_save hands back a card job that has to be polled to free its slot:
   an app has four, and the tag log needs them too. 1.0 never polled, so a
   few setting changes used them all up. Saves go one at a time from
   cfg_pump(), once a frame; a failed one is tried again. */
static bool cfg_dirty;
static int cfg_job = -1;
static uint32_t cfg_at;
static void cfg_save(void) { cfg_dirty = true; }
static void cfg_pump(void)
{
    if (cfg_job >= 0) {
        const int r = lap_sd_poll(cfg_job);
        if (r == LAP_SD_PENDING) return;
        if (r < 0) cfg_dirty = true;
        cfg_job = -1;
    }
    if (!cfg_dirty || lap_millis() - cfg_at < 300u) return;
    cfg_at = lap_millis();
    const int id = lap_save("cfg", &cfg, sizeof(cfg));
    if (id >= 0) { cfg_dirty = false; cfg_job = id; }
}
/* The tag watch's settings back into cfg, saved when one changed. */
static void cfg_sync(void)
{
    uint32_t fm = 0;
    bool pos = false, lg = false;
    bhw_get_config(&fm, &pos, &lg);
    const uint8_t m = (uint8_t)(fm / 60000u);
    if (m && (m != cfg.follow_min || pos != (cfg.possible != 0) || lg != (cfg.log != 0))) {
        cfg.follow_min = m; cfg.possible = pos; cfg.log = lg;
        cfg_save();
    }
}
/* No saved choice yet: the plan for where the board last knew it was (the
   ARGENTINA plan keeps band 28 tower downlink out of the uplink verdicts),
   both plans when it never had a position. */
static uint8_t region_guess(void)
{
    lap_gps_status_t g = {.size = sizeof(g), .version = 1};
    if (lap_gps_status(&g) || g.fix_age_ms == UINT32_MAX) return BH_REG_BOTH;
    const float la = (float)g.position.lat, lo = (float)g.position.lon;
    if (la < -21.5f && la > -55.5f && lo > -74.0f && lo < -53.0f) return BH_REG_AR;
    if (la > 18.0f && la < 72.0f && lo > -170.0f && lo < -65.0f) return BH_REG_US;
    return BH_REG_BOTH;
}

static void cfg_load(void)
{
    cfg_t c;
    memset(&c, 0, sizeof(c));
    const int n = lap_load("cfg", &c, sizeof(c));
    if ((n == (int)sizeof(c) || n == CFG_V10) && c.magic == CFG_MAGIC && c.region >= 1 && c.region <= 3 && c.room_i < 3 &&
        c.car_i < 3) {
        cfg = c;
        if (n == CFG_V10) { cfg.follow_min = 10; cfg.possible = 0; cfg.log = 1; cfg.tone = TONE_AUTO; }   /* 1.0's: add 1.1's */
    } else cfg = (cfg_t){ CFG_MAGIC, region_guess(), 1, 1, 1, 0, 10, 0, 1, TONE_AUTO };
    if (cfg.follow_min < 2 || cfg.follow_min > 60) cfg.follow_min = 10;
    if (cfg.tone > TONE_TONE) cfg.tone = TONE_AUTO;
}

/* ---- state ---- */
static bh_t *h;
static bh_find_t F[BH_MAX_FIND];
static int nf, sel;
static uint32_t find_ms, best_seen;     /* last refresh; strongest conf already announced */
enum { TAB_SWEEP, TAB_FIND, TAB_TAGS, TAB_HUNT, TABS };
static int tab;
/* the following alert: its tag and when it fired (lap_millis), for the banner */
static uint32_t alert_uid, alert_at;
static char alert_text[96];
#define ALERT_SHOW_MS 30000u
enum { ST_IDLE, ST_SWEEP, ST_DONE };
static int state;
static uint32_t now_ms, ended_ms;

/* the LoRa radio */
static int radio = -1;
static char radio_name[16];
static bool has24, radio_rssi;
static int rf_h;                        /* scanner, or the HUNT packet session */
static bool rf_pk;
/* Scanner version 2 (firmware after 2.23.1) reads the RSSI through every
   dwell and reports each step's peak; 1 is one reading a pass. Starts at 2
   and drops to 1 for good when the firmware refuses it. */
static int scan_ver = 2;
static int chunk, nchunks, rf_fail;
static uint32_t pass_ms, rf_retry_ms, steps_seen, rate_ms, rate_steps;
static float step_rate;                 /* RSSI steps a second, measured */
static uint64_t last_ts, live_hz;
static int live_lv = BH_NONE;
static char rf_why[48];

/* Bluetooth and Wi-Fi */
static int ble_cap, ble_scan, ble_kind;   /* ble_scan < 0: busy, tried again */
static bool ble_ended;                  /* a replayed capture ran out */
static char ble_why[96];
static uint8_t *buf;                    /* one frame or advert */
#define BUF_CAP 2400
static int wifi_h, wifi_reason, wifi_ch;
static char wifi_why[72];
static uint32_t ble_seen_ms, wifi_seen_ms;

/* HUNT */
static bh_find_t tgt;
static bool hunting;
static int hunt_from = 1;               /* the tab HUNT goes back to (TAB_FIND) */
static float lv_fast = -1200, lv_slow = -1200;
static int lv_peak = BH_NONE, lv_last = BH_NONE, hunt_floor = -1000;
static uint32_t lv_ms, tick_next, tick_ms;
static bh_dir_t dir;
static bool hunt_scan;                  /* HUNT on a one-frequency version 2 scan */
static float heading = -1;
static bool have_compass;
#define HHIST 64
static float hhist[HHIST];
static int hhist_n;

/* dialogs */
static int dlg = 0, dlg_kind;

/* ---- layout ---- */
static int W, H;
static bool wide;
static int kr, tr, ty, sx, sw, hx, hy, hw, hh, px, py, pw, ph;
static int clip0, clip1, smax, scroll;
static lap_scroll_t drag;
static bool dragging;
static lap_btn_t keys[4];
typedef struct { int16_t y0, y1; int16_t idx; } hit_t;
static hit_t hits[BH_MAX_FIND];
static int hit_n;

static char status_line[72];
static uint32_t status_ms;
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status_line, sizeof(status_line), fmt, ap);
    va_end(ap);
    status_ms = lap_millis();
}

/* ---- drawing helpers ---- */
static uint8_t A(uint8_t fg) { return LAP_ATTR(fg, LAP_BLACK); }
static int put(int x, int y, int w, uint8_t attr, const char *fmt, ...)
{
    if (w <= 0 || y < clip0 || y >= clip1) return 0;
    char t[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    if ((int)strlen(t) > w) t[w] = 0;
    lap_put_str(x, y, t, attr);
    return (int)strlen(t);
}
static void cell(int x, int y, int16_t ch, uint8_t attr)
{
    if (y >= clip0 && y < clip1 && x >= 0 && x < W) lap_put_cell(x, y, ch, attr);
}
/* Words wrapped to w; returns the rows used (at most max). */
static int wrap(int x, int y, int w, int max, uint8_t attr, const char *s)
{
    int rows = 0;
    while (*s && rows < max) {
        while (*s == ' ') s++;
        int n = (int)strlen(s);
        if (n > w) {
            n = w;
            while (n > 0 && s[n] != ' ') n--;
            if (n == 0) n = w;
        }
        char t[160];
        snprintf(t, sizeof(t), "%.*s", n, s);
        if (rows == max - 1 && (int)strlen(s) > n && n >= 3) { t[n - 1] = '.'; t[n - 2] = '.'; }
        put(x, y + rows, w, attr, "%s", t);
        s += n;
        rows++;
    }
    return rows;
}
static int wrap_rows(int w, int max, const char *s)
{
    int rows = 0;
    while (*s && rows < max) {
        while (*s == ' ') s++;
        int n = (int)strlen(s);
        if (n > w) { n = w; while (n > 0 && s[n] != ' ') n--; if (n == 0) n = w; }
        s += n;
        rows++;
    }
    return rows;
}
static bool blink(uint32_t period) { return (now_ms / period) % 2u == 0; }

/* braille canvas */
#define CV_W 64
#define CV_H 32
static struct {
    int x, y, w, h;
    float ax;
    uint8_t dot[CV_H][CV_W], at[CV_H][CV_W];
} cv;
static void cv_begin(int x, int y, int w, int hgt)
{
    memset(cv.dot, 0, sizeof(cv.dot));
    memset(cv.at, 0, sizeof(cv.at));
    cv.x = x; cv.y = y;
    cv.w = w < 0 ? 0 : w > CV_W ? CV_W : w;
    cv.h = hgt < 0 ? 0 : hgt > CV_H ? CV_H : hgt;
    int cw = 0, ch = 0;
    lap_cell_px(&cw, &ch);
    cv.ax = cw > 0 && ch > 0 ? ((float)ch / 4.0f) / ((float)cw / 2.0f) : 1.0f;
}
static void cv_dot(int x, int y, uint8_t attr)
{
    static const uint8_t bit[2][4] = { { 0x01, 0x02, 0x04, 0x40 }, { 0x08, 0x10, 0x20, 0x80 } };
    if (x < 0 || y < 0 || x >= cv.w * 2 || y >= cv.h * 4) return;
    cv.dot[y / 4][x / 2] |= bit[x & 1][y & 3];
    if (attr) cv.at[y / 4][x / 2] = attr;
}
static int rnd(float f) { return (int)(f < 0 ? f - 0.5f : f + 0.5f); }
static void cv_line(int x0, int y0, int x1, int y1, uint8_t attr)
{
    const int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
    const int stx = x0 < x1 ? 1 : -1, sty = y0 < y1 ? 1 : -1;
    int e = dx + dy;
    for (int i = 0; i < 512; i++) {
        cv_dot(x0, y0, attr);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += stx; }
        if (e2 <= dx) { e += dx; y0 += sty; }
    }
}
static void cv_ring(float cx, float cy, float r, int gap, uint8_t attr)
{
    const int n = (int)(r * 7.0f) + 10;
    for (int i = 0; i < n; i++) {
        if (gap && i % gap == 0) continue;
        const float t = 6.2831853f * (float)i / (float)n;
        cv_dot(rnd(cx + r * cosf(t)), rnd(cy + r * sinf(t) / cv.ax), attr);
    }
}
static void cv_flush(void)
{
    for (int r = 0; r < cv.h; r++)
        for (int c = 0; c < cv.w; c++)
            if (cv.dot[r][c]) cell(cv.x + c, cv.y + r, LAP_BRAILLE(cv.dot[r][c]), cv.at[r][c] ? cv.at[r][c] : A(LAP_CYAN));
}

static void mhz(uint32_t khz, char *out, size_t cap)
{
    if (khz % 1000u) snprintf(out, cap, "%u.%03u", (unsigned)(khz / 1000u), (unsigned)(khz % 1000u));
    else snprintf(out, cap, "%u.000", (unsigned)(khz / 1000u));
}
static uint8_t level_hue(int db_over)
{
    return db_over >= 25 ? (LAP_RED | LAP_BRIGHT) : db_over >= 15 ? (LAP_YELLOW | LAP_BRIGHT) : db_over >= 8 ? (LAP_GREEN | LAP_BRIGHT)
         : db_over >= 3 ? LAP_GREEN : LAP_BLUE;
}
static int16_t shade(int db_over)
{
    return db_over >= 25 ? LAP_BLOCK : db_over >= 15 ? LAP_SHADE_75 : db_over >= 8 ? LAP_SHADE_50 : LAP_SHADE_25;
}
static int ref_or(int dflt) { return h && h->ref != BH_NONE ? h->ref : dflt; }
/* Under this a reading is noise: drawn quiet, coloured only above it. */
static int qtop(void) { return h && h->qtop != BH_NONE ? h->qtop : ref_or(-1100) + 100; }

/* ---- the radio ---- */
static void find_radio(void)
{
    lap_radio_info_t r[LAP_RADIOS_MAX];
    const int n = lap_radio_list(r, LAP_RADIOS_MAX);
    radio = -1;
    for (int i = 0; i < n; i++) {
        if (r[i].kind != LAP_RADIO_KIND_LR2021 && r[i].kind != LAP_RADIO_KIND_SX1262 && r[i].kind != LAP_RADIO_KIND_SIM) continue;
        lap_radio_modes_t m = { .size = sizeof(m), .version = 1 };
        if (lap_radio_modes(i, &m) || !(m.modes & LAP_MODE_RSSI) || !(m.modes & (LAP_MODE_LORA | LAP_MODE_FSK))) continue;
        radio = i;
        radio_rssi = true;
        snprintf(radio_name, sizeof(radio_name), "%s", r[i].name);
        /* 2.4 GHz only where the radio says it tunes there */
        lap_radio_info2_t i2;
        memset(&i2, 0, sizeof(i2));
        has24 = false;
        if (!lap_radio_info2(i, &i2, sizeof(i2)))
            for (int k = 0; k < i2.frequency_count && k < LAP_RADIO_RANGES_MAX; k++)
                if (i2.frequencies[k].min_hz <= 2402000000ull && i2.frequencies[k].max_hz >= 2482000000ull) has24 = true;
        if (r[i].kind == LAP_RADIO_KIND_SX1262) has24 = false;
        return;
    }
}
static lap_packet_config_t rx_cfg(uint64_t hz)
{
    lap_packet_config_t c;
    memset(&c, 0, sizeof(c));
    c.size = sizeof(c); c.version = 1;
    c.frequency_hz = hz;
    c.modulation = LAP_MOD_LORA;    /* widest the SX1262 listens: 500 kHz */
    c.bandwidth_hz = 500000; c.sf = 7; c.coding_rate = 5; c.preamble = 8; c.sync_word = 0x12;
    return c;
}
static void rf_stop(void)
{
    if (rf_h > 0) { if (rf_pk) lap_packet_close(rf_h); else lap_scan_stop(rf_h); }
    rf_h = 0;
    rf_pk = hunt_scan = false;
}
/* One scan list at a time: the scanner takes 64 frequencies. (Restarting it
   each list also kept firmware 2.23.1's scans clear of the retune-count bug
   that failed them after about 125 steps.) */
static int scan_open(const uint64_t *hz, int n, uint32_t dwell_v2)
{
    lap_scan_config_t c;
    memset(&c, 0, sizeof(c));
    c.size = sizeof(c); c.radio = radio;
    c.count = (uint16_t)n;
    memcpy(c.frequencies, hz, sizeof(hz[0]) * (size_t)n);
    c.receiver = rx_cfg(c.frequencies[0]);
    for (;;) {
        c.version = (uint16_t)scan_ver;
        c.dwell_ms = scan_ver == 2 ? dwell_v2 : 20;
        c.hang_ms = 0;
        c.squelch_tenths_dbm = scan_ver == 2 ? INT16_MIN : 32767;   /* v2: every step; v1: never holds */
        const int r = lap_scan_start(&c);
        if (r == LAP_RX_EINVAL && scan_ver == 2) { scan_ver = 1; continue; }
        return r;
    }
}
static void rf_chunk(int k)
{
    rf_stop();
    nchunks = bh_chunks(h, LAP_SCAN_MAX);
    if (radio < 0 || !nchunks) return;
    chunk = k % nchunks;
    uint64_t hz[LAP_SCAN_MAX];
    const int n = bh_chunk(h, chunk, LAP_SCAN_MAX, hz);
    const int r = scan_open(hz, n, 3);
    if (r > 0) { rf_h = r; rf_why[0] = 0; }
    else { snprintf(rf_why, sizeof(rf_why), r == LAP_RX_EBUSY ? "radio busy" : "radio refused (%d)", r); rf_retry_ms = now_ms + 2000; }
}
static void rf_poll_sweep(void)
{
    if (rf_h <= 0) {
        if (state == ST_SWEEP && radio >= 0 && now_ms >= rf_retry_ms) rf_chunk(chunk);
        return;
    }
    lap_scan_status_t st = { .size = sizeof(st), .version = 1 };
    lap_scan_status(rf_h, &st);
    lap_packet_meta_t m = { .size = sizeof(m), .version = 1 };
    if (scan_ver == 2) {                    /* every step's peak, in order */
        while (lap_scan_next(rf_h, &m) > 0) {
            if (m.rssi_tenths_dbm == INT16_MIN) continue;
            bh_rf_sample(h, m.frequency_hz, m.rssi_tenths_dbm, now_ms);
            steps_seen++;
            if (m.rssi_tenths_dbm > -1250) { live_hz = m.frequency_hz; live_lv = m.rssi_tenths_dbm; }
        }
    } else {
        if (!lap_radio_rssi(rf_h, &m) && m.timestamp_us != last_ts && m.rssi_tenths_dbm != INT16_MIN) {
            last_ts = m.timestamp_us;
            bh_rf_sample(h, m.frequency_hz, m.rssi_tenths_dbm, now_ms);
            steps_seen++;
            if (m.rssi_tenths_dbm > -1250) { live_hz = m.frequency_hz; live_lv = m.rssi_tenths_dbm; }
        }
        lap_packet_meta_t hit;
        while (lap_scan_next(rf_h, &hit) > 0) {}
    }
    if (st.state == LAP_IO_FAILED) {
        if (st.error == LAP_RX_ERANGE || st.error == LAP_RX_EUNSUPPORTED) {
            bh_rf_refused(h, st.frequency_hz);
            snprintf(rf_why, sizeof(rf_why), "%u MHz refused", (unsigned)(st.frequency_hz / 1000000u));
            rf_chunk(chunk);
        } else {
            rf_fail++;
            rf_chunk(chunk + 1);
        }
        return;
    }
    if (st.sweeps >= 1) {
        if (chunk + 1 >= nchunks) {
            if (pass_ms) h->sweep_ms = now_ms - pass_ms;
            pass_ms = now_ms;
        }
        rf_chunk(chunk + 1);
    }
}

/* ---- Bluetooth and Wi-Fi ---- */
static void sources_start(void)
{
    lap_caps_t c;
    memset(&c, 0, sizeof(c));
    if (lap_caps(&c, sizeof(c))) return;
    have_compass = (c.hardware & LAP_HW_IMU) != 0;
    ble_why[0] = 0;
    ble_ended = false;
    if (c.hardware & LAP_HW_BLE) {
        /* the advert capture (API 2.7) where the board grants it, else the scan */
        lap_ble_capture_config_t bc = { .size = sizeof(bc), .version = 1 };
        const int r = lap_ble_capture_start(&bc);
        if (r > 0) { ble_cap = r; ble_kind = 1; }
        else {
            lap_capture_status_t st;
            if (!lap_capture_status(LAP_CAPTURE_BLE, &st)) snprintf(ble_why, sizeof(ble_why), "capture: %s", st.why);
            ble_scan = lap_ble_scan_start();
            if (ble_scan > 0) ble_kind = 2;
            else if (!ble_why[0]) snprintf(ble_why, sizeof(ble_why), "scan refused (%d)", ble_scan);
        }
    } else snprintf(ble_why, sizeof(ble_why), "this board has no Bluetooth");
    bhw_source(ble_kind, ble_why, false);
    if (c.hardware & LAP_HW_WIFI) {
        /* Listen where the board already is unless the owner said hopping may
           drop the link (MORE): a hop also stops the station's reconnects. */
        lap_wifi_capture_config_t wc = { .size = sizeof(wc), .version = 1, .flags = cfg.hop ? LAP_CAPTURE_DROP_LINK : 0,
                                         .types = LAP_WIFI_MGMT | LAP_WIFI_DATA, .dwell_ms = 400, .snap_bytes = 64 };
        if (cfg.hop) for (; wc.channel_count < 13; wc.channel_count++) wc.channels[wc.channel_count] = (uint8_t)(wc.channel_count + 1);
        const int r = lap_wifi_capture_start(&wc);
        lap_capture_status_t st;
        if (!lap_capture_status(LAP_CAPTURE_WIFI, &st)) { wifi_reason = st.reason; snprintf(wifi_why, sizeof(wifi_why), "%s", st.why); }
        if (r > 0) wifi_h = r;
    }
}
static void sources_stop(void)
{
    if (ble_cap > 0) lap_capture_stop(ble_cap);
    if (ble_scan > 0) lap_ble_scan_stop(ble_scan);
    if (wifi_h > 0) lap_capture_stop(wifi_h);
    ble_cap = ble_scan = ble_kind = wifi_h = 0;
    bhw_source(0, "stopped", false);
}
static bool ble_on(void) { return ble_cap > 0 || ble_scan > 0; }
static void hunt_level(int tenths);
static void target_text(const bh_find_t *f, char *out, size_t cap);
/* How long a level stays current: an RF reading comes ~25 times a second; a
   tag's adverts every 2 s or so, and the scanner hears only some of them. */
static uint32_t fresh_ms(void) { return tgt.target == BH_T_FREQ ? 3000u : 10000u; }
static bool same_target_ble(const uint8_t addr[6])
{
    if (!memcmp(addr, tgt.addr, 6)) return true;
    /* the tag rotated: the tag watch links the new address to the chain */
    const bw_dev_t *d = tgt.uid ? bhw_dev(tgt.uid) : NULL;
    return d && !memcmp(d->addr, addr, 6);
}
static void ble_in(const uint8_t addr[6], uint8_t type, uint8_t pdu, int rssi, const uint8_t *ad, size_t n, uint64_t ts_us)
{
    bhw_advert(addr, type, pdu, rssi, ad, n, ts_us);         /* every advert, whatever the tab */
    if (state == ST_SWEEP || hunting) bh_ble_advert(h, addr, type, pdu, rssi, ad, n, now_ms);
    ble_seen_ms = now_ms;
    if (hunting && tgt.target == BH_T_BLE && same_target_ble(addr)) hunt_level(rssi * 10);
}
static void sources_poll(void)
{
    if (ble_cap > 0) {
        lap_capture_meta_t m;
        for (int i = 0, n; i < 128 && (n = lap_capture_read(ble_cap, buf, BUF_CAP, &m)) >= 0; i++)
            ble_in(m.addr, m.addr_type, m.kind, m.rssi, buf, (size_t)n, m.timestamp_us);
        if (!(lap_frame() % 50)) {
            lap_capture_status_t st;
            if (!lap_capture_status(ble_cap, &st) && (st.reason == LAP_CAPTURE_REPLAY_END) != ble_ended) {
                ble_ended = st.reason == LAP_CAPTURE_REPLAY_END;
                bhw_source(ble_kind, ble_why, ble_ended);
            }
        }
    }
    if ((ble_scan == LAP_IO_EBUSY || ble_scan == LAP_IO_EAGAIN) && !(lap_frame() % 25) && (ble_scan = lap_ble_scan_start()) > 0) {
        ble_kind = 2;                       /* the scanner was busy; it is free now */
        bhw_source(ble_kind, ble_why, false);
    }
    if (ble_scan > 0) {
        lap_ble_advert_t a;
        for (int i = 0, n; i < 16 && (n = lap_ble_advert_next(ble_scan, &a, buf, BUF_CAP)) >= 0; i++)
            ble_in(a.addr, a.addr_type, a.scan_response ? LAP_BLE_SCAN_RSP : 0xFF, a.rssi, buf, (size_t)n, (uint64_t)a.at_ms * 1000u);
    }
    if (wifi_h > 0) {
        lap_capture_meta_t m;
        const uint32_t began = lap_millis();
        for (int i = 0, n; i < 128 && lap_millis() - began < 8u && (n = lap_capture_read(wifi_h, buf, BUF_CAP, &m)) >= 0; i++) {
            wifi_seen_ms = now_ms;
            if (m.channel) wifi_ch = m.channel;
            if (state == ST_SWEEP || hunting) bh_wifi_frame(h, buf, (size_t)n, m.original_length, m.rssi, now_ms);
            if (hunting && tgt.target == BH_T_WIFI && n >= 16 && !memcmp(buf + 10, tgt.addr, 6)) hunt_level(m.rssi * 10);
        }
        if (!(lap_frame() % 25)) {
            lap_capture_status_t st;
            if (!lap_capture_status(LAP_CAPTURE_WIFI, &st)) { wifi_reason = st.reason; if (st.channel) wifi_ch = st.channel; }
        }
    }
}

/* ---- sweep and hunt ---- */
static void refresh_findings(void)
{
    if (!h || state == ST_IDLE) { nf = 0; return; }
    nf = bh_findings(h, state == ST_DONE ? ended_ms : now_ms, F, BH_MAX_FIND);
    if (sel >= nf) sel = nf ? nf - 1 : 0;
    find_ms = now_ms;
    if (state == ST_SWEEP && nf && F[0].conf >= 65 && F[0].conf > best_seen) {
        best_seen = F[0].conf;
        if (cfg.sound) lap_sound(LAP_SOUND_CHIRP);
        lap_haptic(LAP_HAPTIC_ALERT);
        say("%s", F[0].title);
    }
}
static void to_tab(int t)
{
    tab = (t + TABS) % TABS;
    scroll = 0;
    lap_scroll_stop(&drag);
}
static void sweep_start(int mode)
{
    if (hunting) { hunting = false; rf_stop(); lap_tone_stop(); }
    const uint32_t dur = (mode == BH_CAR ? CAR_MIN[cfg.car_i] : ROOM_MIN[cfg.room_i]) * 60000u;
    bh_init(h, mode, cfg.region, has24, dur, now_ms);
    state = ST_SWEEP;
    nf = 0; sel = 0; best_seen = 0; pass_ms = 0; rf_fail = 0; live_lv = BH_NONE; live_hz = 0;
    steps_seen = rate_steps = rate_ms = 0; step_rate = 0;
    if (!wifi_h && !ble_cap && !ble_scan) sources_start();
    rf_chunk(0);
    if (cfg.sound) lap_sound(LAP_SOUND_KEY);
    to_tab(TAB_SWEEP);
    say("%s sweep: phone in airplane mode", mode == BH_CAR ? "Car" : "Room");
}
static void sweep_end(void)
{
    if (state != ST_SWEEP) return;
    rf_stop();
    state = ST_DONE;
    ended_ms = now_ms;
    refresh_findings();
    if (cfg.sound) lap_sound(LAP_SOUND_ROGER);
    char t[80];
    int strong = 0;
    for (int i = 0; i < nf; i++) strong += F[i].conf >= 60;
    lap_log("sweep: v%d %d steps, pass %u ms, %.0f steps/s, floor %d, %u samples, %u adverts, %u frames, %d verdicts", scan_ver,
            h->nstep, (unsigned)h->sweep_ms, (double)step_rate, h->ref / 10, (unsigned)h->samples, (unsigned)h->ble_adverts,
            (unsigned)h->wifi_frames, nf);
    for (int i = 0; i < nf && i < 6; i++) lap_log("verdict %d%% %s | %.90s", F[i].conf, F[i].title, F[i].detail);
    if (strong) snprintf(t, sizeof(t), "Sweep done. %d thing%s worth checking.", strong, strong == 1 ? "" : "s");
    else snprintf(t, sizeof(t), "Sweep done. Nothing stands out.");
    if (cfg.sound) lap_say(t);
    to_tab(TAB_FIND);
}
/* HUNT on one target: a frequency, a Bluetooth tag (followed through its
   address changes by the tag watch) or a Wi-Fi station. */
static void hunt_begin(const bh_find_t *f, int from)
{
    tgt = *f;
    hunt_from = from;
    if (state == ST_SWEEP) { rf_stop(); state = ST_DONE; ended_ms = now_ms; }
    if (hunting && (rf_pk || hunt_scan)) rf_stop();
    lap_tone_stop();
    hunting = true;
    lv_fast = lv_slow = -1200; lv_peak = BH_NONE; lv_last = BH_NONE; lv_ms = 0; hhist_n = 0; tick_next = 0;
    bh_dir_reset(&dir);
    hunt_floor = tgt.target == BH_T_FREQ ? ref_or(-1080) : -1000;
    if (tgt.target == BH_T_BLE) {
        /* start from what the tag watch last heard, so the bar moves at once */
        const bw_dev_t *d = tgt.uid ? bhw_dev(tgt.uid) : NULL;
        if (d && bhw_now() - d->last_ms < 10000u) hunt_level((int)(d->smooth * 10.0f));
    }
    if (tgt.target == BH_T_FREQ) {
        rf_stop();
        if (radio >= 0) {
            /* version 2: the peak of every 8 ms of listening (one report a
               worker pass, ~45 a second on the board), so a burst between
               frames still moves the bar; else one reading a frame */
            const uint64_t hz = (uint64_t)tgt.khz * 1000u;
            int r = scan_ver == 2 ? scan_open(&hz, 1, 8) : -1;
            if (r > 0 && scan_ver == 2) { rf_h = r; rf_pk = false; hunt_scan = true; }
            else {
                lap_packet_config_t c = rx_cfg(hz);
                r = lap_packet_open(radio, &c);
                if (r > 0) { rf_h = r; rf_pk = true; }
                else snprintf(rf_why, sizeof(rf_why), "radio refused (%d)", r);
            }
        }
    } else if (!wifi_h && !ble_cap && !ble_scan) sources_start();
    to_tab(TAB_HUNT);
}
static void hunt_start(int i)
{
    if (i < 0 || i >= nf) return;
    bh_find_t f = F[i];
    /* a sweep's tag: HUNT follows it in the tag watch, which hears every advert */
    if (f.target == BH_T_BLE) f.uid = bhw_uid_of(f.addr, f.addr_type);
    hunt_begin(&f, TAB_FIND);
}
/* HUNT a tag from TAGS (or the following alert). */
static void hunt_tag(uint32_t uid)
{
    const bw_dev_t *d = bhw_dev(uid);
    if (!d) return;
    bh_find_t f;
    memset(&f, 0, sizeof(f));
    f.target = BH_T_BLE;
    f.kind = d->alert ? BH_F_BLE_FOLLOW : BH_F_BLE_TAG;
    f.uid = d->uid;
    memcpy(f.addr, d->addr, 6);
    f.addr_type = d->addr_type;
    f.level = (int16_t)(d->rssi * 10);
    f.count = (uint16_t)(d->rotations + 1);
    char label[32];
    bw_label(d, label, sizeof(label));
    snprintf(f.title, sizeof(f.title), "%s%s  %s", d->alert ? "! " : "", label, bw_class_tag(d->ad.cls));
    snprintf(f.detail, sizeof(f.detail), "%s", bw_class_note(d->ad.cls));
    bhw_pick_set(d->uid);
    if (alert_uid && bhw_dev(alert_uid) == d) alert_uid = 0;      /* hunting it answers the alert: the banner goes */
    hunt_begin(&f, TAB_TAGS);
}
static void hunt_stop(void)
{
    if (!hunting) return;
    hunting = false;
    if (rf_pk || hunt_scan) rf_stop();
    hunt_scan = false;
    lap_tone_stop();
    to_tab(hunt_from);
}
/* The next or the previous: the tag beside it in TAGS, or the next finding. */
static void hunt_step(int by)
{
    if (!hunting) return;
    if (hunt_from == TAB_TAGS && tgt.target == BH_T_BLE) {
        const uint32_t u = bhw_step(tgt.uid, by);
        if (u && u != tgt.uid) hunt_tag(u);
        return;
    }
    if (nf < 2) return;
    sel = (sel + by + nf) % nf;
    hunt_start(sel);
}
/* The bar: dB over the floor it spans. A tag reads -100 (cold) to -30 dBm
   (hot, in your hand) as BLE WATCH's meter did; a frequency 60 dB over its
   floor. */
static int hunt_span(void) { return tgt.target == BH_T_BLE ? 700 : 600; }
static float hunt_frac(float tenths)
{
    const float f = (tenths - (float)hunt_floor) / (float)hunt_span();
    return f < 0 ? 0 : f > 1 ? 1 : f;
}
/* Ticks or a tone: AUTO is BLE WATCH's rising tone for a tag (an advert
   comes every second or two, a pitch says how strong at once) and the
   Geiger tick for a frequency or a station (readings many times a second). */
static bool hunt_tone(void) { return cfg.tone == TONE_TONE || (cfg.tone == TONE_AUTO && tgt.target == BH_T_BLE); }
static void hunt_level(int tenths)
{
    lv_last = tenths;
    lv_ms = now_ms;
    if (lv_fast < -1150) lv_fast = lv_slow = (float)tenths;
    lv_fast += ((float)tenths - lv_fast) * 0.35f;
    lv_slow += ((float)tenths - lv_slow) * 0.04f;
    if (lv_peak == BH_NONE || tenths > lv_peak) lv_peak = tenths;
    if (hhist_n == HHIST) { memmove(hhist, hhist + 1, sizeof(hhist[0]) * (HHIST - 1)); hhist_n--; }
    hhist[hhist_n++] = (float)tenths / 10.0f;
    if (heading >= 0) bh_dir_add(&dir, heading, tenths);
}
static void hunt_poll(void)
{
    lap_motion_t m;
    heading = lap_motion(&m) && m.heading >= 0 ? m.heading : -1;
    if (hunt_scan && rf_h > 0) {
        lap_packet_meta_t pm;
        while (lap_scan_next(rf_h, &pm) > 0)
            if (pm.rssi_tenths_dbm != INT16_MIN) hunt_level(pm.rssi_tenths_dbm);
    } else if (rf_pk && rf_h > 0) {
        lap_packet_meta_t pm = { .size = sizeof(pm), .version = 1 };
        if (!lap_radio_rssi(rf_h, &pm) && pm.timestamp_us != last_ts && pm.rssi_tenths_dbm != INT16_MIN) {
            last_ts = pm.timestamp_us;
            hunt_level(pm.rssi_tenths_dbm);
        }
    }
    if (hunt_tone()) {
        /* the tone: pitch and rate follow the smoothed signal, quiet once
           the target has not been heard for 3 s */
        const float q = hunt_frac(lv_fast);
        if (cfg.sound && lv_ms && now_ms - lv_ms < 3000u && !lap_tone_busy() && now_ms >= tick_next) {
            lap_tone(300 + (int)(q * 1700.0f), 50, 70, LAP_WAVE_SINE);
            tick_next = now_ms + 900u - (uint32_t)(q * 780.0f);
            tick_ms = now_ms;
        }
        return;
    }
    /* Geiger: faster and higher as it gets stronger; silent with no signal */
    const bool fresh = lv_ms && now_ms - lv_ms < fresh_ms();
    if (cfg.sound && fresh && now_ms >= tick_next) {
        const int over = (int)lv_fast - hunt_floor;
        const int hz = 500 + (over > 0 ? over : 0) * 3;
        lap_tone(hz > 3200 ? 3200 : hz, 14, 60, LAP_WAVE_SQUARE);
        tick_next = now_ms + bh_geiger_ms((int)lv_fast, hunt_floor);
        tick_ms = now_ms;
    }
}

/* ---- the screen ---- */
static void layout(void)
{
    W = lap_cols(); H = lap_rows();
    wide = W >= 90 && W > H * 2;
    kr = lap_ui_keys_rows(4);
    tr = lap_ui_keys_rows(3);
    if (tr > 5) tr = 5;
    if (tr < 3) tr = 3;
    if (!wide) {
        hx = 0; hy = 1; hw = W; hh = 10;
        sx = 0; sw = W; ty = hy + hh;
        py = ty + tr; px = 0; pw = W; ph = H - kr - py;
    } else {
        hw = 38; hx = 1; hy = 1; hh = H - kr - 1;
        sx = hw + 2; sw = W - sx; ty = 1;
        py = ty + tr; px = sx; pw = sw; ph = H - kr - py;
    }
}

/* The emblem: a magnifier over a bug. The legs walk, a glint runs round
   the rim, and the lens throws rings while a sweep runs. */
static void emblem(int x, int y, bool big)
{
    const uint32_t t = now_ms;
    const bool walk = (t / 300u) % 2u;
    const uint8_t rim = A(LAP_CYAN | LAP_BRIGHT), glass = A(LAP_CYAN);
    uint8_t bug = A(LAP_GREEN | LAP_BRIGHT);
    if (state == ST_DONE && nf && F[0].conf >= 60) bug = A(blink(400) ? (LAP_RED | LAP_BRIGHT) : LAP_RED);
    if (hunting) {
        const int over = (int)lv_fast - hunt_floor;
        bug = A(level_hue(over / 10));
    }
    static const char *const lens[7] = {
        "  .-'''-.   ",
        " / .   . \\  ",
        "|         | ",
        "|         | ",
        "|         | ",
        " \\ '   ' /  ",
        "  '-...-'\\\\ ",
    };
    for (int r = 0; r < 7; r++) put(x, y + r, 12, r == 0 || r == 6 ? rim : glass, "%s", lens[r]);
    cell(x + 10, y + 7, '\\', rim); cell(x + 11, y + 7, '\\', rim);
    /* the bug */
    put(x + 3, y + 2, 5, bug, walk ? " \\ / " : " | | ");
    put(x + 3, y + 3, 5, bug, walk ? "=(@)=" : "-(@)-");
    put(x + 3, y + 4, 5, bug, walk ? " / \\ " : " \\ / ");
    /* a glint running round the rim */
    static const int8_t gx[] = { 4, 6, 8, 9, 9, 9, 8, 6, 4, 2, 0, 0, 0, 2 }, gy[] = { 0, 0, 1, 2, 3, 4, 5, 6, 6, 6, 4, 3, 2, 1 };
    const int g = (int)((t / 90u) % 14u);
    cell(x + gx[g], y + gy[g], '*', A(LAP_WHITE | LAP_BRIGHT));
    if (state == ST_SWEEP && big) {
        /* rings out of the lens, one every 800 ms */
        for (int k = 0; k < 2; k++) {
            const int ph2 = (int)(((t + (uint32_t)k * 400u) / 200u) % 4u);
            const uint8_t at = A(ph2 < 2 ? (LAP_GREEN | LAP_BRIGHT) : LAP_GREEN);
            cell(x + 12 + ph2, y + 2, ')', at);
            cell(x + 12 + ph2, y + 3, ')', at);
            cell(x + 12 + ph2, y + 4, ')', at);
        }
    }
}

/* Block figures: digits and letters 3x5, ':' and '-' in one or two cells. */
static int big_text(int x, int y, int maxw, const char *s, uint8_t at)
{
    int cx = x;
    for (const char *c = s; *c; c++) {
        if (*c == ':') { if (cx + 1 > x + maxw) break; cell(cx, y + 1, LAP_BLOCK, at); cell(cx, y + 3, LAP_BLOCK, at); cx += 2; continue; }
        if (*c == '-') { if (cx + 3 > x + maxw) break; cell(cx, y + 2, LAP_BLOCK, at); cell(cx + 1, y + 2, LAP_BLOCK, at); cx += 3; continue; }
        if (*c == ' ') { cx += 2; continue; }
        if (cx + 3 > x + maxw) break;
        if (y >= clip0 && y + 5 <= clip1) lap_big_char(cx, y, *c, at);
        cx += 4;
    }
    return cx - x;
}

static void title_bar(void)
{
    const uint8_t tb = LAP_ATTR(LAP_BLACK, LAP_GREEN);
    lap_fill(0, 0, W, 1, ' ', tb);
    put(1, 0, 8, tb, "BUGHUNT");
    const char *legs = (now_ms / 250u) % 2u ? "/(@)\\" : "\\(@)/";
    put(9, 0, 5, state == ST_SWEEP || hunting ? LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_GREEN) : tb, "%s", legs);
    char st[40];
    uint8_t sa;
    if (radio < 0 && state != ST_IDLE) { snprintf(st, sizeof(st), " NO LORA RADIO "); sa = LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_RED); }
    else if (hunting) { snprintf(st, sizeof(st), " HUNTING "); sa = LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_RED); }
    else if (state == ST_SWEEP) {
        const uint32_t left = h->dur_ms > now_ms - h->t0_ms ? (h->dur_ms - (now_ms - h->t0_ms)) / 1000u : 0;
        snprintf(st, sizeof(st), " %s %u:%02u ", h->mode == BH_CAR ? "CAR" : "ROOM", (unsigned)(left / 60), (unsigned)(left % 60));
        sa = LAP_ATTR(LAP_BLACK, LAP_YELLOW | LAP_BRIGHT);
    } else if (state == ST_DONE) { snprintf(st, sizeof(st), " DONE  %d FOUND ", nf); sa = LAP_ATTR(LAP_BLACK, LAP_WHITE | LAP_BRIGHT); }
    else { snprintf(st, sizeof(st), " READY "); sa = LAP_ATTR(LAP_BLACK, LAP_WHITE); }
    const int sl = (int)strlen(st);
    int alerts = 0;
    bhw_counts(NULL, &alerts, NULL);
    if (alerts) put(15, 0, W - 16 - sl, blink(300) ? LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_RED) : LAP_ATTR(LAP_RED | LAP_BRIGHT, LAP_GREEN),
                    " ! %d FOLLOWING ", alerts);
    else put(15, 0, W - 16 - sl, tb, "%s", W >= 50 ? "receive only" : "");
    put(W - sl, 0, sl, sa, "%s", st);
    if (alert_uid && lap_millis() - alert_at < ALERT_SHOW_MS) {
        /* the following alert, over the title for 30 s: a tap hunts the tag */
        const uint8_t pa = blink(300) ? LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_RED) : LAP_ATTR(LAP_RED | LAP_BRIGHT, LAP_BLACK);
        lap_fill(0, 0, W, 1, ' ', pa);
        put(1, 0, W - 2, pa, "! %s  tap: HUNT", alert_text);
    } else if (status_line[0] && now_ms - status_ms < 4000u) {
        const uint8_t a = LAP_ATTR(LAP_BLACK, LAP_WHITE | LAP_BRIGHT);
        lap_fill(0, 0, W, 1, ' ', a);
        put(1, 0, W - 2, a, "%s", status_line);
    }
}

static void chips(int x, int y, int w)
{
    char bt[12], wf[12];
    int tags = 0;
    bhw_counts(&tags, NULL, NULL);          /* the tag watch: every tag heard since the app opened */
    snprintf(bt, sizeof(bt), "BT %d", tags);
    if (wifi_h > 0 && wifi_ch) snprintf(wf, sizeof(wf), "WIFI c%d", wifi_ch);
    else snprintf(wf, sizeof(wf), "WIFI");
    const struct { const char *t; bool on; uint8_t hue; } C[] = {
        { radio >= 0 ? (radio_name[0] ? radio_name : "LORA") : "NO LORA", rf_h > 0, LAP_GREEN },
        { bt, ble_on() && now_ms - ble_seen_ms < 3000u, LAP_BLUE },
        { wf, wifi_h > 0 && now_ms - wifi_seen_ms < 3000u, LAP_MAGENTA },
        { has24 ? "2.4G" : "", has24, LAP_YELLOW },
    };
    int cx = x;
    for (int i = 0; i < ROWS(C); i++) {
        if (!C[i].t[0]) continue;
        const int need = (int)strlen(C[i].t) + 2;
        if (cx + need > x + w) break;
        if (y >= clip0 && y < clip1) cx = lap_ui_chip(cx, y, C[i].t, C[i].on, C[i].hue) + 1;
    }
}

static void header(void)
{
    const int x = hx, y = hy;
    if (tab == TAB_TAGS) {
        /* the tag watch: a radar of every device heard, the tally, the alert */
        bhw_draw_head(x + 1, y, (wide ? hw : W) - 2, wide ? 8 : 9);
        clip0 = 0; clip1 = H;
        chips(x + 1, y + 9, hw - 2);
        if (wide) {
            put(x + 1, y + 11, hw - 2, LAP_UI_FAINT, "THE PICK");
            bhw_draw_detail(x + 1, y + 12, hw - 2, hh - 12);
            clip0 = 0; clip1 = H;
        }
        return;
    }
    int ex = x, ey = y, ix, iy, iw;
    if (!wide) { ix = x + 17; iy = y; iw = hw - 18; }
    else { ix = x + 1; iy = y + 9; iw = hw - 2; ex = x + 1; }
    emblem(ex, ey + (wide ? 0 : 1), true);
    if (wide) {
        /* landscape: the plan beside the emblem */
        put(x + 18, y + 1, hw - 19, LAP_UI_DIM, "SWEEP %s", REGION_NAMES[cfg.region]);
        put(x + 18, y + 2, hw - 19, LAP_UI_FAINT, "room %u min  car %u min", ROOM_MIN[cfg.room_i], CAR_MIN[cfg.car_i]);
        if (h && state != ST_IDLE) {
            put(x + 18, y + 4, hw - 19, LAP_UI_DIM, "%d steps  %d bands", h->nstep, h->nband);
            put(x + 18, y + 5, hw - 19, LAP_UI_DIM, "pass %u.%u s  %.0f/s", (unsigned)(h->sweep_ms / 1000u), (unsigned)(h->sweep_ms / 100u % 10u),
                (double)step_rate);
            put(x + 18, y + 6, hw - 19, LAP_UI_FAINT, "%s", scan_ver == 2 ? "peak of each 3 ms" : "one reading a step");
        } else put(x + 18, y + 4, hw - 19, LAP_UI_FAINT, "receive only");
    }
    char big[16];
    uint8_t bat = A(LAP_YELLOW | LAP_BRIGHT);
    const char *under = "";
    static char under_t[40];
    if (hunting) {
        const int v = lv_ms ? (int)(lv_fast / 10.0f) : 0;
        if (lv_ms) snprintf(big, sizeof(big), "%d", v); else snprintf(big, sizeof(big), "--");
        bat = A(lv_ms ? level_hue(((int)lv_fast - hunt_floor) / 10) : LAP_BLUE);
        under = "dBm  strength now";
        if (tgt.target == BH_T_BLE) {
            /* a tag: how near, in words, from -100 dBm (far) to -30 (in your hand) */
            snprintf(under_t, sizeof(under_t), "dBm  %s", bhw_distance_word(hunt_frac(lv_fast), lv_ms && now_ms - lv_ms < 10000u));
            under = under_t;
        }
    } else if (state == ST_SWEEP) {
        const uint32_t left = h->dur_ms > now_ms - h->t0_ms ? (h->dur_ms - (now_ms - h->t0_ms)) / 1000u : 0;
        snprintf(big, sizeof(big), "%u:%02u", (unsigned)(left / 60), (unsigned)(left % 60));
        under = h->mode == BH_CAR ? "left of the car sweep" : "left of the room sweep";
    } else if (state == ST_DONE) {
        snprintf(big, sizeof(big), "%02d", nf > 99 ? 99 : nf);
        bat = A(nf && F[0].conf >= 60 ? (LAP_RED | LAP_BRIGHT) : LAP_GREEN | LAP_BRIGHT);
        under = nf ? "findings, ranked" : "nothing stands out";
    } else {
        snprintf(big, sizeof(big), "READY");
        bat = A(blink(700) ? (LAP_GREEN | LAP_BRIGHT) : LAP_GREEN);
        under = "phone in airplane mode first";
    }
    big_text(ix, iy, iw, big, bat);
    put(ix, iy + 5, iw, LAP_UI_DIM, "%s", under);
    /* progress, or the level bar */
    float frac = 0, peak = -1;
    if (hunting && lv_ms) {
        frac = hunt_frac(lv_fast);
        peak = lv_peak == BH_NONE ? -1 : hunt_frac((float)lv_peak);
    } else if (state == ST_SWEEP) frac = (float)(now_ms - h->t0_ms) / (float)h->dur_ms;
    else if (state == ST_DONE) frac = 1;
    frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
    if (peak > 1) peak = 1;
    lap_ui_bargraph(ix, iy + 6, iw, 1, frac, peak);
    /* what the radio hears this instant */
    if (state == ST_SWEEP && live_hz && live_lv != BH_NONE) {
        char f[16];
        mhz((uint32_t)(live_hz / 1000u), f, sizeof(f));
        const int over = (live_lv - ref_or(-1100)) / 10;
        put(ix, iy + 7, iw, A(level_hue(over)), "%s MHz %4d dBm", f, live_lv / 10);
    } else if (hunting) {
        char f[24];
        if (tgt.target == BH_T_FREQ) { mhz(tgt.khz, f, sizeof(f)); put(ix, iy + 7, iw, A(LAP_UI_VALUE), "on %s MHz", f); }
        else {
            char t[40];
            target_text(&tgt, t, sizeof(t));    /* a tag where it is now, after any rotation; a station's MAC */
            put(ix, iy + 7, iw, A(LAP_UI_VALUE), "on %s", t);
        }
    } else if (radio < 0) put(ix, iy + 7, iw, A(LAP_UI_WARN), "no LoRa radio: BT + Wi-Fi only");
    else put(ix, iy + 7, iw, LAP_UI_FAINT, "%s RSSI, %s", radio_name, REGION_SHORT[cfg.region]);
    chips(wide ? ix : x + 1, wide ? iy + 8 : y + 9, wide ? iw : hw - 2);
    if (wide) {
        /* the BT and Wi-Fi tallies under the chips */
        int tags = 0, st = h ? h->nsta : 0;
        if (h) for (int i = 0; i < h->ble.n; i++) tags += !h->ble.dev[i].succ && h->ble.dev[i].ad.level >= 1;
        put(ix, iy + 10, iw, LAP_UI_DIM, "Bluetooth  %u adverts, %d tag%s", h ? (unsigned)h->ble_adverts : 0u, tags, tags == 1 ? "" : "s");
        put(ix, iy + 11, iw, LAP_UI_DIM, "Wi-Fi      %u frames, %d station%s", h ? (unsigned)h->wifi_frames : 0u, st, st == 1 ? "" : "s");
        const char *wn = wifi_h <= 0 ? (wifi_why[0] ? wifi_why : "Wi-Fi capture off")
                       : cfg.hop ? "Wi-Fi: hopping 1-13" : "Wi-Fi: one channel (MORE > WI-FI HOP)";
        if (wn[0]) wrap(ix, iy + 12, iw, 2, LAP_UI_FAINT, wn);
        if (rf_why[0]) put(ix, iy + 14, iw, A(LAP_UI_WARN), "LoRa: %s", rf_why);
        /* phone warning, the thing most likely to fool a sweep */
        const int wy = hy + hh - 3;
        if (wy > iy + 15) {
            put(ix, wy, iw, A(blink(900) ? LAP_UI_WARN : LAP_YELLOW), "!! Your phone transmits in these");
            put(ix, wy + 1, iw, A(LAP_YELLOW), "   bands: airplane mode it.");
        }
    }
}

/* < name >: three finger-sized keys (lap_art.h style, one lap_ui_keys a frame). */
static void strip(int x, int y, int w, int hgt, const char *label, const char *value, bool badge)
{
    const uint8_t fr = A(LAP_GREEN | LAP_BRIGHT), fill = A(LAP_GREEN), face = A(LAP_WHITE | LAP_BRIGHT), val = A(LAP_YELLOW | LAP_BRIGHT);
    if (hgt < 3 || w < 12) return;
    for (int i = 0; i < 3; i++) {
        const int x0 = x + i * w / 3, x1 = x + (i + 1) * w / 3, bx = x0 + (i ? 1 : 0), bwid = x1 - x0 - (i ? 1 : 0) - (i < 2 ? 1 : 0);
        lap_box(bx, y, bwid, hgt, NULL, fr);
        lap_fill(bx + 1, y + 1, bwid - 2, hgt - 2, LAP_SHADE_25, fill);
        char t[40];
        if (i != 1) {
            snprintf(t, sizeof(t), " %s ", i ? ">" : "<");
            lap_put_str(bx + (bwid - 3) / 2, y + hgt / 2, t, face);
            continue;
        }
        const int ly = y + (hgt - (value ? 2 : 1)) / 2;
        int n = snprintf(t, sizeof(t), " %.*s ", bwid - 4 > 0 ? bwid - 4 : 0, label);
        lap_put_str(bx + (bwid - n) / 2, ly, t, face);
        if (value) {
            n = snprintf(t, sizeof(t), " %.*s ", bwid - 4 > 0 ? bwid - 4 : 0, value);
            lap_put_str(bx + (bwid - n) / 2, ly + 1, t, val);
        }
        if (badge && blink(400)) lap_put_cell(bx + bwid - 2, y + 1, '*', A(LAP_RED | LAP_BRIGHT));
    }
}

/* ---- SWEEP ---- */
static const char *kind_icon(int k);
/* The plan on a frequency ruler, 300-960 MHz (2.4 GHz on its own at the
   end): where each band sits, with a scan mark running along it. */
static void plan_art(int x, int y, int w, int hgt)
{
    (void)hgt;
    const int lw = 5, rw = w - lw - 1;
    if (rw < 20) return;
    put(x, y, w, LAP_UI_FAINT, "THE PLAN  %s  %d steps", REGION_NAMES[cfg.region], h->nstep);
    const uint32_t lo = 300000, hi = 960000;
    const int run = (int)((now_ms / 60u) % (uint32_t)(rw + 20)) - 10;
    for (int b = 0; b < h->nband; b++) {
        const bh_band_t *bd = &h->band[b];
        const int yy = y + 1 + b;
        put(x, yy, lw, A(LAP_CYAN | LAP_BRIGHT), "%-4s", bd->name);
        for (int c = 0; c < rw; c++) cell(x + lw + c, yy, c % 6 ? LAP_BRAILLE(0x40) : LAP_BRAILLE(0x47), LAP_UI_FAINT);
        int c0, c1;
        if (bd->kind == BH_BAND_24G) { c0 = rw - 4; c1 = rw - 1; }
        else {
            c0 = (int)((uint64_t)(bd->lo_khz - lo) * (uint64_t)(rw - 6) / (hi - lo));
            c1 = (int)((uint64_t)(bd->hi_khz - lo) * (uint64_t)(rw - 6) / (hi - lo));
        }
        const uint8_t hue = bd->kind == BH_BAND_CELL ? LAP_YELLOW : bd->kind == BH_BAND_UHF ? LAP_MAGENTA : bd->kind == BH_BAND_24G ? LAP_RED : LAP_GREEN;
        for (int c = c0; c <= c1 && c < rw; c++) {
            const bool lit = c >= run - 1 && c <= run + 1;
            cell(x + lw + c, yy, lit ? LAP_BLOCK : LAP_SHADE_75, A(lit ? (LAP_WHITE | LAP_BRIGHT) : (uint8_t)(hue | LAP_BRIGHT)));
        }
        const int tl = (int)strlen(bd->what);
        if (c1 + 2 + tl <= rw) put(x + lw + c1 + 2, yy, tl, LAP_UI_DIM, "%s", bd->what);
        else if (c0 - tl - 1 >= 0) put(x + lw + c0 - tl - 1, yy, tl, LAP_UI_DIM, "%s", bd->what);
        else put(x + lw + c1 + 2, yy, rw - c1 - 2, LAP_UI_DIM, "%s", bd->what);
    }
    const int ry = y + 1 + h->nband;
    put(x + lw, ry, rw, LAP_UI_FAINT, "300");
    put(x + lw + (int)((uint64_t)(600000 - lo) * (uint64_t)(rw - 6) / (hi - lo)) - 1, ry, 3, LAP_UI_FAINT, "600");
    put(x + lw + rw - 13, ry, 13, LAP_UI_FAINT, "960 MHz  2.4G");
}

static void idle_art(int x, int y, int w, int hgt)
{
    /* a room and a car, a scan beam passing over both */
    static const char *const room[] = {
        "    /\\      ",
        "   /  \\     ",
        "  /____\\    ",
        "  |[]  |    ",
        "  |__[]|    ",
    };
    static const char *const car[] = {
        "    ______    ",
        "  _/  |   \\_  ",
        " |  _    _  | ",
        " '-(_)--(_)-' ",
        "              ",
    };
    const int cw = 12 + 2 + 14;
    const int ax = x + (w - cw) / 2;
    const int beam = (int)((now_ms / 120u) % (uint32_t)(cw + 8)) - 4;
    for (int r = 0; r < 5; r++) {
        put(ax, y + r, 12, A(LAP_CYAN), "%s", room[r]);
        put(ax + 14, y + r, 14, A(LAP_CYAN), "%s", car[r]);
    }
    for (int r = 0; r < 5; r++)
        for (int d = -1; d <= 1; d++) {
            const int c = ax + beam + d + (r - 2) / 2;
            if (c >= ax && c < ax + cw) cell(c, y + r, d ? LAP_SHADE_25 : LAP_SHADE_50, A(d ? LAP_GREEN : (LAP_GREEN | LAP_BRIGHT)));
        }
    put(ax, y + 5, 12, A(LAP_UI_VALUE), "SWEEP ROOM");
    put(ax + 14, y + 5, 14, A(LAP_UI_VALUE), "SWEEP CAR");
    put(ax, y + 6, 12, LAP_UI_DIM, "%u min", ROOM_MIN[cfg.room_i]);
    put(ax + 14, y + 6, 14, LAP_UI_DIM, "%u min", CAR_MIN[cfg.car_i]);
    int yy = y + 8;
    if (yy < y + hgt) yy += wrap(x + 1, yy, w - 2, 3, LAP_UI_TEXT, "Listens on the cellular uplink bands trackers use, ISM 315/433/915, the UHF bug band, Bluetooth tags and Wi-Fi cameras. Receive only.");
    yy++;
    if (yy < y + hgt) yy += wrap(x + 1, yy, w - 2, 2, A(blink(900) ? LAP_UI_WARN : LAP_YELLOW), "First put YOUR phone in airplane mode: it transmits in the same bands.");
    yy++;
    if (yy < y + hgt) yy += wrap(x + 1, yy, w - 2, 3, LAP_UI_DIM, "ROOM: walk it slowly, past vents, lamps and chargers. CAR: sit or drive; trackers check in every 1-10 min.");
    if (radio < 0 && yy + 1 < y + hgt) { yy += 1 + wrap(x + 1, yy + 1, w - 2, 2, A(LAP_UI_WARN), "No LoRa radio with RSSI here: the sweep uses Bluetooth and Wi-Fi only."); }
    yy += 2;
    if (h && yy + h->nband + 3 < y + hgt) plan_art(x + 1, yy, w - 2, y + hgt - yy);
}

static void timeline(int x, int y, int w, int rows_max)
{
    const int lw = 4, pk = 5;
    const int tw = w - lw - pk - 1;
    if (tw < 8) return;
    const int ref = ref_or(-1100);
    const int now_slot = bh_slot_now(h, state == ST_DONE ? ended_ms : now_ms);
    const int nb = h->nband < rows_max ? h->nband : rows_max;
    for (int b = 0; b < nb; b++) {
        const bh_band_t *bd = &h->band[b];
        const int yy = y + b;
        const bool live = state == ST_SWEEP && live_hz && bh_step_of(h, live_hz) >= bd->first && bh_step_of(h, live_hz) < bd->first + bd->count;
        put(x, yy, lw, live ? A(LAP_WHITE | LAP_BRIGHT) : bd->failed ? LAP_UI_FAINT : A(LAP_CYAN | LAP_BRIGHT), "%-4s", bd->name);
        /* a steady level: magenta outside the cell bands (a carrier), dim
           in them (the towers' downlink next door) */
        bool steady = false;
        for (int s2 = bd->first; s2 < bd->first + bd->count; s2++) steady |= bh_step_carrier(h, s2);
        int carry = BH_NONE, carry_age = 0;
        for (int c = 0; c < tw; c++) {
            const int s0 = c * BH_SLOTS / tw, s1 = (c + 1) * BH_SLOTS / tw > s0 ? (c + 1) * BH_SLOTS / tw : s0 + 1;
            if (s0 > now_slot) { cell(x + lw + c, yy, (c % 4) ? ' ' : LAP_BRAILLE(0x04), LAP_UI_FAINT); continue; }
            int v = BH_NONE, ex = BH_NONE;
            for (int s = s0; s < s1 && s < BH_SLOTS; s++) {
                if (h->slot[b][s] > v) v = h->slot[b][s];
                if (h->hslot[b][s] > ex) ex = h->hslot[b][s];
            }
            const int col_ms = (int)(h->slot_ms * (uint32_t)(s1 - s0));
            if (v != BH_NONE) { carry = v; carry_age = 0; }
            else if (carry != BH_NONE && (carry_age += col_ms) < (int)h->sweep_ms * 3 / 2) v = carry;
            if (v == BH_NONE) { cell(x + lw + c, yy, bd->failed ? 'x' : LAP_BRAILLE(0x40), LAP_UI_FAINT); continue; }
            const bool fresh = carry_age == 0;
            if (fresh && ex >= 0) {                     /* a burst: how far over its step's own quiet */
                const int db = ex / 10;
                cell(x + lw + c, yy, db >= 15 ? LAP_BLOCK : db >= 5 ? LAP_SHADE_75 : LAP_SHADE_50,
                     A(db >= 15 ? (LAP_RED | LAP_BRIGHT) : db >= 5 ? (LAP_YELLOW | LAP_BRIGHT) : (LAP_GREEN | LAP_BRIGHT)));
            } else if (steady && v >= ref + 120)
                cell(x + lw + c, yy, bd->kind == BH_BAND_CELL ? LAP_SHADE_25 : LAP_SHADE_75,
                     A(bd->kind == BH_BAND_CELL ? LAP_BLUE : (uint8_t)(fresh ? (LAP_MAGENTA | LAP_BRIGHT) : LAP_MAGENTA)));
            else cell(x + lw + c, yy, fresh ? LAP_BRAILLE(0x24) : LAP_BRAILLE(0x04), A(LAP_BLUE));
        }
        /* the sweep head */
        if (state == ST_SWEEP) {
            const int hc = now_slot * tw / BH_SLOTS;
            if (hc < tw) cell(x + lw + hc, yy, live && blink(150) ? LAP_BLOCK : '|', A(live ? (LAP_WHITE | LAP_BRIGHT) : LAP_WHITE));
        }
        if (bd->peak != BH_NONE) put(x + lw + tw + 1, yy, pk, A(level_hue((bd->peak - ref) / 10)), "%4d", bd->peak / 10);
        else put(x + lw + tw + 1, yy, pk, LAP_UI_FAINT, "  --");
    }
}

/* Max-hold per step as braille bars, the floor under them, the step being
   listened to lit; band names under their first step. */
static void spectrum(int x, int y, int w, int rows)
{
    if (rows < 2 || w < 10) return;
    const int ref = ref_or(-1100), quiet = qtop();
    cv_begin(x, y, w, rows);
    const int dots_w = w * 2, dots_h = rows * 4;
    int qh = (quiet - ref + 30) * dots_h / 450;
    qh = qh < 0 ? 0 : qh >= dots_h ? dots_h - 1 : qh;
    const int live = live_hz ? bh_step_of(h, live_hz) : -1;
    for (int dx = 0; dx < dots_w; dx++) {
        const int s0 = dx * h->nstep / dots_w, s1 = (dx + 1) * h->nstep / dots_w > s0 ? (dx + 1) * h->nstep / dots_w : s0 + 1;
        int pkv = BH_NONE, fl = BH_NONE, thr = 32767;
        bool is_live = false, car = false;
        for (int s = s0; s < s1 && s < h->nstep; s++) {
            if (h->step[s].peak > pkv) { pkv = h->step[s].peak; thr = bh_hot_threshold(h, s); }
            if (h->step[s].floor != BH_NONE && (fl == BH_NONE || h->step[s].floor > fl)) fl = h->step[s].floor;
            is_live |= s == live;
            /* steady in a cell band is the towers' downlink: not a carrier to show */
            car |= bh_step_carrier(h, s) && h->band[h->step[s].band].kind != BH_BAND_CELL;
        }
        if (!(dx % 3)) cv_dot(dx, dots_h - 1 - qh, A(LAP_BLUE));                  /* the top of the noise */
        if (pkv == BH_NONE) { cv_dot(dx, dots_h - 1, A(LAP_BLUE)); continue; }
        const int over = (pkv - ref) / 10;
        int hgt = (pkv - ref + 30) * dots_h / 450;
        hgt = hgt < 1 ? 1 : hgt > dots_h ? dots_h : hgt;
        const uint8_t at = is_live && blink(120) ? A(LAP_WHITE | LAP_BRIGHT)
                         : A(car ? (LAP_MAGENTA | LAP_BRIGHT) : pkv < (thr != 32767 ? thr : quiet) ? LAP_BLUE : level_hue(over));
        for (int k = 0; k < hgt; k++) cv_dot(dx, dots_h - 1 - k, at);
        (void)fl;
    }
    cv_flush();
}
static void band_ruler(int x, int y, int w)
{
    int last = -10;
    for (int b = 0; b < h->nband; b++) {
        const int c = h->band[b].first * w * 2 / (h->nstep ? h->nstep : 1) / 2;
        if (c < last + 1) continue;
        cell(x + c, y, LAP_BRAILLE(0x47), LAP_UI_FAINT);
        last = c + put(x + c + 1, y, w - c - 1, LAP_UI_DIM, "%s", h->band[b].name);
    }
}

static void draw_sweep(int x, int y, int w, int hgt)
{
    if (state == ST_IDLE || !h) { idle_art(x, y + 1, w, hgt - 1); return; }
    int yy = y;
    put(x, yy, w, LAP_UI_FAINT, "BAND%*s%s", w - 9 - 4, state == ST_SWEEP ? "time >" : "", " PEAK");
    yy++;
    const int tl_rows = h->nband;
    timeline(x, yy, w, tl_rows);
    yy += tl_rows + 1;
    const int sp_rows = hgt - (yy - y) - (wide ? 5 : 6);
    if (sp_rows >= 2) {
        if (h->ref == BH_NONE) put(x, yy, w, LAP_UI_FAINT, "MAX-HOLD PER STEP  learning the floor");
        else put(x, yy, w, LAP_UI_FAINT, "%s  floor %d  top %d  %.0f/s", scan_ver == 2 ? "PEAK OF EACH DWELL" : "MAX-HOLD PER STEP",
                 h->ref / 10, qtop() / 10, (double)step_rate);
        yy++;
        const int r = sp_rows - 1 > 6 ? 6 : sp_rows - 1;
        spectrum(x, yy, w, r);
        yy += r;
        band_ruler(x, yy, w);
        yy += 2;
    }
    /* BT and Wi-Fi in one line each */
    int tags = 0;
    for (int i = 0; i < h->ble.n; i++) tags += !h->ble.dev[i].succ && h->ble.dev[i].ad.level >= 1;
    const bool bt_on = ble_on();
    put(x, yy, w, bt_on ? LAP_UI_DIM : LAP_UI_FAINT, "BT    %s%u adverts  %d tracker tag%s", bt_on ? "" : "off  ", (unsigned)h->ble_adverts, tags, tags == 1 ? "" : "s");
    if (tags) put(x + w - 4, yy, 4, A(blink(500) ? (LAP_RED | LAP_BRIGHT) : LAP_RED), "TAG!");
    yy++;
    int cams = 0, streams = 0;
    for (int i = 0; i < h->nsta; i++) cams += h->sta[i].vendor && bh_cam_vendor(h->sta[i].mac) != NULL;
    for (int i = 0; i < nf; i++) streams += F[i].kind == BH_F_WIFI_STREAM;
    if (wifi_h > 0)
        put(x, yy, w, LAP_UI_DIM, "WIFI  %u frames  %d sta  %d cam  %d stream", (unsigned)h->wifi_frames, h->nsta, cams, streams);
    else
        put(x, yy, w, LAP_UI_FAINT, "WIFI  off: %s", wifi_why[0] ? wifi_why : "no capture");
    yy++;
    /* the best verdicts so far, while there is room */
    if (nf && yy + 3 < y + hgt - 2) {
        yy++;
        put(x, yy++, w, LAP_UI_FAINT, "%s", state == ST_SWEEP ? "BEST SO FAR" : "VERDICTS");
        for (int i = 0; i < nf && i < 4 && yy < y + hgt - 3; i++, yy++) {
            const uint8_t hue = (uint8_t)(F[i].conf >= 70 ? (LAP_RED | LAP_BRIGHT) : F[i].conf >= 50 ? (LAP_YELLOW | LAP_BRIGHT) : F[i].conf >= 30 ? (LAP_GREEN | LAP_BRIGHT) : LAP_CYAN);
            put(x, yy, 4, A(hue), "%3d%%", F[i].conf);
            put(x + 5, yy, 5, A(hue), "%s", kind_icon(F[i].kind));
            put(x + 11, yy, w - 11, i == 0 ? LAP_UI_TEXT : LAP_UI_DIM, "%s", F[i].title);
        }
        yy++;
    }
    if (yy < y + hgt) {
        static const char *const tips[] = {
            "Your phone transmits in these bands: airplane mode it.",
            "Bright cells are bursts over each step's own quiet level.",
            "Magenta bars hold a steady carrier: an audio bug does.",
            "Walk slowly. Bugs are loud within a metre.",
        };
        const char *tip = state == ST_DONE ? "Done. FINDINGS has the verdicts; HUNT one to find it." : tips[(now_ms / 5000u) % 4u];
        wrap(x, yy, w, y + hgt - yy, A(state == ST_DONE ? LAP_UI_LIVE : LAP_YELLOW), tip);
    }
}

/* ---- FINDINGS ---- */
static const char *kind_icon(int k)
{
    switch (k) {
    case BH_F_CELL_PERIODIC: return "((o))";
    case BH_F_BLE_FOLLOW: case BH_F_BLE_TAG: return "[tag]";
    case BH_F_WIFI_STREAM: case BH_F_WIFI_CAM: return "[cam]";
    case BH_F_CARRIER: return "~~~~~";
    case BH_F_STRONG: return "!!!!!";
    case BH_F_PHONE: return "[ph?]";
    default: return "(( ))";
    }
}
static uint8_t conf_hue(int c) { return c >= 70 ? (LAP_RED | LAP_BRIGHT) : c >= 50 ? (LAP_YELLOW | LAP_BRIGHT) : c >= 30 ? (LAP_GREEN | LAP_BRIGHT) : LAP_CYAN; }
static int card_rows(const bh_find_t *f, int w) { return 2 + wrap_rows(w - 2, wide ? 2 : 3, f->detail); }
static void target_text(const bh_find_t *f, char *out, size_t cap)
{
    char t[24];
    if (f->target == BH_T_FREQ) { mhz(f->khz, t, sizeof(t)); snprintf(out, cap, "%s MHz", t); }
    else if (f->target == BH_T_BLE) {
        const bw_dev_t *d = f->uid && f == &tgt ? bhw_dev(f->uid) : NULL;
        bw_addr_text(d ? d->addr : f->addr, t);
        snprintf(out, cap, "BT %s", t);
    }
    else { snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x", f->addr[0], f->addr[1], f->addr[2], f->addr[3], f->addr[4], f->addr[5]); }
}
static void card(const bh_find_t *f, int idx, int x, int y, int w)
{
    const bool on = idx == sel;
    const int rows = card_rows(f, w);
    const uint8_t hue = conf_hue(f->conf);
    /* a bar down the left: brighter for the chosen one */
    for (int r = 0; r < rows - 1; r++) cell(x, y + r, on ? LAP_BLOCK : LAP_SHADE_50, A(on ? hue : (uint8_t)(hue & 7)));
    put(x + 2, y, 5, A(hue), "%s", kind_icon(f->kind));
    put(x + 8, y, w - 8 - 5, on ? LAP_UI_TEXT : A(LAP_WHITE), "%s", f->title);
    put(x + w - 4, y, 4, A(hue), "%3d%%", f->conf);
    wrap(x + 2, y + 1, w - 2, wide ? 2 : 3, LAP_UI_DIM, f->detail);
    /* confidence pips and the target */
    char tt[40];
    target_text(f, tt, sizeof(tt));
    const int pips = (f->conf + 19) / 20;
    for (int p = 0; p < 5; p++) cell(x + 2 + p, y + rows - 1, p < pips ? LAP_BLOCK : LAP_SHADE_25, A(p < pips ? hue : LAP_BLUE));
    put(x + 8, y + rows - 1, w - 8, on ? A(LAP_UI_VALUE) : LAP_UI_FAINT, "%s%s", tt, on ? "  > HUNT" : "");
    if (hit_n < BH_MAX_FIND) hits[hit_n++] = (hit_t){ (int16_t)y, (int16_t)(y + rows), (int16_t)idx };
}
static void empty_art(int x, int y, int w, int hgt)
{
    const int cx = x + w / 2 - 6;
    const int yy = y + hgt / 2 - 5;
    clip0 = y; clip1 = y + hgt;
    emblem(cx, yy, false);
    const char *msg = state == ST_IDLE ? "No sweep yet." : state == ST_SWEEP ? "Listening..." : "Nothing stands out.";
    put(x + (w - (int)strlen(msg)) / 2, yy + 9, w, LAP_UI_TEXT, "%s", msg);
    const char *sub = state == ST_IDLE ? "SWEEP ROOM or SWEEP CAR to start." : state == ST_SWEEP ? "Verdicts appear here as they firm up." : "Sweep longer, or move and sweep again.";
    put(x + (w - (int)strlen(sub)) / 2, yy + 10, w, LAP_UI_DIM, "%s", sub);
    static const char dots[] = ".oOo";
    if (state == ST_SWEEP) cell(x + w / 2, yy + 11, dots[(now_ms / 200u) % 4u], A(LAP_GREEN | LAP_BRIGHT));
}
static bool sel_moved;                  /* the keys moved the choice: bring it into view */
static void draw_find(int x, int y, int w, int hgt)
{
    if (!nf) { empty_art(x, y, w, hgt); return; }
    int total = 0, at = 0;
    for (int i = 0; i < nf; i++) { if (i == sel) at = total; total += card_rows(&F[i], w) + 1; }
    smax = total - hgt > 0 ? total - hgt : 0;
    if (sel_moved && !dragging) {
        const int r = card_rows(&F[sel], w);
        if (at < scroll) scroll = at;
        else if (at + r > scroll + hgt) scroll = at + r - hgt;
        sel_moved = false;
    }
    if (scroll > smax) scroll = smax;
    int yy = y - scroll;
    for (int i = 0; i < nf; i++) {
        const int r = card_rows(&F[i], w);
        if (yy + r >= y && yy < y + hgt) card(&F[i], i, x, yy, w);
        yy += r + 1;
    }
}

/* ---- HUNT ---- */
static void dir_plot(int x, int y, int w, int hgt)
{
    cv_begin(x, y, w, hgt);
    const float cx = (float)cv.w - 0.5f, cy = (float)cv.h * 2.0f - 0.5f;
    float R = cx - 1.0f;
    if (R > (cy - 1.0f) * cv.ax) R = (cy - 1.0f) * cv.ax;
    if (R < 4) return;
    cv_ring(cx, cy, R, 0, A(LAP_GREEN));
    cv_ring(cx, cy, R * 0.5f, 3, A(LAP_BLUE));
    /* level per heading: a petal out from the middle, strongest longest */
    const int lo = dir.peak == BH_NONE ? hunt_floor : (dir.peak - 300 > hunt_floor ? dir.peak - 300 : hunt_floor);
    for (int b = 0; b < BH_DIRS; b++) {
        if (dir.best[b] == BH_NONE) continue;
        float f = (float)(dir.best[b] - lo) / (float)((dir.peak > lo ? dir.peak : lo + 1) - lo);
        f = f < 0.08f ? 0.08f : f > 1 ? 1 : f;
        const float a = (float)b * 6.2831853f / BH_DIRS - 1.5707963f;      /* 0 = north, up */
        const int below = (dir.peak - dir.best[b]) / 10;
        const uint8_t at = b == dir.peak_bin ? A(LAP_RED | LAP_BRIGHT)
                         : A(below <= 3 ? LAP_RED : below <= 8 ? (LAP_YELLOW | LAP_BRIGHT) : below <= 15 ? LAP_GREEN : LAP_BLUE);
        cv_line(rnd(cx), rnd(cy), rnd(cx + R * f * cosf(a)), rnd(cy + R * f * sinf(a) / cv.ax), at);
    }
    /* where the board points now */
    if (heading >= 0) {
        const float a = heading * 0.0174533f - 1.5707963f;
        for (float r = R * 0.2f; r <= R + 1.5f; r += 0.7f)
            cv_dot(rnd(cx + r * cosf(a)), rnd(cy + r * sinf(a) / cv.ax), A(LAP_WHITE | LAP_BRIGHT));
    }
    cv_flush();
    cell(x + cv.w - 1, y, 'N', A(LAP_CYAN | LAP_BRIGHT));
    if (dir.peak_bin >= 0) {
        const float a = (float)dir.peak_bin * 6.2831853f / BH_DIRS - 1.5707963f;
        const int mx = x + (int)((cx + (R + 2) * cosf(a)) / 2.0f), my = y + (int)((cy + (R + 2) * sinf(a) / cv.ax) / 4.0f);
        if (blink(300)) cell(mx < x ? x : mx, my < y ? y : my, '*', A(LAP_RED | LAP_BRIGHT));
    }
}
static const char *compass_word(int deg)
{
    static const char *const n[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    return n[((deg + 22) / 45) % 8];
}
static void draw_hunt(int x, int y, int w, int hgt)
{
    if (!hunting) {
        clip0 = y; clip1 = y + hgt;
        emblem(x + w / 2 - 6, y + 2, false);
        put(x + 1, y + 11, w - 2, LAP_UI_TEXT, "Pick a finding or a tag, then HUNT.");
        wrap(x + 1, y + 13, w - 2, 4, LAP_UI_DIM, "The radio parks on it: frequency, Bluetooth address (followed through its changes) or Wi-Fi station. The bar, the ticks or the tone rise as you get closer.");
        return;
    }
    /* landscape: the direction plot takes the right side */
    const int full_w = w;
    int plot_x = x, plot_y = 0, plot_w = 0, plot_h = 0;
    if (wide && w >= 60) {
        plot_w = 30;
        plot_h = hgt;
        plot_x = x + w - plot_w;
        plot_y = y;
        w -= plot_w + 2;
    }
    int yy = y;
    put(x, yy, w, LAP_UI_TEXT, "%s", tgt.title);
    yy++;
    char tt[40];
    target_text(&tgt, tt, sizeof(tt));
    put(x, yy, w, A(LAP_UI_VALUE), "%s", tt);
    if (tgt.target == BH_T_FREQ && radio < 0) put(x + 22, yy, w - 22, A(LAP_UI_ALARM), "no LoRa radio");
    const bw_dev_t *tag = tgt.target == BH_T_BLE && tgt.uid ? bhw_dev(tgt.uid) : NULL;
    if (tag) {
        /* what the tag watch knows: its address changes, its owner, how long it has been about */
        char ex[48];
        int n = 0;
        if (tag->rotations) n += snprintf(ex + n, sizeof(ex) - (size_t)n, "rot %u ", tag->rotations);
        if (tag->ad.separated == 1) n += snprintf(ex + n, sizeof(ex) - (size_t)n, "AWAY ");
        if (tag->ad.separated == 0) n += snprintf(ex + n, sizeof(ex) - (size_t)n, "owner near ");
        snprintf(ex + n, sizeof(ex) - (size_t)n, "here %um", (unsigned)(bw_span_ms(tag) / 60000u));
        const int tl = (int)strlen(tt) + 2;
        put(x + tl, yy, w - tl, A(tag->alert ? (LAP_RED | LAP_BRIGHT) : (LAP_MAGENTA | LAP_BRIGHT)), "%s", ex);
    }
    yy += 2;
    /* the big bar, with its scale under it: cold on the left, hot on the right */
    const bool fresh = lv_ms && now_ms - lv_ms < fresh_ms();
    const float frac = fresh ? hunt_frac(lv_fast) : 0;
    const float pk = lv_peak == BH_NONE ? -1 : hunt_frac((float)lv_peak);
    lap_ui_bargraph(x, yy, w, 3, frac, pk);
    {
        const int lo = hunt_floor / 10, hi = lo + hunt_span() / 10, steps = hunt_span() / 100;
        for (int i = 1; i < steps; i++) cell(x + w * i / steps, yy + 3, '\'', LAP_UI_FAINT);   /* every 10 dB */
        char l[16], r[16];
        snprintf(l, sizeof(l), "%d cold", lo);
        snprintf(r, sizeof(r), "hot %d", hi);
        put(x, yy + 3, (int)strlen(l), A(LAP_BLUE | LAP_BRIGHT), "%s", l);
        put(x + w - (int)strlen(r), yy + 3, (int)strlen(r), A(LAP_RED | LAP_BRIGHT), "%s", r);
    }
    yy += 5;
    /* hotter / colder and the Geiger click */
    const float d = lv_fast - lv_slow;
    const bool hot = fresh && d > 25, cold = fresh && d < -25;
    const char *word = !fresh ? (lv_ms ? "LOST IT" : "LISTEN") : hot ? "HOTTER" : cold ? "COLDER" : "STEADY";
    const uint8_t wa = A(!fresh ? LAP_BLUE : hot ? (LAP_RED | LAP_BRIGHT) : cold ? (LAP_CYAN | LAP_BRIGHT) : LAP_WHITE);
    const int bw2 = big_text(x, yy, w - 8, word, wa);
    if (hot || cold) {
        const int ph2 = (int)((now_ms / 150u) % 3u);
        for (int k = 0; k < 3; k++)
            cell(x + bw2 + 1 + k, yy + (hot ? 4 - ph2 : ph2), hot ? '^' : 'v', A(k == ph2 ? (LAP_WHITE | LAP_BRIGHT) : (hot ? LAP_RED : LAP_CYAN)));
    }
    /* the speaker throws a ring per tick */
    const uint32_t since = tick_ms ? now_ms - tick_ms : 9999u;
    const int ring = since < 80u ? 3 : since < 160u ? 2 : since < 240u ? 1 : 0;
    put(x + w - 6, yy + 1, 2, A(LAP_WHITE | LAP_BRIGHT), "<|");
    for (int k = 0; k < 3; k++) cell(x + w - 4 + k, yy + 1, ')', A(k < ring ? (LAP_YELLOW | LAP_BRIGHT) : LAP_BLUE));
    if (!cfg.sound) put(x + w - 6, yy + 2, 6, LAP_UI_FAINT, "muted");
    else if (hunt_tone()) put(x + w - 6, yy + 2, 6, LAP_UI_FAINT, "%4dHz", 300 + (int)(hunt_frac(lv_fast) * 1700.0f));
    else if (fresh) put(x + w - 6, yy + 2, 6, LAP_UI_FAINT, "%4u/s", (unsigned)(1000u / bh_geiger_ms((int)lv_fast, hunt_floor)));
    yy += 6;
    /* history: a tag's last two minutes from the tag watch (it was listening
       before HUNT began); else the readings since HUNT began */
    if (tag) {
        int cells = w - 5 - 6;
        if (cells > BW_HIST / 2) cells = BW_HIST / 2;
        put(x, yy, 5, LAP_UI_FAINT, "LAST");
        put(x, yy + 1, 5, LAP_UI_FAINT, "%ds", cells * 2 * BW_HIST_MS / 1000);
        bhw_graph(x + 5, yy, cells, 3, tag);
        if (lv_peak != BH_NONE) put(x + w - 5, yy, 5, A(LAP_RED | LAP_BRIGHT), "%4d", lv_peak / 10);
        put(x + w - 5, yy + 1, 5, LAP_UI_FAINT, " peak");
        yy += 4;
    } else if (hhist_n > 1) {
        put(x, yy, 5, LAP_UI_FAINT, "LAST");
        lap_ui_history(x + 5, yy, w - 5 - 6, 2, hhist, hhist_n, (float)hunt_floor / 10.0f, (float)hunt_floor / 10.0f + 60.0f, LAP_GREEN | LAP_BRIGHT);
        if (lv_peak != BH_NONE) put(x + w - 5, yy, 5, A(LAP_RED | LAP_BRIGHT), "%4d", lv_peak / 10);
        put(x + w - 5, yy + 1, 5, LAP_UI_FAINT, " peak");
        yy += 3;
    }
    /* the turn-in-a-circle plot: under it in portrait, beside it in landscape */
    if (!plot_w) {
        const int left = y + hgt - yy;
        if (left < 6) return;
        plot_y = yy;
        plot_h = left;
        plot_w = full_w;
    }
    const int pr0 = plot_w == full_w ? plot_h - 1 : plot_h - 6;
    const int pr = pr0 > 20 ? 20 : pr0;
    int pwid = plot_w == full_w ? pr * 2 + 2 : plot_w;
    if (plot_w == full_w && pwid > full_w - 19) pwid = full_w - 19;
    if (!have_compass || pwid < 8 || pr < 4) {
        wrap(plot_x, plot_y, plot_w, plot_h, LAP_UI_DIM, "No compass: walk and watch the bar. Your body blocks signal, so turn slowly where you stand first.");
        return;
    }
    dir_plot(plot_x, plot_y, pwid, pr);
    const int cov = bh_dir_covered(&dir);
    int tx = plot_x + pwid + 2, ty2 = plot_y, tw = full_w - (tx - x);
    if (plot_w != full_w) { tx = plot_x; ty2 = plot_y + pr; tw = plot_w; }
    put(tx, ty2, tw, LAP_UI_FAINT, "DIRECTION");
    if (heading >= 0) put(tx, ty2 + 1, tw, LAP_UI_TEXT, "facing %3d %s", (int)heading, compass_word((int)heading));
    else put(tx, ty2 + 1, tw, LAP_UI_FAINT, "compass settling");
    if (dir.peak_bin >= 0) put(tx, ty2 + 2, tw, A(LAP_RED | LAP_BRIGHT), "peak   %3d %s", dir.peak_bin * 360 / BH_DIRS, compass_word(dir.peak_bin * 360 / BH_DIRS));
    put(tx, ty2 + 3, tw, LAP_UI_DIM, "%d%% round%s", cov * 100 / BH_DIRS, cov >= BH_DIRS * 2 / 3 ? "" : "...");
    if (plot_w == full_w)
        wrap(tx, ty2 + 5, tw, pr - 5 > 1 ? pr - 5 : 1, LAP_UI_DIM,
             cov < BH_DIRS * 2 / 3 ? "Hold it flat, turn slowly all the way round, then walk toward the red peak." : "Walk toward the peak and turn again: the peak sharpens as you close in.");
    else
        put(tx, ty2 + 4, tw, LAP_UI_DIM, "%s", cov < BH_DIRS * 2 / 3 ? "turn slowly all the way round" : "walk to the peak, turn again");
}

/* ---- keys ---- */
static void set_keys(void)
{
    static char v0[24], v1[24], v2[24];
    switch (tab) {
    case TAB_SWEEP:
        if (state == ST_SWEEP) {
            const uint32_t left = h->dur_ms > now_ms - h->t0_ms ? (h->dur_ms - (now_ms - h->t0_ms)) / 1000u : 0;
            snprintf(v0, sizeof(v0), "%u:%02u LEFT", (unsigned)(left / 60), (unsigned)(left % 60));
            keys[0] = (lap_btn_t){ "STOP", v0, 's', true, false, false };
            snprintf(v1, sizeof(v1), "%d", nf);
            keys[1] = (lap_btn_t){ "FINDINGS", v1, 'f', false, false, nf > 0 };
            keys[2] = (lap_btn_t){ "SOUND", cfg.sound ? "ON" : "OFF", 't', cfg.sound != 0, false, false };
        } else {
            snprintf(v0, sizeof(v0), "%u MIN", ROOM_MIN[cfg.room_i]);
            snprintf(v1, sizeof(v1), "%u MIN", CAR_MIN[cfg.car_i]);
            keys[0] = (lap_btn_t){ "SWEEP ROOM", v0, 'r', false, false, false };
            keys[1] = (lap_btn_t){ "SWEEP CAR", v1, 'c', false, false, false };
            keys[2] = (lap_btn_t){ "REGION", REGION_SHORT[cfg.region], 'g', false, false, false };
        }
        break;
    case TAB_FIND:
        snprintf(v0, sizeof(v0), "%s", nf ? (F[sel].target == BH_T_FREQ ? "FREQUENCY" : F[sel].target == BH_T_BLE ? "BLUETOOTH" : "WI-FI") : "");
        keys[0] = (lap_btn_t){ "HUNT", nf ? v0 : NULL, 'h', false, !nf, false };
        snprintf(v1, sizeof(v1), "%d OF %d", nf ? sel + 1 : 0, nf);
        keys[1] = (lap_btn_t){ "NEXT", v1, 'n', false, nf < 2, false };
        keys[2] = state == ST_SWEEP ? (lap_btn_t){ "STOP", NULL, 's', true, false, false }
                                    : (lap_btn_t){ "SWEEP", h && h->mode == BH_CAR ? "CAR" : "ROOM", 'r', false, false, false };
        break;
    case TAB_TAGS:
        if (bhw_view() == BHW_INFO) {
            uint32_t fm = 0;
            bool pos = false, lg = false;
            bhw_get_config(&fm, &pos, &lg);
            snprintf(v0, sizeof(v0), "%u MIN", (unsigned)(fm / 60000u));
            keys[0] = (lap_btn_t){ "ALERT AFTER", v0, 'a', false, false, false };
            keys[2] = (lap_btn_t){ "POSSIBLE", pos ? "ALERT" : "LIST", 'p', pos, false, false };
        } else {
            keys[0] = (lap_btn_t){ "HUNT", bhw_pick() ? "THIS TAG" : NULL, 'h', false, !bhw_pick(), false };
            keys[2] = (lap_btn_t){ "LOG", bhw_log_on() ? "ON" : "OFF", 'g', bhw_log_on(), false, false };
        }
        keys[1] = (lap_btn_t){ "VIEW", bhw_view_name(bhw_view()), 'v', false, false, false };
        break;
    default:
        keys[0] = (lap_btn_t){ "SOUND", !cfg.sound ? "OFF" : hunt_tone() ? "TONE" : "TICKS", 't', cfg.sound != 0, false, false };
        snprintf(v2, sizeof(v2), "%d%%", bh_dir_covered(&dir) * 100 / BH_DIRS);
        keys[1] = (lap_btn_t){ "RESET", hunting ? v2 : NULL, 'd', false, !hunting, false };
        keys[2] = (lap_btn_t){ "STOP", "HUNT", 'x', hunting, !hunting, false };
        break;
    }
    keys[3] = (lap_btn_t){ "MORE", NULL, 'm', false, false, false };
}

/* ---- MORE ---- */
enum { M_REGION, M_ROOM, M_CAR, M_HOP, M_SOUND, M_TONE, M_FOLLOW, M_POSSIBLE, M_LOG, M_CLEAR, M_HELP, M_N };
static char mlabel[M_N][32];
static void more(void)
{
    if (dlg) return;
    uint32_t fm = 0;
    bool pos = false, lg = false;
    bhw_get_config(&fm, &pos, &lg);
    snprintf(mlabel[M_REGION], 32, "REGION: %s", REGION_NAMES[cfg.region]);
    snprintf(mlabel[M_ROOM], 32, "ROOM SWEEP: %u MIN", ROOM_MIN[cfg.room_i]);
    snprintf(mlabel[M_CAR], 32, "CAR SWEEP: %u MIN", CAR_MIN[cfg.car_i]);
    snprintf(mlabel[M_HOP], 32, "WI-FI HOP, DROP LINK: %s", cfg.hop ? "YES" : "NO");
    snprintf(mlabel[M_SOUND], 32, "SOUND: %s", cfg.sound ? "ON" : "OFF");
    snprintf(mlabel[M_TONE], 32, "HUNT SOUND: %s", TONE_NAMES[cfg.tone]);
    snprintf(mlabel[M_FOLLOW], 32, "FOLLOWING ALERT: %u MIN", (unsigned)(fm / 60000u));
    snprintf(mlabel[M_POSSIBLE], 32, "POSSIBLE TAGS: %s", pos ? "ALERT" : "LIST ONLY");
    snprintf(mlabel[M_LOG], 32, "TAG LOG TO CARD: %s", bhw_log_on() ? "ON" : "OFF");
    snprintf(mlabel[M_CLEAR], 32, "CLEAR THE TAG LIST");
    snprintf(mlabel[M_HELP], 32, "HOW IT WORKS");
    const char *l[M_N];
    for (int i = 0; i < M_N; i++) l[i] = mlabel[i];
    dlg = lap_choose(l, M_N);
    dlg_kind = 1;
}
static void dialogs(void)
{
    if (dlg <= 0) { dlg = 0; return; }
    char none[4];
    int v = -1;
    const int r = lap_dialog_poll(dlg, none, sizeof(none), &v);
    if (!r) return;
    const int kind = dlg_kind;
    dlg = 0;
    if (r != 1) return;
    if (kind == 2) {                        /* the link-drop question */
        cfg.hop = v == 1;
        cfg_save();
        if (wifi_h > 0) { lap_capture_stop(wifi_h); wifi_h = 0; }
        sources_stop();
        sources_start();
        say(cfg.hop ? "Wi-Fi hops 1-13 (link dropped)" : "Wi-Fi stays on the link's channel");
        return;
    }
    if (kind == 3) return;                  /* help: nothing to do */
    switch (v) {
    case M_REGION:
        cfg.region = (uint8_t)(cfg.region % 3 + 1);
        if (state == ST_IDLE) bh_init(h, BH_ROOM, cfg.region, has24, 240000, now_ms);
        say("Region %s: next sweep", REGION_NAMES[cfg.region]);
        break;
    case M_ROOM: cfg.room_i = (uint8_t)((cfg.room_i + 1) % 3); say("Room sweep %u min", ROOM_MIN[cfg.room_i]); break;
    case M_CAR: cfg.car_i = (uint8_t)((cfg.car_i + 1) % 3); say("Car sweep %u min", CAR_MIN[cfg.car_i]); break;
    case M_HOP:
        if (cfg.hop) { cfg.hop = 0; cfg_save(); sources_stop(); sources_start(); say("Wi-Fi stays on the link's channel"); }
        else { dlg = lap_confirm("Hopping Wi-Fi channels drops the board's Wi-Fi link (LINK, remote screens, network apps) while BUGHUNT runs. Allow it?"); dlg_kind = 2; }
        break;
    case M_SOUND: cfg.sound = !cfg.sound; if (!cfg.sound) lap_tone_stop(); break;
    case M_TONE:
        cfg.tone = (uint8_t)((cfg.tone + 1) % 3);
        lap_tone_stop();
        say("HUNT sound: %s", cfg.tone == TONE_AUTO ? "tone on a tag, ticks on a frequency" : cfg.tone == TONE_TICKS ? "ticks" : "a rising tone");
        break;
    case M_FOLLOW: {
        uint32_t fm = 0;
        bool pos = false, lg = false;
        bhw_get_config(&fm, &pos, &lg);
        if (fm >= 60u * 60000u) bhw_follow_step(-7); else bhw_follow_step(1);   /* up, then round to 2 min */
        break;
    }
    case M_POSSIBLE: bhw_possible_toggle(); break;
    case M_LOG: bhw_log_toggle(); break;
    case M_CLEAR: bhw_clear(); alert_uid = 0; break;
    case M_HELP:
        dlg = lap_confirm("Sweeps tracker uplink and ISM bands, names Bluetooth tags and warns when one follows you, watches Wi-Fi for cameras. HUNT finds one. Receive only.");
        dlg_kind = 3;
        break;
    }
    cfg_save();
    cfg_sync();
}

/* ---- callbacks ---- */
static bool watch_ok;
static void enter(void)
{
    cfg_dirty = false; cfg_job = -1; cfg_at = 0;
    cfg_load();
    cfg.hop = 0;                             /* dropping the link is asked for each time */
    h = (bh_t *)calloc(1, sizeof(bh_t));
    buf = (uint8_t *)malloc(BUF_CAP);
    watch_ok = bhw_open();
    bhw_config((uint32_t)cfg.follow_min * 60000u, cfg.possible != 0, cfg.log != 0);
    find_radio();
    state = ST_IDLE;
    tab = TAB_SWEEP;
    hunting = false;
    hunt_from = TAB_FIND;
    alert_uid = 0;
    rf_h = ble_cap = ble_scan = wifi_h = 0;
    nf = sel = 0;
    dlg = 0;
    bh_dir_reset(&dir);
    if (h) bh_init(h, BH_ROOM, cfg.region, has24, 240000, lap_millis());
    sources_start();                         /* BT and Wi-Fi run from the start: the tag watch listens from now */
}
static void leave(void)
{
    if (cfg_dirty && cfg_job < 0) lap_save("cfg", &cfg, sizeof(cfg));     /* a change made just before closing */
    rf_stop();
    sources_stop();
    lap_tone_stop();
    if (dlg > 0) lap_dialog_cancel(dlg);
    dlg = 0;
    bhw_close();
    free(h); free(buf);
    h = NULL; buf = NULL;
}

/* The tag watch says one has stayed with you: a board alert (banner, ring,
   vibrate), a haptic pulse, a red line over the title for 30 s (a tap
   there hunts it) and a line in the card log, as BLE WATCH did. */
static void following_alert(int i)
{
    const bw_t *t = bhw_table();
    if (!t || i < 0 || i >= t->n) return;
    bhw_alert_text(i, alert_text, sizeof(alert_text));
    lap_alert("Tracker following", alert_text, LAP_ALERT_WARN);
    lap_haptic(LAP_HAPTIC_ALERT);
    alert_uid = t->dev[i].uid;
    alert_at = lap_millis();
    say("%s", alert_text);
}
static bool alert_live(void) { return alert_uid && lap_millis() - alert_at < ALERT_SHOW_MS && bhw_dev(alert_uid); }

static void draw(void)
{
    now_ms = lap_millis();
    if (!h || !buf || !watch_ok) { lap_clear(LAP_ATTR(LAP_WHITE, LAP_BLACK)); lap_put_str(1, 1, "out of memory", LAP_ATTR(LAP_RED | LAP_BRIGHT, LAP_BLACK)); return; }
    if (radio < 0 && !(lap_frame() % 25)) find_radio();
    dialogs();
    cfg_pump();
    if (state == ST_SWEEP) {
        rf_poll_sweep();
        if (now_ms - rate_ms >= 2000u) {
            if (rate_ms) step_rate = (float)(steps_seen - rate_steps) * 1000.0f / (float)(now_ms - rate_ms);
            rate_ms = now_ms;
            rate_steps = steps_seen;
        }
        if (now_ms - h->t0_ms >= h->dur_ms) sweep_end();
    }
    sources_poll();
    {
        const int a = bhw_poll();           /* the tag watch: rotations, the following alert, the log */
        if (a >= 0) following_alert(a);
        char t[96];
        if (bhw_note(t, sizeof(t))) say("%s", t);
    }
    if (hunting) hunt_poll();
    static uint32_t tick_at;
    if (now_ms - tick_at >= 250u) { tick_at = now_ms; if (state == ST_SWEEP || hunting) bh_tick(h, now_ms); }
    if (state == ST_SWEEP && now_ms - find_ms >= 1000u) refresh_findings();
    /* awake while it works for you: a sweep, a hunt, or watching the tags */
    lap_keep_awake(state == ST_SWEEP || hunting || tab == TAB_TAGS);

    layout();
    const int sw2 = lap_swipe();
    if (sw2 == LAP_SWIPE_LEFT) to_tab(tab + 1);
    else if (sw2 == LAP_SWIPE_RIGHT) to_tab(tab - 1);
    clip0 = py + 1; clip1 = py + ph - 1;
    if (tab == TAB_TAGS) {
        lap_scroll_stop(&drag);
        dragging = bhw_follow(bhw_view() != BHW_INFO);
    } else {
        bhw_follow(false);
        smax = tab == TAB_FIND ? smax : 0;
        dragging = lap_scroll_follow(&drag, &scroll, smax, clip0, clip1);
    }
    hit_n = 0;

    lap_clear(LAP_ATTR(LAP_WHITE, LAP_BLACK));
    clip0 = 0; clip1 = H;
    title_bar();
    header();
    clip0 = 0; clip1 = H;
    if (wide) lap_vline(hw + 1, 1, H - kr - 1, ':', LAP_UI_FAINT);
    static const char *const TAB_NAME[TABS] = { "SWEEP", "FINDINGS", "TAGS", "HUNT" };
    int trackers = 0, alerts = 0, all = 0;
    bhw_counts(&trackers, &alerts, &all);
    char sv[24];
    if (tab != TAB_FIND && nf) snprintf(sv, sizeof(sv), "%d/%d  +%d", tab + 1, TABS, nf);
    else snprintf(sv, sizeof(sv), "%d/%d", tab + 1, TABS);
    strip(sx, ty, sw, tr, TAB_NAME[tab], sv, (tab != TAB_FIND && nf && F[0].conf >= 60) || (tab != TAB_TAGS && alerts));
    char bt[48];
    switch (tab) {
    case TAB_SWEEP:
        if (state == ST_IDLE) snprintf(bt, sizeof(bt), " CHOOSE A SWEEP ");
        else snprintf(bt, sizeof(bt), " %s  %s  %d steps ", h->mode == BH_CAR ? "CAR" : "ROOM", REGION_SHORT[h->region], h->nstep);
        break;
    case TAB_FIND: snprintf(bt, sizeof(bt), " VERDICTS  %d ", nf); break;
    case TAB_TAGS:
        if (bhw_view() == BHW_INFO) snprintf(bt, sizeof(bt), " TAG WATCH ");
        else snprintf(bt, sizeof(bt), " %s %d ", bhw_view() == BHW_ALL ? "ALL DEVICES" : "TRACKER TAGS", bhw_view() == BHW_ALL ? all : trackers);
        break;
    default: snprintf(bt, sizeof(bt), hunting ? " HUNT  closer = louder " : " HUNT "); break;
    }
    lap_ui_bezel(px, py, pw, ph, bt);
    const int ix = px + 1, iy = py + 1, iw = pw - 2, ih = ph - 2;
    clip0 = iy; clip1 = iy + ih;
    switch (tab) {
    case TAB_SWEEP: smax = 0; draw_sweep(ix, iy, iw, ih); break;
    case TAB_FIND: draw_find(ix, iy, iw, ih); break;
    case TAB_TAGS: smax = 0; bhw_draw(ix, iy, iw, ih, wide); break;
    default: smax = 0; draw_hunt(ix, iy, iw, ih); break;
    }
    clip0 = 0; clip1 = H;
    if (tab == TAB_FIND && smax > 0) {
        const int at = ih > 1 ? iy + (int)((long)scroll * (ih - 1) / smax) : iy;
        lap_put_cell(px + pw - 1, at, LAP_BLOCK, A(LAP_GREEN | LAP_BRIGHT));
    }
    set_keys();
    lap_ui_keys(0, H - kr, W, kr, keys, 4, -1);
}

static void act(char c)
{
    switch (c) {
    case 'r': if (state != ST_SWEEP) sweep_start(h && state == ST_DONE && tab == TAB_FIND ? h->mode : BH_ROOM); break;
    case 'c': if (state != ST_SWEEP) sweep_start(BH_CAR); break;
    case 's': sweep_end(); break;
    case 'f': to_tab(TAB_FIND); break;
    case 'g':
        if (tab == TAB_TAGS) { bhw_log_toggle(); break; }
        cfg.region = (uint8_t)(cfg.region % 3 + 1);
        cfg_save();
        if (state == ST_IDLE) bh_init(h, BH_ROOM, cfg.region, has24, 240000, now_ms);     /* the plan shown */
        say("Region %s", REGION_NAMES[cfg.region]);
        break;
    case 't': cfg.sound = !cfg.sound; if (!cfg.sound) lap_tone_stop(); cfg_save(); break;
    case 'h': if (tab == TAB_TAGS) hunt_tag(bhw_pick()); else hunt_start(sel); break;
    case 'n': if (nf) { sel = (sel + 1) % nf; sel_moved = true; } break;
    case 'd': bh_dir_reset(&dir); lv_peak = BH_NONE; break;
    case 'x': hunt_stop(); break;
    case 'm': more(); break;
    case 'v': bhw_view_set(bhw_view() + 1); break;
    case 'a': {                                        /* the alert's minutes: up a step, round to the shortest */
        uint32_t fm = 0;
        bool pos = false, lg = false;
        bhw_get_config(&fm, &pos, &lg);
        if (fm >= 60u * 60000u) bhw_follow_step(-7); else bhw_follow_step(1);
        break;
    }
    case 'p': bhw_possible_toggle(); break;
    default: break;
    }
    cfg_sync();
}
static bool key(lap_key_t k, char ch)
{
    if (k == LAP_KEY_LEFT) { to_tab(tab - 1); return true; }
    if (k == LAP_KEY_RIGHT || k == LAP_KEY_TAB) { to_tab(tab + 1); return true; }
    if (tab == TAB_TAGS) {
        const int r = bhw_key(k, ch);
        if (r == 2) { hunt_tag(bhw_pick()); return true; }
        if (r) { cfg_sync(); return true; }
        if (k == LAP_KEY_BACK && bhw_view() != BHW_TAGS) { bhw_view_set(BHW_TAGS); return true; }
    }
    if (tab == TAB_FIND && k == LAP_KEY_UP) { if (sel > 0) sel--; sel_moved = true; return true; }
    if (tab == TAB_FIND && k == LAP_KEY_DOWN) { if (sel + 1 < nf) sel++; sel_moved = true; return true; }
    if (tab == TAB_FIND && k == LAP_KEY_ENTER) { hunt_start(sel); return true; }
    if (tab == TAB_HUNT && hunting && (k == LAP_KEY_UP || k == LAP_KEY_DOWN)) { hunt_step(k == LAP_KEY_UP ? -1 : 1); return true; }
    if (k == LAP_KEY_BACK && hunting) { hunt_stop(); return true; }
    if (k == LAP_KEY_CHAR && (ch == 'l' || ch == 'L') && alert_live()) { hunt_tag(alert_uid); return true; }   /* BLE WATCH's LOCATE key */
    if (ch) {
        for (int b = 0; b < 4; b++)
            if (keys[b].key == ch && !keys[b].dim) { act(ch); return true; }
    }
    return false;
}
static bool touch(int col, int row)
{
    if (row >= H - kr) {
        if (dragging) return true;
        const int b = lap_ui_key_hit(col, row);
        if (b >= 0 && !keys[b].dim) act(keys[b].key);
        return true;
    }
    if (row >= ty && row < ty + tr && col >= sx && col < sx + sw) {
        const int t = (col - sx) * 3 / (sw > 0 ? sw : 1);
        if (t == 0) to_tab(tab - 1);
        else if (t == 2) to_tab(tab + 1);
        return true;
    }
    if (row == 0) {
        if (alert_live()) hunt_tag(alert_uid);            /* the alert over the title: find that tag */
        else if (status_line[0] && now_ms - status_ms < 4000u) status_ms = 0;
        else more();
        return true;
    }
    if (dragging) return true;
    if (tab == TAB_TAGS) {
        /* the radar: the alert's tag, else the pick */
        if (row >= hy && row < hy + 8 && (wide ? col <= hx + 15 : row < ty)) {
            hunt_tag(alert_live() ? alert_uid : bhw_pick());
            return true;
        }
        const int r = bhw_touch(col, row);
        if (r == 2) hunt_tag(bhw_pick());
        else if (r) cfg_sync();
        return true;
    }
    if (tab == TAB_FIND) {
        for (int i = 0; i < hit_n; i++)
            if (row >= hits[i].y0 && row < hits[i].y1 && row >= py + 1 && row < py + ph - 1) {
                if (sel == hits[i].idx) hunt_start(sel);     /* a second tap on the chosen card hunts it */
                else sel = hits[i].idx;
                return true;
            }
    }
    if (tab == TAB_SWEEP && state == ST_IDLE && row >= py && row < py + 9) {
        act(col < px + pw / 2 ? 'r' : 'c');              /* the room or the car picture */
        return true;
    }
    return true;
}

LAP_APP(.id = "bughunt", .name = "BUGHUNT", .sub = "trackers, tags and bugs", .version = "1.1",
        .author = "LakeShark", .color = LAP_GREEN,
        .icon_art = { " .-'''-.  ", "/  \\ /  \\ ", "| -(@)- | ", "\\  / \\  / ", " '-...-'\\ ", "         \\" },
        .flags = LAP_CAP_RADIO_RX,
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
