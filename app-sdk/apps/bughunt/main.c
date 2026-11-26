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
#include "bh_hunt.h"
#include "bh_lookup.h"
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
    /* 1.2: the finder and tracking. dmode PEAK or BODY; autorange the meter
       steps 20 dB as you close in; rx LORA, SDR or DUAL; sdr_dir the SDR has
       the directional antenna (else the LoRa radio has); watch_min how long a
       WATCH tag may go unheard; kind_off kinds that never alert */
    uint8_t dmode, autorange, rx, sdr_dir, watch_min, spare;
    uint16_t kind_off;
    /* 1.3: the SDR on a frequency HUNT. sdr_gain 0 AUTO or a step of the
       radio's range (SDR_GAINS); sdr_span 0 WIDE, 1 NARROW (a quarter) */
    uint8_t sdr_gain, sdr_span;
} cfg_t;
#define CFG_MAGIC 0xB6u
#define CFG_V10 6                       /* bytes BUGHUNT 1.0 saved */
#define CFG_V11 10                      /* bytes BUGHUNT 1.1 saved */
#define CFG_V12 18                      /* bytes BUGHUNT 1.2 saved */
#define SDR_GAINS 5                     /* AUTO, then 1/4, 1/2, 3/4 and all of the radio's range */
_Static_assert(sizeof(cfg_t) == CFG_V12 + 2, "1.3 adds two bytes after 1.2's");
enum { TONE_AUTO, TONE_TICKS, TONE_TONE };
enum { RX_LORA, RX_SDR, RX_DUAL };
static const char *const RX_NAMES[] = { "LORA", "SDR", "DUAL" };
static const uint8_t WATCH_MIN[] = { 1, 2, 5, 10, 15, 30 };
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
    if ((n == (int)sizeof(c) || n == CFG_V10 || n == CFG_V11 || n == CFG_V12) && c.magic == CFG_MAGIC && c.region >= 1 && c.region <= 3 &&
        c.room_i < 3 && c.car_i < 3) {
        cfg = c;
        if (n == CFG_V10) { cfg.follow_min = 10; cfg.possible = 0; cfg.log = 1; cfg.tone = TONE_AUTO; }   /* 1.0's: add 1.1's */
        if (n == CFG_V10 || n == CFG_V11) { cfg.dmode = BH_DM_PEAK; cfg.autorange = 1; cfg.rx = RX_LORA; cfg.sdr_dir = 1; cfg.watch_min = 2; cfg.kind_off = 0; }
    } else {
        memset(&cfg, 0, sizeof(cfg));
        cfg.magic = CFG_MAGIC; cfg.region = region_guess(); cfg.room_i = 1; cfg.car_i = 1; cfg.sound = 1;
        cfg.follow_min = 10; cfg.log = 1; cfg.tone = TONE_AUTO;
        cfg.autorange = 1; cfg.sdr_dir = 1; cfg.watch_min = 2;
    }
    if (cfg.follow_min < 2 || cfg.follow_min > 60) cfg.follow_min = 10;
    if (cfg.tone > TONE_TONE) cfg.tone = TONE_AUTO;
    if (cfg.dmode > BH_DM_BODY) cfg.dmode = BH_DM_PEAK;
    if (cfg.rx > RX_DUAL) cfg.rx = RX_LORA;
    if (cfg.watch_min < 1 || cfg.watch_min > 30) cfg.watch_min = 2;
    if (n != (int)sizeof(c)) { cfg.sdr_gain = 0; cfg.sdr_span = 0; }  /* 1.2's and older: add 1.3's */
    if (cfg.sdr_gain >= SDR_GAINS) cfg.sdr_gain = 0;
    if (cfg.sdr_span > 1) cfg.sdr_span = 0;
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
static bh_lvl_t lv;                     /* the level shown: the main receiver's */
static bh_range_t rng;                  /* the meter's range */
static bh_dir2_t dir2;                  /* turn-in-a-circle */
static bh_bursts_t bursts;              /* a frequency's bursts */
static bh_pair_t pair;                  /* DUAL: directional against reference */
static int hunt_floor = -1000;          /* the main receiver's quiet level (tenths) */
static uint32_t tick_next, tick_ms;
static bool hunt_scan;                  /* HUNT on a one-frequency version 2 scan */
static float heading = -1;
static bool have_compass;
#define HHIST 64
static float hhist[HHIST];
static int hhist_n;
/* which receivers a frequency HUNT runs on: 1 the LoRa radio, 2 the SDR */
enum { HR_LORA = 1, HR_SDR = 2 };
static int hunt_rx;
static char rx_note[64];
/* a tag's 1 m level from CAL (0 none), for the tag it was taken on */
static int cal_ref;
static uint32_t cal_uid;

/* The SDR on USB (RTL-SDR, HackRF): spectrum lines around the target */
static int sdr = -1;
static char sdr_name[16];
static uint64_t sdr_min, sdr_max;
static int sdr_h;
static uint32_t sdr_seq;
#define SPEC_N 256
static float spec[SPEC_N];
static int spec_n;
static uint64_t spec_center;
static uint32_t spec_rate;
static float sdr_floor = 1.0f;          /* dBFS, the spectrum's median; 1 = not yet */
static int16_t sdr_gmin, sdr_gmax;      /* tenths of a dB: the radio's gain range */
static bool sdr_has_gain;
static uint64_t sdr_center;             /* where the SDR is tuned: the target plus sdr_off */
static uint32_t sdr_off;
static uint32_t sdr_quiet_until;        /* lines still averaging the old tuning or gain: skipped */
static bool sdr_rebase;                 /* the gain changed: the meter starts from the new quiet level */
/* the strip: the window's bins on the line, the peak-hold trace, its top,
   and where the strongest signal last stood out over the quiet level */
static float spec_hold[SPEC_N];
static bool hold_ok;
static int sp_lo, sp_hi, sp_pk = -1;
static float sp_top;
static uint32_t pk_khz, pk_ms;          /* pk_khz 0: nothing yet */
static float pk_db;
static uint32_t lora_khz;               /* where the LoRa radio listens in a frequency hunt */
#define PK_OVER 6.0f                    /* dB over the quiet level that counts as a signal */

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
/* An SDR on USB that gives spectrum lines: HUNT can listen on it alone, or
   beside the LoRa radio (DUAL) with a directional antenna on one of them. */
static void find_sdr(void)
{
    lap_radio_info_t r[LAP_RADIOS_MAX];
    const int n = lap_radio_list(r, LAP_RADIOS_MAX);
    sdr = -1;
    for (int i = 0; i < n; i++) {
        if (r[i].kind != LAP_RADIO_KIND_RTL && r[i].kind != LAP_RADIO_KIND_HACKRF) continue;
        if (!r[i].present || !(r[i].features & LAP_RXF_SPECTRUM)) continue;
        sdr = i;
        snprintf(sdr_name, sizeof(sdr_name), "%s", r[i].name);
        sdr_min = r[i].min_hz;
        sdr_max = r[i].max_hz;
        sdr_has_gain = (r[i].features & LAP_RXF_GAIN) && r[i].gain_max > r[i].gain_min;
        sdr_gmin = r[i].gain_min;
        sdr_gmax = r[i].gain_max;
        return;
    }
}
static bool sdr_tunes(uint64_t hz) { return sdr >= 0 && hz >= sdr_min && hz <= sdr_max; }
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
/* ---- HUNT: the receivers ---- */
static void sdr_stop(void)
{
    if (sdr_h > 0) lap_rx_close(sdr_h);
    sdr_h = 0;
    spec_n = 0;
}
/* After a retune or a gain change the next lines still average the old
   setting: skip them, and start the strip and its hold afresh. */
static void sdr_settle(void)
{
    spec_n = 0;
    hold_ok = false;
    sp_pk = -1;
    sp_top = 0;
    sdr_quiet_until = lap_millis() + 500u;
}
/* AUTO, or a step of the radio's own range in whole dB */
static int sdr_gain_tenths(void)
{
    if (!cfg.sdr_gain || !sdr_has_gain) return LAP_GAIN_AUTO;
    const int g = sdr_gmin + (sdr_gmax - sdr_gmin) * cfg.sdr_gain / (SDR_GAINS - 1);
    const int r = (g + 5) / 10 * 10;
    return r > sdr_gmax ? g / 10 * 10 : r;           /* never over the top (49.6 dB on an RTL-SDR) */
}
static void sdr_gain_text(char *out, size_t cap)
{
    const int g = sdr_gain_tenths();
    if (g == LAP_GAIN_AUTO) snprintf(out, cap, "AUTO");
    else snprintf(out, cap, "%d dB", g / 10);
}
/* The window either side of the target the meter and the strip use: an
   eighth of the line (at most 250 kHz), a quarter of that NARROW. */
static int32_t sdr_half_hz(uint32_t rate)
{
    int32_t half = (int32_t)(rate / 8u);
    if (half > 250000) half = 250000;
    return cfg.sdr_span ? half / 4 : half;
}
/* The SDR tuned a little off the target, so its own centre spike (DC) is
   not read as the signal; spectrum lines only, no I/Q to carry. */
static int sdr_open(uint64_t hz)
{
    lap_rx_config_t c;
    memset(&c, 0, sizeof(c));
    lap_radio_info_t r[LAP_RADIOS_MAX];
    const int n = lap_radio_list(r, LAP_RADIOS_MAX);
    const uint32_t rate = sdr >= 0 && sdr < n && r[sdr].min_rate ? r[sdr].min_rate : 1000000u;
    c.rate_hz = 0;                          /* the radio's lowest rate */
    sdr_off = rate >= 400000u ? rate / 4u : 100000u;
    c.center_hz = hz + sdr_off;
    c.gain_tenths_db = sdr_gain_tenths();
    c.flags = LAP_RX_SPECTRUM_ONLY;
    const int h2 = lap_rx_open(sdr, &c);
    if (h2 > 0) {
        sdr_h = h2; sdr_seq = 0; sdr_floor = 1.0f; spec_n = 0;
        sdr_center = c.center_hz;
        hold_ok = false; sp_pk = -1; sp_top = 0;
        pk_khz = 0; pk_ms = 0;
        sdr_quiet_until = lap_millis(); sdr_rebase = false;
    }
    return h2;
}
static int fcmp(const void *a, const void *b)
{
    const float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}
/* The strongest bin within the window either side of the target (the
   LoRa radio's 500 kHz, or the span the rate allows), and the quiet level:
   the line's median. Keeps the peak-hold trace and where the peak is. */
static bool sdr_read(int *tenths)
{
    if (sdr_h <= 0) return false;
    uint32_t seq = 0;
    const int n = lap_rx_spectrum(sdr_h, spec, SPEC_N, &seq);
    if (n <= 0 || seq == sdr_seq) return false;
    sdr_seq = seq;
    if ((int32_t)(now_ms - sdr_quiet_until) < 0) return false;
    spec_n = n;
    lap_rx_status_t st;
    memset(&st, 0, sizeof(st));
    if (lap_rx_status(sdr_h, &st) || !st.rate_hz) return false;
    spec_center = st.center_hz;
    spec_rate = st.rate_hz;
    const int32_t off_hz = (int32_t)((int64_t)((uint64_t)tgt.khz * 1000u) - (int64_t)st.center_hz);   /* small: in 32 bits, no 64-bit float maths */
    int lo = 0, hi = 0;
    if (!bh_spec_window(n, st.rate_hz, off_hz, sdr_half_hz(st.rate_hz), &lo, &hi)) return false;
    int32_t pk_off = 0;
    const int pk = bh_spec_peak(spec, n, lo, hi, st.rate_hz, &pk_off);
    if (pk < 0 || spec[pk] <= -199.0f) return false;
    const float best = spec[pk];
    static float tmp[SPEC_N];
    memcpy(tmp, spec, sizeof(float) * (size_t)n);
    qsort(tmp, (size_t)n, sizeof(float), fcmp);
    const float med = tmp[n / 2];
    sdr_floor = sdr_floor > 0.5f ? med : sdr_floor + (med - sdr_floor) * 0.1f;
    /* the hold falls ~6 dB a second, and only as lines come */
    if (!hold_ok) memcpy(spec_hold, spec, sizeof(float) * (size_t)n);
    else bh_spec_hold(spec_hold, spec, n, 0.6f);
    hold_ok = true;
    sp_lo = lo; sp_hi = hi; sp_pk = pk;
    /* the strip's top: over the held peak, falling slowly, 30 dB at least */
    float top = sdr_floor + 30.0f;
    for (int i = lo; i <= hi; i++) if (spec_hold[i] + 3.0f > top) top = spec_hold[i] + 3.0f;
    sp_top = sp_top == 0 || top > sp_top ? top : sp_top - 0.3f;
    if (best >= sdr_floor + PK_OVER) {
        const int64_t at = (int64_t)st.center_hz + pk_off;
        pk_khz = at > 0 ? (uint32_t)((at + 500) / 1000) : 0;
        pk_db = best;
        pk_ms = now_ms;
    }
    *tenths = (int)(best * 10.0f);
    return true;
}

/* ---- HUNT ---- */
static uint32_t lvl_window(void) { return tgt.target == BH_T_BLE ? 2500u : tgt.target == BH_T_WIFI ? 1500u : 400u; }
static bool lv_fresh(void) { return lv.last_ms && now_ms - lv.last_ms < fresh_ms(); }
static int sdr_floor_t(void) { return sdr_floor > 0.5f ? -1000 : (int)(sdr_floor * 10.0f); }
static bool dual_on(void) { return (hunt_rx & HR_LORA) && (hunt_rx & HR_SDR); }
/* the main receiver: what the meter, the dial and the sound follow */
static bool main_is_sdr(void) { return (hunt_rx & HR_SDR) && (!(hunt_rx & HR_LORA) || cfg.sdr_dir); }
static void range_reset(void)
{
    const bool rf = tgt.target == BH_T_FREQ;
    bh_range_init(&rng, hunt_floor, rf ? 600 : 700, !cfg.autorange ? 0 : rf ? 3 : tgt.target == BH_T_WIFI ? 1 : 0);
}

/* One reading. src 0: the LoRa radio, Bluetooth or Wi-Fi; 1: the SDR. */
static void hunt_level_src(int src, int tenths)
{
    if (tgt.target == BH_T_FREQ && dual_on()) {
        /* DUAL: the pair is told which is the directional; the meter shows that one */
        const int dir_src = cfg.sdr_dir ? 1 : 0;
        bh_pair_floor(&pair, dir_src ? sdr_floor_t() : ref_or(-1080), dir_src ? ref_or(-1080) : sdr_floor_t());
        bh_pair_add(&pair, src == dir_src ? 0 : 1, tenths, now_ms);
        if (src != dir_src) return;
    }
    if (src == 1 && sdr_floor <= 0.5f && main_is_sdr() && (!lv.started || sdr_rebase)) {
        /* the SDR's quiet level is known now: the meter and the bursts start
           from it. After a gain change the old readings are another scale:
           the level, its history and the dial start again too. */
        if (sdr_rebase) { bh_lvl_init(&lv, lvl_window()); hhist_n = 0; bh_dir2_reset(&dir2); }
        hunt_floor = sdr_floor_t();
        range_reset();
        bursts.thr = (int16_t)(hunt_floor + 100);
    }
    if (src == 1 && sdr_floor <= 0.5f) sdr_rebase = false;
    bh_lvl_add(&lv, tenths, now_ms);
    if (cfg.autorange) bh_range_feed(&rng, bh_lvl_now(&lv, now_ms), now_ms);
    if (hhist_n == HHIST) { memmove(hhist, hhist + 1, sizeof(hhist[0]) * (HHIST - 1)); hhist_n--; }
    hhist[hhist_n++] = (float)tenths / 10.0f;
    if (tgt.target == BH_T_FREQ) bh_bursts_add(&bursts, tenths, heading >= 0 ? (int)heading : -1, now_ms);
    if (heading >= 0) {
        /* DUAL turns on the difference, so the transmitter's own level drops out */
        const int pt = dual_on() ? bh_pair_point(&pair, now_ms) : BH_HNONE;
        bh_dir2_add(&dir2, heading, pt != BH_HNONE ? pt - 1000 : tenths);
    }
}
static void hunt_level(int tenths) { hunt_level_src(0, tenths); }

/* The LoRa radio on a hunt frequency (alone, or beside the SDR in DUAL). */
static void hunt_lora_open(uint64_t hz)
{
    /* version 2: the peak of every 8 ms of listening (one report a
       worker pass, ~45 a second on the board), so a burst between
       frames still moves the meter; else one reading a frame */
    int r = scan_ver == 2 ? scan_open(&hz, 1, 8) : -1;
    if (r > 0 && scan_ver == 2) { rf_h = r; rf_pk = false; hunt_scan = true; hunt_rx |= HR_LORA; }
    else if (r == LAP_RX_EBUSY && (hunt_rx & HR_SDR)) {
        snprintf(rx_note, sizeof(rx_note), "DUAL needs newer firmware: %s only", sdr_name);
    } else {
        lap_packet_config_t c = rx_cfg(hz);
        r = lap_packet_open(radio, &c);
        if (r > 0) { rf_h = r; rf_pk = true; hunt_rx |= HR_LORA; }
        else if (r == LAP_RX_EBUSY && (hunt_rx & HR_SDR)) snprintf(rx_note, sizeof(rx_note), "DUAL needs newer firmware: %s only", sdr_name);
        else if (!(hunt_rx & HR_SDR)) snprintf(rf_why, sizeof(rf_why), "radio refused (%d)", r);
    }
    lora_khz = (uint32_t)(hz / 1000u);
}
/* A frequency: the LoRa radio, the SDR, or both (DUAL). */
static void hunt_rf_open(void)
{
    const uint64_t hz = (uint64_t)tgt.khz * 1000u;
    const bool lora_ok = radio >= 0, sdr_ok = sdr_tunes(hz);
    int want = 0;
    if (cfg.rx == RX_DUAL && lora_ok && sdr_ok) want = HR_LORA | HR_SDR;
    else if (cfg.rx != RX_LORA && sdr_ok) want = HR_SDR;
    else if (lora_ok) want = HR_LORA;
    else if (sdr_ok) want = HR_SDR;
    hunt_rx = 0;
    rx_note[0] = 0;
    if (cfg.rx != RX_LORA && sdr < 0) snprintf(rx_note, sizeof(rx_note), "no SDR on USB: LoRa radio only");
    else if (cfg.rx != RX_LORA && !sdr_ok) snprintf(rx_note, sizeof(rx_note), "%s cannot tune here: LoRa only", sdr_name);
    else if (cfg.rx == RX_DUAL && !lora_ok) snprintf(rx_note, sizeof(rx_note), "DUAL: no LoRa radio, %s only", sdr_name);
    if (want & HR_SDR) {
        const int r = sdr_open(hz);
        if (r > 0) hunt_rx |= HR_SDR;
        else {
            snprintf(rx_note, sizeof(rx_note), "%s refused (%d)%s", sdr_name, r, lora_ok ? ": LoRa only" : "");
            if (lora_ok) want |= HR_LORA;
        }
    }
    if (want & HR_LORA) hunt_lora_open(hz);
    hunt_floor = main_is_sdr() ? sdr_floor_t() : ref_or(-1080);
}
/* The SDR tools run on a frequency HUNT the SDR listens to (SDR or DUAL). */
static bool sdr_hunt(void) { return hunting && tgt.target == BH_T_FREQ && (hunt_rx & HR_SDR) && sdr_h > 0; }
/* A new hunt frequency (TUNE, SNAP). The SDR keeps its offset but only
   moves when the target has left the middle of its line, and the LoRa
   radio (500 kHz wide) when it moved over 25 kHz; the level, the dial and
   the bursts carry on, it being the same signal a little off. */
static void hunt_retune(uint32_t khz)
{
    if (!sdr_hunt() || !khz || khz == tgt.khz) return;
    const uint64_t hz = (uint64_t)khz * 1000u;
    char was[16], m[16];
    mhz(tgt.khz, was, sizeof(was));
    mhz(khz, m, sizeof(m));
    if (!sdr_tunes(hz)) { say("%s does not tune %s MHz", sdr_name, m); return; }
    /* the title names the frequency first ("315.000 MHz  US CAR REMOTES") */
    const size_t wl = strlen(was);
    if (!strncmp(tgt.title, was, wl)) {
        char t[sizeof(tgt.title) + sizeof(m)];
        snprintf(t, sizeof(t), "%s%s", m, tgt.title + wl);
        snprintf(tgt.title, sizeof(tgt.title), "%.*s", (int)sizeof(tgt.title) - 1, t);
    }
    tgt.khz = khz;
    const uint64_t want = hz + sdr_off;
    const uint64_t d = want > sdr_center ? want - sdr_center : sdr_center - want;
    const uint32_t rate = spec_rate ? spec_rate : sdr_off * 4u;
    if (d > rate / 16u) {
        const int r = lap_rx_tune(sdr_h, want);
        if (r) say("%s refused the retune (%d)", sdr_name, r);
        else { sdr_center = want; sdr_settle(); }
    }
    if ((hunt_rx & HR_LORA) && (khz > lora_khz ? khz - lora_khz : lora_khz - khz) > 25u) {
        rf_stop();
        hunt_rx &= ~HR_LORA;
        hunt_lora_open(hz);
    }
}
static void hunt_tune_by(int khz)
{
    if (!sdr_hunt()) return;
    const int64_t k = (int64_t)tgt.khz + khz;
    if (k < 1000) return;
    hunt_retune((uint32_t)k);
}
/* SNAP: the hunt onto the strongest signal in the window (the last time
   one stood over the quiet level, so a burst that has ended still counts). */
static void hunt_snap(void)
{
    if (!sdr_hunt()) return;
    char m[16];
    if (!pk_khz) { say("SNAP: nothing over the quiet level yet"); return; }
    mhz(pk_khz, m, sizeof(m));
    if (pk_khz == tgt.khz) { say("SNAP: already on %s MHz", m); return; }
    const uint32_t ago = (now_ms - pk_ms) / 1000u;
    hunt_retune(pk_khz);
    if (tgt.khz == pk_khz) {
        if (ago) say("SNAP: %s MHz, the peak %us ago", m, (unsigned)ago);
        else say("SNAP: %s MHz", m);
    }
}
/* GAIN: AUTO and four steps of the radio's range, kept for next time. */
static void sdr_gain_step(void)
{
    if (sdr < 0) { say("No SDR on USB"); return; }
    if (!sdr_has_gain) { say("%s: gain is automatic only", sdr_name); return; }
    cfg.sdr_gain = (uint8_t)((cfg.sdr_gain + 1) % SDR_GAINS);
    cfg_save();
    char g[16];
    sdr_gain_text(g, sizeof(g));
    if (sdr_hunt()) {
        const int r = lap_rx_gain(sdr_h, sdr_gain_tenths());
        if (r) { say("%s refused gain %s (%d)", sdr_name, g, r); return; }
        sdr_settle();
        sdr_floor = 1.0f;                   /* a new quiet level at this gain */
        sdr_rebase = true;
    }
    say("SDR gain %s", g);
}
static void sdr_span_toggle(void)
{
    cfg.sdr_span = !cfg.sdr_span;
    cfg_save();
    say(cfg.sdr_span ? "SDR span NARROW: the meter hears only near the target" : "SDR span WIDE");
}
static void hunt_begin(const bh_find_t *f, int from)
{
    tgt = *f;
    hunt_from = from;
    if (state == ST_SWEEP) { rf_stop(); state = ST_DONE; ended_ms = now_ms; }
    if (hunting && (rf_pk || hunt_scan)) rf_stop();
    sdr_stop();
    lap_tone_stop();
    hunting = true;
    hunt_rx = 0;
    rx_note[0] = 0;
    bh_lvl_init(&lv, lvl_window());
    bh_pair_init(&pair, 400);
    hhist_n = 0;
    tick_next = 0;
    bh_dir2_reset(&dir2);
    hunt_floor = tgt.target == BH_T_FREQ ? ref_or(-1080) : -1000;
    if (tgt.target == BH_T_FREQ) { rf_stop(); hunt_rf_open(); }
    else if (!wifi_h && !ble_cap && !ble_scan) sources_start();
    range_reset();
    bh_bursts_init(&bursts, hunt_floor);
    if (tgt.target == BH_T_BLE) {
        /* start from what the tag watch last heard, so the meter moves at once */
        const bw_dev_t *d = tgt.uid ? bhw_dev(tgt.uid) : NULL;
        if (d && bhw_now() - d->last_ms < 10000u) hunt_level((int)(d->smooth * 10.0f));
    }
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
    bhw_label(d, label, sizeof(label));
    snprintf(f.title, sizeof(f.title), "%s%s  %s", d->alert ? "! " : "", label, bw_class_tag(d->ad.cls));
    snprintf(f.detail, sizeof(f.detail), "%s", bw_class_note(d->ad.cls));
    bhw_pick_set(d->uid);
    if (alert_uid && bhw_dev(alert_uid) == d) alert_uid = 0;      /* hunting it answers the alert: the banner goes */
    hunt_begin(&f, TAB_TAGS);
}
/* HUNT a frequency named from the list or typed in: no sweep needed. */
static void hunt_freq(uint32_t khz, const char *what)
{
    bh_find_t f;
    memset(&f, 0, sizeof(f));
    f.target = BH_T_FREQ;
    f.kind = BH_F_CARRIER;
    f.khz = khz;
    char m[16];
    mhz(khz, m, sizeof(m));
    snprintf(f.title, sizeof(f.title), "%s MHz  %s", m, what && what[0] ? what : "YOUR PICK");
    snprintf(f.detail, sizeof(f.detail), "A frequency you named.");
    hunt_begin(&f, TAB_HUNT);
}
static void hunt_stop(void)
{
    if (!hunting) return;
    hunting = false;
    if (rf_pk || hunt_scan) rf_stop();
    hunt_scan = false;
    sdr_stop();
    hunt_rx = 0;
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
    if (nf < 2 || hunt_from != TAB_FIND) return;
    sel = (sel + by + nf) % nf;
    hunt_start(sel);
}
/* 0..1 across the meter's range now */
static float hunt_frac(float tenths) { return bh_range_frac(&rng, tenths); }
/* Ticks or a tone: AUTO is BLE WATCH's rising tone for a tag (an advert
   comes every second or two, a pitch says how strong at once) and the
   Geiger tick for a frequency or a station (readings many times a second). */
static bool hunt_tone(void) { return cfg.tone == TONE_TONE || (cfg.tone == TONE_AUTO && tgt.target == BH_T_BLE); }
static void hunt_poll(void)
{
    lap_motion_t m;
    const bool mo = lap_motion(&m);
    if (mo) have_compass = true;
    heading = mo && m.heading >= 0 ? m.heading : -1;
    if (hunt_scan && rf_h > 0) {
        lap_packet_meta_t pm;
        while (lap_scan_next(rf_h, &pm) > 0)
            if (pm.rssi_tenths_dbm != INT16_MIN) hunt_level_src(0, pm.rssi_tenths_dbm);
    } else if (rf_pk && rf_h > 0) {
        lap_packet_meta_t pm = { .size = sizeof(pm), .version = 1 };
        if (!lap_radio_rssi(rf_h, &pm) && pm.timestamp_us != last_ts && pm.rssi_tenths_dbm != INT16_MIN) {
            last_ts = pm.timestamp_us;
            hunt_level_src(0, pm.rssi_tenths_dbm);
        }
    }
    int st;
    if (sdr_h > 0 && sdr_read(&st)) hunt_level_src(1, st);
    if (tgt.target == BH_T_FREQ) bh_bursts_tick(&bursts, now_ms);
    if (cfg.autorange && bh_lvl_now(&lv, now_ms) == BH_HNONE) bh_range_feed(&rng, BH_HNONE, now_ms);
    if (hunt_tone()) {
        /* the tone: pitch and rate follow the smoothed signal, quiet once
           the target has not been heard for 3 s */
        const float q = hunt_frac(lv.fast);
        if (cfg.sound && lv.last_ms && now_ms - lv.last_ms < 3000u && !lap_tone_busy() && now_ms >= tick_next) {
            lap_tone(300 + (int)(q * 1700.0f), 50, 70, LAP_WAVE_SINE);
            tick_next = now_ms + 900u - (uint32_t)(q * 780.0f);
            tick_ms = now_ms;
        }
        return;
    }
    /* Geiger: faster and higher as it gets stronger within the meter's
       range, so it does not sit at its fastest beside the thing */
    if (cfg.sound && lv_fresh() && now_ms >= tick_next) {
        const int over = (int)lv.fast - bh_range_lo(&rng);
        const int hz = 500 + (over > 0 ? over : 0) * 3;
        lap_tone(hz > 3200 ? 3200 : hz, 14, 60, LAP_WAVE_SQUARE);
        tick_next = now_ms + bh_geiger_ms((int)lv.fast, bh_range_lo(&rng));
        tick_ms = now_ms;
    }
}
/* CAL: the tag is held 1 m away now; its level is the reference */
static void hunt_cal(void)
{
    if (!hunting || tgt.target != BH_T_BLE) { say("CAL: hunt a tag and hold it 1 m away"); return; }
    const int w = bh_lvl_now(&lv, now_ms);
    if (w == BH_HNONE) { say("CAL: the tag is not heard just now"); return; }
    cal_ref = w / 10;
    cal_uid = tgt.uid;
    say("CAL: %d dBm is 1 m for this tag", cal_ref);
}
static int tag_ref(const bw_dev_t *d)
{
    if (cal_ref && d && cal_uid && bhw_dev(cal_uid) == d) return cal_ref;
    return d ? bh_dist_ref(d->ad.cls, d->ad.has_tx, d->ad.tx_power) : -60;
}

/* ---- the screen ---- */
static uint8_t zone_hue(float q);
static const char *zone_word(float q);
static const char *unit(void);
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
    if (hunting) bug = lv.started && lv.last_ms && now_ms - lv.last_ms < 10000u ? A(zone_hue(hunt_frac(lv.fast))) : A(LAP_BLUE);
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
        const bool fr = lv.started && lv.last_ms && now_ms - lv.last_ms < 10000u;
        if (fr) snprintf(big, sizeof(big), "%d", (int)(lv.fast / 10.0f)); else snprintf(big, sizeof(big), "--");
        bat = A(fr ? zone_hue(hunt_frac(lv.fast)) : LAP_BLUE);
        const bw_dev_t *td = tgt.target == BH_T_BLE && tgt.uid ? bhw_dev(tgt.uid) : NULL;
        if (td && fr) {
            /* a tag: how far, from its level and what its kind reads at 1 m */
            char dt[24];
            bh_dist_text((int)lv.fast / 10, tag_ref(td), dt, sizeof(dt));
            snprintf(under_t, sizeof(under_t), "dBm  %s", dt);
        } else snprintf(under_t, sizeof(under_t), "%s  %s", unit(), fr ? zone_word(hunt_frac(lv.fast)) : "not heard");
        under = under_t;
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
    if (hunting && lv.started) {
        frac = hunt_frac(lv.fast);
        peak = lv.peak == BH_HNONE ? -1 : hunt_frac((float)lv.peak);
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
static const char *compass_word(int deg)
{
    static const char *const n[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    return n[(((deg % 360) + 360) % 360 + 22) / 45 % 8];
}
/* the meter's zones, cold to hot: the same four everywhere on the screen */
static uint8_t zone_hue(float q)
{
    return q >= 0.64f ? (LAP_RED | LAP_BRIGHT) : q >= 0.45f ? (LAP_YELLOW | LAP_BRIGHT) : q >= 0.25f ? (LAP_GREEN | LAP_BRIGHT) : (LAP_BLUE | LAP_BRIGHT);
}
static uint8_t zone_dim(float q) { return q >= 0.64f ? LAP_RED : q >= 0.45f ? LAP_YELLOW : q >= 0.25f ? LAP_GREEN : LAP_BLUE; }
static const char *zone_word(float q)
{
    if (tgt.target == BH_T_BLE) return q >= 0.64f ? "HERE" : q >= 0.45f ? "CLOSE" : q >= 0.25f ? "NEARBY" : "FAR";
    return q >= 0.64f ? "HOT" : q >= 0.45f ? "STRONG" : q >= 0.25f ? "FAIR" : "WEAK";
}
static const char *unit(void) { return tgt.target == BH_T_FREQ && main_is_sdr() ? "dBFS" : "dBm"; }

/* The proximity gauge: half a dial, cold on the left and hot on the right.
   The scale lights up to the level, the needle shows it, the red tick the
   most it has been; under it the range's ends and the zone in words. */
static void gauge(int x, int y, int w, int hgt, float q, float pk, bool fresh)
{
    cv_begin(x, y, w, hgt);
    const float cx = (float)cv.w - 0.5f, cy = (float)cv.h * 4.0f - 1.0f;
    float R = cx - 1.0f;
    if (R > (cy - 1.0f) * cv.ax) R = (cy - 1.0f) * cv.ax;
    if (R < 6) return;
    const int n = (int)(R * 4.0f);
    for (int i = 0; i <= n; i++) {
        const float t = (float)i / (float)n, a = 3.1415926f * (1.0f - t);
        const bool lit = fresh && t <= q;
        /* a gap between zones, so the four read as four */
        if (fabsf(t - 0.25f) < 0.012f || fabsf(t - 0.45f) < 0.012f || fabsf(t - 0.64f) < 0.012f) continue;
        const uint8_t at = lit ? A(zone_hue(t)) : A(zone_dim(t));
        for (float rr = R; rr >= R - (lit ? 2.0f : 1.0f); rr -= 1.0f) {
            if (!lit && rr < R && i % 2) continue;      /* the unlit scale is thinner */
            cv_dot(rnd(cx + rr * cosf(a)), rnd(cy - rr * sinf(a) / cv.ax), at);
        }
    }
    /* ticks every tenth, inside the scale */
    for (int k = 0; k <= 10; k++) {
        const float a = 3.1415926f * (1.0f - (float)k / 10.0f), r0 = R - 4.0f, r1 = R - (k % 5 ? 5.0f : 7.0f);
        cv_line(rnd(cx + r0 * cosf(a)), rnd(cy - r0 * sinf(a) / cv.ax), rnd(cx + r1 * cosf(a)), rnd(cy - r1 * sinf(a) / cv.ax), LAP_UI_FAINT);
    }
    if (pk >= 0) {
        const float a = 3.1415926f * (1.0f - pk);
        cv_line(rnd(cx + (R - 4) * cosf(a)), rnd(cy - (R - 4) * sinf(a) / cv.ax), rnd(cx + (R - 9) * cosf(a)), rnd(cy - (R - 9) * sinf(a) / cv.ax),
                A(LAP_RED | LAP_BRIGHT));
    }
    if (fresh) {
        const float a = 3.1415926f * (1.0f - q);
        const uint8_t na = A(LAP_WHITE | LAP_BRIGHT);
        cv_line(rnd(cx), rnd(cy), rnd(cx + (R - 6) * cosf(a)), rnd(cy - (R - 6) * sinf(a) / cv.ax), na);
        cv_line(rnd(cx) + 1, rnd(cy), rnd(cx + (R - 6) * cosf(a)) + 1, rnd(cy - (R - 6) * sinf(a) / cv.ax), na);
    }
    for (int dx = -2; dx <= 2; dx++) cv_dot(rnd(cx) + dx, rnd(cy), A(LAP_WHITE | LAP_BRIGHT));    /* the hub */
    cv_dot(rnd(cx), rnd(cy) - 1, A(LAP_WHITE | LAP_BRIGHT));
    cv_flush();
}

/* The dial: the circle you turned through, heading up (the top is where the
   board points, so the letters turn as you turn). Each 10 degrees is lit by
   how loud it was there; faint dots are the part not turned through yet. The
   arrow is the answer: where the thing is from where you face. */
static void dial(int x, int y, int dc, int dr, const bh_dir_fix_t *fx, float hd)
{
    if (dc < 10 || dr < 6) return;
    cv_begin(x + 1, y + 1, dc - 2, dr - 2);
    const float cx = (float)cv.w - 0.5f, cy = (float)cv.h * 2.0f - 0.5f;
    float R = cx - 1.0f;
    if (R > (cy - 1.0f) * cv.ax) R = (cy - 1.0f) * cv.ax;
    if (R < 6) return;
    const float face = hd >= 0 ? hd : 0;
    const int lo = dir2.lo, hi = dir2.hi;
    const int n = (int)(R * 7.0f);
    for (int i = 0; i < n; i++) {
        const float sa = 360.0f * (float)i / (float)n;          /* clockwise from the top */
        const int b = ((int)((face + sa) / 10.0f + 0.5f)) % BH_DIR2;
        const int v = bh_dir2_bin(&dir2, b);
        const float a = sa * 0.0174533f;
        const float px2 = cx + R * sinf(a), py2 = cy - R * cosf(a) / cv.ax;
        if (v == BH_HNONE) {
            if (i % 3 == 0) cv_dot(rnd(px2), rnd(py2), LAP_UI_FAINT);
            continue;
        }
        const bool flat = hi - lo < 20;                         /* all alike so far: no colour means anything */
        const float q = flat ? 0.0f : (float)(v - lo) / (float)(hi - lo);
        const uint8_t at = flat ? A(LAP_GREEN) : A(q >= 0.75f ? (LAP_RED | LAP_BRIGHT) : q >= 0.5f ? (LAP_YELLOW | LAP_BRIGHT) : q >= 0.25f ? (LAP_GREEN | LAP_BRIGHT) : LAP_BLUE);
        const float thick = 1.0f + q * 2.0f;                    /* the loud part is bolder */
        for (float rr = R; rr >= R - thick; rr -= 1.0f) cv_dot(rnd(cx + rr * sinf(a)), rnd(cy - rr * cosf(a) / cv.ax), at);
    }
    /* where you face: a short white tick at the top */
    cv_line(rnd(cx), rnd(cy - (R - 4) / cv.ax), rnd(cx), rnd(cy - (R - 8) / cv.ax), A(LAP_WHITE | LAP_BRIGHT));
    /* the answer, once one way stands out */
    if (fx->bearing >= 0 && fx->contrast >= 20) {
        const float a = (float)bh_turn((int)face, fx->bearing) * 0.0174533f;
        const uint8_t at = A(fx->quality >= 3 ? (LAP_RED | LAP_BRIGHT) : fx->quality == 2 ? (LAP_YELLOW | LAP_BRIGHT) : LAP_WHITE);
        const float tip = R - 5.0f, sx2 = sinf(a), sy2 = -cosf(a) / cv.ax, nx = cosf(a), ny = sinf(a) / cv.ax;
        for (int k = -1; k <= 1; k++)
            cv_line(rnd(cx + 3 * sx2 + k * nx), rnd(cy + 3 * sy2 + k * ny), rnd(cx + (tip - 3) * sx2 + k * nx), rnd(cy + (tip - 3) * sy2 + k * ny), at);
        for (int side = -1; side <= 1; side += 2) {
            const float b2 = a + (float)side * 2.7f;            /* the head's barbs, back from the tip */
            cv_line(rnd(cx + tip * sx2), rnd(cy + tip * sy2), rnd(cx + tip * sx2 + 7 * sinf(b2)), rnd(cy + tip * sy2 - 7 * cosf(b2) / cv.ax), at);
        }
    }
    cv_ring(cx, cy, 2.0f, 0, A(LAP_WHITE | LAP_BRIGHT));
    cv_flush();
    /* the compass letters, just outside the ring, turning with you */
    static const char *const L[4] = { "N", "E", "S", "W" };
    for (int k = 0; k < 4; k++) {
        const float a = (float)(k * 90 - (int)face) * 0.0174533f;
        int col = x + 1 + (int)((cx + (R + 3.0f) * sinf(a)) / 2.0f), row = y + 1 + (int)((cy - (R + 3.0f) * cosf(a) / cv.ax) / 4.0f);
        col = col < x ? x : col > x + dc - 1 ? x + dc - 1 : col;
        row = row < y ? y : row > y + dr - 1 ? y + dr - 1 : row;
        cell(col, row, (int16_t)L[k][0], A(k == 0 ? (LAP_RED | LAP_BRIGHT) : LAP_CYAN));
    }
    if (hd < 0) put(x + 1, y + dr - 1, dc - 2, LAP_UI_FAINT, "north up");
}

/* The strip under the gauge: a tag's last two minutes; a frequency's
   bursts and when the next is due; DUAL's pointing difference. Rows used. */
static int hunt_strip(int x, int y, int w, int rows_left)
{
    const bw_dev_t *tag = tgt.target == BH_T_BLE && tgt.uid ? bhw_dev(tgt.uid) : NULL;
    int yy = y;
    if (dual_on() && rows_left >= 3) {
        const int pt = bh_pair_point(&pair, now_ms);
        const char *dn = cfg.sdr_dir ? sdr_name : radio_name, *rn = cfg.sdr_dir ? radio_name : sdr_name;
        put(x, yy, 6, LAP_UI_FAINT, "POINT");
        if (pt == BH_HNONE) put(x + 6, yy, w - 6, LAP_UI_DIM, "waiting for both to hear it");
        else put(x + 6, yy, w - 6, A(pt >= 60 ? (LAP_RED | LAP_BRIGHT) : pt >= 20 ? (LAP_YELLOW | LAP_BRIGHT) : pt > -20 ? LAP_WHITE : (LAP_CYAN | LAP_BRIGHT)),
                 "%+.1f dB  %s", (double)pt / 10.0, pt >= 60 ? "ON IT" : pt >= 20 ? "TOWARD IT" : pt > -20 ? "SIDE ON" : "AWAY");
        /* -20 .. +20 dB, the middle is the two hearing it alike */
        const int mw = w - 2, mid = x + 1 + mw / 2;
        put(x, yy + 1, 1, LAP_UI_FAINT, "[");
        put(x + w - 1, yy + 1, 1, LAP_UI_FAINT, "]");
        for (int i = 0; i < mw; i++) cell(x + 1 + i, yy + 1, i == mw / 2 ? '|' : (i % 5 ? '-' : '+'), LAP_UI_FAINT);
        if (pt != BH_HNONE) {
            int at = mid + (int)((float)pt / 200.0f * (float)(mw / 2));
            at = at < x + 1 ? x + 1 : at > x + mw ? x + mw : at;
            for (int c = at < mid ? at : mid; c <= (at < mid ? mid : at); c++) cell(c, yy + 1, '=', A(pt >= 20 ? LAP_YELLOW | LAP_BRIGHT : LAP_CYAN));
            cell(at, yy + 1, 'O', A(LAP_WHITE | LAP_BRIGHT));
        }
        put(x, yy + 2, w, LAP_UI_FAINT, "DIR %.12s  REF %.12s  -20 dB   +20 dB", dn, rn);
        yy += 3;
        rows_left -= 3;
    }
    if (tag && rows_left >= 3) {
        int cells = w - 5 - 6;
        if (cells > BW_HIST / 2) cells = BW_HIST / 2;
        put(x, yy, 5, LAP_UI_FAINT, "LAST");
        put(x, yy + 1, 5, LAP_UI_FAINT, "%ds", cells * 2 * BW_HIST_MS / 1000);
        bhw_graph(x + 5, yy, cells, 3, tag);
        return yy + 3 - y;
    }
    if (tgt.target == BH_T_FREQ && rows_left >= 3) {
        /* bursts: when a tracker checks in rarely, compare one burst with the last */
        char due[24] = "", per[24] = "";
        const int32_t d = bh_bursts_due(&bursts, now_ms);
        if (bursts.period_ms) snprintf(per, sizeof(per), "every ~%us", (unsigned)((bursts.period_ms + 500u) / 1000u));
        if (d > 0) snprintf(due, sizeof(due), "next %u:%02u", (unsigned)(d / 60000), (unsigned)(d / 1000 % 60));
        else if (d == 0) snprintf(due, sizeof(due), "due now");
        const bool steady = bursts.in && now_ms - bursts.cur.t_ms > 5000u;
        if (steady) {
            put(x, yy, w, A(LAP_MAGENTA | LAP_BRIGHT), "STEADY CARRIER");
            put(x + 15, yy, w - 15, LAP_UI_DIM, "on %us: walk with the meter", (unsigned)((now_ms - bursts.cur.t_ms) / 1000u));
        } else {
            put(x, yy, w, LAP_UI_FAINT, "BURSTS");
            put(x + 7, yy, w - 7, bursts.in ? A(LAP_RED | LAP_BRIGHT) : A(LAP_UI_VALUE), "%s%u  %s  %s", bursts.in ? "ON AIR " : "", (unsigned)bursts.total, per, due);
        }
        if (bursts.n >= 2 && !steady) {
            const int dl = bh_bursts_delta(&bursts);
            const char *wd = dl >= 30 ? "HOTTER" : dl <= -30 ? "COLDER" : "SAME";
            put(x + w - 14, yy, 14, A(dl >= 30 ? (LAP_RED | LAP_BRIGHT) : dl <= -30 ? (LAP_CYAN | LAP_BRIGHT) : LAP_WHITE), "%+5.1f %s", (double)dl / 10.0, wd);
        }
        /* the readings, with each burst's peak marked over them */
        if (hhist_n > 1) lap_ui_history(x, yy + 1, w - 6, 2, hhist, hhist_n, (float)bh_range_lo(&rng) / 10.0f, (float)bh_range_hi(&rng) / 10.0f, LAP_GREEN | LAP_BRIGHT);
        else put(x, yy + 1, w - 6, LAP_UI_FAINT, "listening on %s", hunt_rx == HR_SDR ? sdr_name : radio_name);
        if (lv.peak != BH_HNONE) put(x + w - 5, yy + 1, 5, A(LAP_RED | LAP_BRIGHT), "%4d", lv.peak / 10);
        put(x + w - 5, yy + 2, 5, LAP_UI_FAINT, " peak");
        return yy + 3 - y;
    }
    if (hhist_n > 1 && rows_left >= 2) {
        put(x, yy, 5, LAP_UI_FAINT, "LAST");
        lap_ui_history(x + 5, yy, w - 5 - 6, 2, hhist, hhist_n, (float)bh_range_lo(&rng) / 10.0f, (float)bh_range_hi(&rng) / 10.0f, LAP_GREEN | LAP_BRIGHT);
        if (lv.peak != BH_HNONE) put(x + w - 5, yy, 5, A(LAP_RED | LAP_BRIGHT), "%4d", lv.peak / 10);
        return yy + 2 - y;
    }
    return yy - y;
}

/* ---- the SDR panel: a frequency HUNT on the SDR (SDR or DUAL) ---- */
static void secs_text(uint32_t ms, char *out, size_t cap)
{
    if (ms < 100u) snprintf(out, cap, "<0.1s");
    else if (ms < 100000u) snprintf(out, cap, "%u.%us", (unsigned)(ms / 1000u), (unsigned)(ms / 100u % 10u));
    else snprintf(out, cap, "%um%02us", (unsigned)(ms / 60000u), (unsigned)(ms / 1000u % 60u));
}
/* where a frequency falls on the strip, in dots; -1 off it */
static int sdr_dot_of(int64_t hz, int dots)
{
    if (!spec_rate || !spec_n || sp_hi <= sp_lo) return -1;
    const float bin = (float)(int32_t)(hz - (int64_t)spec_center) * (float)spec_n / (float)spec_rate + (float)(spec_n / 2);
    const float at = (bin - (float)sp_lo + 0.5f) * (float)dots / (float)(sp_hi - sp_lo + 1);
    return at < 0 || at >= (float)dots ? -1 : (int)at;
}
/* The peak's frequency, read out like a radio's display: bright while the
   signal is on, plain with its age once it has gone; the gain at the end. */
static void sdr_readout(int x, int y, int w)
{
    int k = put(x, y, w, LAP_UI_FAINT, "PEAK ");
    if (pk_khz) {
        const bool live = now_ms - pk_ms < 600u;
        char m[16];
        mhz(pk_khz, m, sizeof(m));
        for (const char *c = m; *c && k < w; c++, k++)
            cell(x + k, y, *c, A(*c == '.' ? LAP_WHITE : live ? (LAP_GREEN | LAP_BRIGHT) : LAP_GREEN));
        k += put(x + k, y, w - k, A(LAP_GREEN), " MHz ");
        const int d = (int)pk_khz - (int)tgt.khz;
        if (d) k += put(x + k, y, w - k, A(LAP_YELLOW | LAP_BRIGHT), "%+dk ", d);
        else k += put(x + k, y, w - k, A(LAP_CYAN | LAP_BRIGHT), "ON IT ");
        if (live) k += put(x + k, y, w - k, A(level_hue((int)(pk_db - sdr_floor))), "%d dBFS", (int)pk_db);
        else {
            char a[16];
            secs_text(now_ms - pk_ms, a, sizeof(a));
            k += put(x + k, y, w - k, LAP_UI_DIM, "%s ago", a);
        }
    } else k += put(x + k, y, w - k, A(LAP_BLUE), "---.--- MHz  quiet");
    char g[16], t[24];
    sdr_gain_text(g, sizeof(g));
    const int gl = snprintf(t, sizeof(t), "GAIN %s", g);
    if (k + gl + 1 <= w) {
        put(x + w - gl, y, 5, LAP_UI_FAINT, "GAIN ");
        put(x + w - gl + 5, y, gl - 5, A(cfg.sdr_gain ? (LAP_MAGENTA | LAP_BRIGHT) : LAP_UI_VALUE), "%s", g);
    }
}
static int ydot(float v, float lo_db, float sc, int dh)
{
    const int y = dh - 1 - (int)((v - lo_db) * sc + 0.5f);
    return y < 0 ? 0 : y > dh - 1 ? dh - 1 : y;
}
/* The spectrum around the target in braille: the live line coloured by how
   far over the quiet level it stands, the peak hold over it in magenta, the
   burst threshold dotted in yellow and the target a dotted white upright. */
static void sdr_strip(int x, int y, int w, int rows)
{
    if (!spec_n || !hold_ok || sp_hi <= sp_lo || sdr_floor > 0.5f) {
        put(x, y + rows / 2, w, LAP_UI_FAINT, "  %s: waiting for a spectrum line", sdr_name);
        return;
    }
    cv_begin(x, y, w, rows);
    const int dw = cv.w * 2, dh = cv.h * 4, nb = sp_hi - sp_lo + 1;
    const float lo_db = sdr_floor - 4.0f, hi_db = sp_top > lo_db + 10.0f ? sp_top : lo_db + 10.0f;
    const float sc = (float)(dh - 1) / (hi_db - lo_db);
    const int thr = ydot((float)bursts.thr / 10.0f, lo_db, sc, dh);
    const bool thr_on = main_is_sdr() && (float)bursts.thr / 10.0f > lo_db && (float)bursts.thr / 10.0f < hi_db;
    const int tc = sdr_dot_of((int64_t)tgt.khz * 1000, dw);
    for (int c = 0; c < dw; c++) {
        if (thr_on && c % 3 == 0) cv_dot(c, thr, A(LAP_YELLOW));
        if (c == tc) for (int r = 0; r < dh; r += 2) cv_dot(c, r, A(LAP_WHITE | LAP_BRIGHT));
    }
    int py = -1;
    for (int c = 0; c < dw; c++) {
        int b0 = sp_lo + c * nb / dw, b1 = sp_lo + (c + 1) * nb / dw - 1;
        if (b1 < b0) b1 = b0;
        float v = -200.0f, hv = -200.0f;
        for (int b = b0; b <= b1 && b < spec_n; b++) {
            if (spec[b] > v) v = spec[b];
            if (spec_hold[b] > hv) hv = spec_hold[b];
        }
        const int yv = ydot(v, lo_db, sc, dh), yh = ydot(hv, lo_db, sc, dh);
        if (yh < yv - 1) cv_dot(c, yh, A(LAP_MAGENTA | LAP_BRIGHT));
        const uint8_t at = A(level_hue((int)(v - sdr_floor)));
        if (py < 0) cv_dot(c, yv, at);
        else cv_line(c - 1, py, c, yv, at);
        py = yv;
    }
    cv_flush();
}
/* Under the strip: its span, the target (^) and the strongest bin (*). */
static void sdr_axis(int x, int y, int w)
{
    for (int i = 0; i < w; i++) cell(x + i, y, i % 5 ? '-' : '+', LAP_UI_FAINT);
    if (spec_rate && spec_n) {
        const int32_t half = (int32_t)((int64_t)(sp_hi - sp_lo + 1) * spec_rate / spec_n / 2);
        char l[12];
        const int ll = snprintf(l, sizeof(l), "-%dk", (int)(half / 1000));
        put(x, y, ll, A(LAP_CYAN), "%s", l);
        put(x + w - ll, y, ll, A(LAP_CYAN), "+%dk", (int)(half / 1000));
    }
    const int tc = sdr_dot_of((int64_t)tgt.khz * 1000, w * 2);
    if (tc >= 0) cell(x + tc / 2, y, '^', A(LAP_WHITE | LAP_BRIGHT));
    if (sp_pk >= 0 && spec_n && spec[sp_pk] >= sdr_floor + PK_OVER && sp_hi > sp_lo) {
        const int pc = (int)((float)(sp_pk - sp_lo) * (float)w / (float)(sp_hi - sp_lo + 1));
        if (pc >= 0 && pc < w) cell(x + pc, y, '*', A(LAP_RED | LAP_BRIGHT));
    }
}
/* The burst timing: how long the last one was and how far apart they come
   (the median start to start of the last five), the clue to what it is. */
static void sdr_timing(int x, int y, int w, bool count)
{
    int k = 0;
    char a[16], b[16], c[16];
    if (count) {
        k += put(x + k, y, w - k, LAP_UI_FAINT, "BURSTS ");
        k += put(x + k, y, w - k, A(LAP_UI_VALUE), "%u  ", (unsigned)bursts.total);
    }
    if (bursts.in) {
        secs_text(now_ms - bursts.cur.t_ms, a, sizeof(a));
        k += put(x + k, y, w - k, A(LAP_RED | LAP_BRIGHT), "ON AIR %s  ", a);
    } else {
        k += put(x + k, y, w - k, LAP_UI_FAINT, "LAST ");
        if (bursts.n) { secs_text(bursts.b[bursts.n - 1].dur_ms, a, sizeof(a)); k += put(x + k, y, w - k, A(LAP_YELLOW | LAP_BRIGHT), "%s  ", a); }
        else k += put(x + k, y, w - k, A(LAP_BLUE), "--  ");
    }
    k += put(x + k, y, w - k, LAP_UI_FAINT, "GAP ");
    uint32_t lo = 0, hi = 0;
    const uint32_t g = bh_bursts_gap(&bursts, 5, &lo, &hi);
    if (!g) { put(x + k, y, w - k, A(LAP_BLUE), "--"); return; }
    secs_text(g, a, sizeof(a));
    k += put(x + k, y, w - k, A(LAP_CYAN | LAP_BRIGHT), "%s ", a);
    if (hi > lo) {
        secs_text(lo, b, sizeof(b));
        secs_text(hi, c, sizeof(c));
        put(x + k, y, w - k, LAP_UI_DIM, "%s-%s", b, c);
    }
}
/* Readout, strip, axis, timing: rows used (0 when it does not fit). */
static int sdr_panel(int x, int y, int w, int rows, bool count)
{
    if (!sdr_hunt() || rows < 5 || w < 24) return 0;
    int sr = rows - 3;
    if (sr > 4) sr = 4;
    sdr_readout(x, y, w);
    sdr_strip(x, y + 1, w, sr);
    sdr_axis(x, y + 1 + sr, w);
    sdr_timing(x, y + 2 + sr, w, count);
    return sr + 3;
}

static int dial_x0, dial_y0, dial_x1, dial_y1;  /* where the dial was drawn: a tap there starts it over */
static void draw_hunt(int x, int y, int w, int hgt)
{
    dial_x0 = dial_x1 = -1;
    if (!hunting) {
        clip0 = y; clip1 = y + hgt;
        emblem(x + w / 2 - 6, y + 1, false);
        put(x + 1, y + 10, w - 2, LAP_UI_TEXT, "Pick a finding or a tag, then HUNT.");
        put(x + 1, y + 11, w - 2, LAP_UI_DIM, "Or HUNT FREQ: a frequency from a list or typed.");
        int yy = y + 13;
        put(x + 1, yy++, w - 2, LAP_UI_FAINT, "RECEIVERS");
        put(x + 2, yy++, w - 3, A(radio >= 0 ? (LAP_GREEN | LAP_BRIGHT) : LAP_BLUE), "%-8s %s", "LORA", radio >= 0 ? radio_name : "none in the socket");
        put(x + 2, yy++, w - 3, A(sdr >= 0 ? (LAP_MAGENTA | LAP_BRIGHT) : LAP_BLUE), "%-8s %s", "SDR", sdr >= 0 ? sdr_name : "none on USB (RTL-SDR, HackRF)");
        put(x + 2, yy++, w - 3, A(LAP_UI_VALUE), "%-8s %s", "USING", cfg.rx == RX_DUAL ? "DUAL: both on one frequency" : cfg.rx == RX_SDR ? "the SDR when it tunes there" : "the LoRa radio");
        yy++;
        put(x + 1, yy++, w - 2, LAP_UI_FAINT, "DIRECTION");
        wrap(x + 2, yy, w - 3, 3, LAP_UI_DIM, cfg.dmode == BH_DM_BODY
             ? "BODY: hold the board flat against your chest and turn. Your body blocks the signal; the quietest way is your back to it."
             : "PEAK: hold the board flat in front of you and turn. The loudest way is toward it.");
        yy += 4;
        if (yy + 4 < y + hgt) {
            put(x + 1, yy++, w - 2, LAP_UI_FAINT, "DUAL");
            wrap(x + 2, yy, w - 3, 4, LAP_UI_DIM,
                 "A directional antenna on one receiver, the other as the reference: their difference shows where the directional one points, "
                 "even while a tracker's bursts come and go.");
        }
        return;
    }
    const bool fresh = lv_fresh();
    const float q = fresh ? hunt_frac(lv.fast) : 0;
    const float pk = lv.peak == BH_HNONE ? -1 : hunt_frac((float)lv.peak);
    const bw_dev_t *tag = tgt.target == BH_T_BLE && tgt.uid ? bhw_dev(tgt.uid) : NULL;
    /* landscape: the dial takes the right side */
    int dx = x, dyy = 0, dcw = 0, drh = 0;
    const int full_w = w;
    if (wide && w >= 64) { dcw = 46; w -= dcw + 2; dx = x + w + 2; dyy = y; drh = hgt; }
    int yy = y;
    /* the target */
    put(x, yy, w, LAP_UI_TEXT, "%s", tgt.title);
    yy++;
    char tt[40];
    target_text(&tgt, tt, sizeof(tt));
    int tl = put(x, yy, w, A(LAP_UI_VALUE), "%s", tt) + 1;
    if (tag) {
        if (tag->mark) tl += put(x + tl, yy, w - tl, A(tag->mark == BW_MARK_MINE ? (LAP_GREEN | LAP_BRIGHT) : (LAP_CYAN | LAP_BRIGHT)), "%s ",
                                 tag->mark == BW_MARK_MINE ? "MINE" : "WATCH");
        if (tag->ad.separated == 1) tl += put(x + tl, yy, w - tl, A(LAP_MAGENTA | LAP_BRIGHT), "AWAY ");
        if (tag->rotations) tl += put(x + tl, yy, w - tl, A(LAP_MAGENTA), "rot %u ", tag->rotations);
        put(x + tl, yy, w - tl, LAP_UI_FAINT, "here %um", (unsigned)(bw_span_ms(tag) / 60000u));
    } else if (tgt.target == BH_T_FREQ) {
        const char *rx = dual_on() ? "DUAL" : hunt_rx == HR_SDR ? sdr_name : hunt_rx == HR_LORA ? radio_name : "no receiver";
        put(x + tl, yy, w - tl, A(dual_on() ? (LAP_MAGENTA | LAP_BRIGHT) : LAP_GREEN), "%s", rx);
    }
    yy++;
    if (rx_note[0]) put(x, yy, w, A(LAP_UI_WARN), "%s", rx_note);
    yy++;
    /* the gauge, and what it says beside it */
    const int gw = w >= 26 ? 26 : w, gh = 8;
    gauge(x, yy, gw, gh, q, pk, fresh);
    put(x, yy + gh, 4, A(LAP_BLUE | LAP_BRIGHT), "%d", bh_range_lo(&rng) / 10);
    char hi_t[8];
    const int hl = snprintf(hi_t, sizeof(hi_t), "%d", bh_range_hi(&rng) / 10);
    put(x + gw - hl, yy + gh, hl, A(LAP_RED | LAP_BRIGHT), "%s", hi_t);
    {
        const char *zw = fresh ? zone_word(q) : lv.started ? "LOST IT" : "LISTEN";
        const int zl = (int)strlen(zw);
        put(x + (gw - zl) / 2, yy + gh, zl, A(fresh ? zone_hue(q) : LAP_BLUE), "%s", zw);
    }
    const int ix = x + gw + 2, iw = w - gw - 2;
    int iy = yy;
    if (iw >= 14) {
        const int now_lv = fresh ? (int)lv.fast / 10 : 0;
        put(ix, iy, 6, LAP_UI_FAINT, "LEVEL");
        if (fresh) put(ix + 6, iy, iw - 6, A(zone_hue(q)), "%d %s", now_lv, unit());
        else put(ix + 6, iy, iw - 6, A(LAP_BLUE), "-- %s", unit());
        iy++;
        put(ix, iy, 6, LAP_UI_FAINT, "PEAK");
        if (lv.peak != BH_HNONE) put(ix + 6, iy, iw - 6, A(LAP_RED | LAP_BRIGHT), "%d", lv.peak / 10);
        iy++;
        put(ix, iy, 6, LAP_UI_FAINT, "RANGE");
        put(ix + 6, iy, iw - 6, rng.step ? A(LAP_MAGENTA | LAP_BRIGHT) : LAP_UI_DIM, "%s%s", rng.step ? "+" : "", cfg.autorange ? (rng.step ? "" : "AUTO") : "FIXED");
        if (rng.step) put(ix + 7, iy, iw - 7, A(LAP_MAGENTA | LAP_BRIGHT), "%d dB up", rng.step * 20);
        iy++;
        if (tag) {
            put(ix, iy, 6, LAP_UI_FAINT, "AWAY");
            char dt[24];
            if (fresh) { bh_dist_text((int)lv.fast / 10, tag_ref(tag), dt, sizeof(dt)); put(ix + 6, iy, iw - 6, A(LAP_CYAN | LAP_BRIGHT), "%s", dt); }
            else put(ix + 6, iy, iw - 6, A(LAP_BLUE), "?");
            iy++;
            put(ix + 6, iy, iw - 6, cal_ref && cal_uid && bhw_dev(cal_uid) == tag ? A(LAP_GREEN | LAP_BRIGHT) : LAP_UI_FAINT, "%s",
                cal_ref && cal_uid && bhw_dev(cal_uid) == tag ? "CAL at 1 m" : "K: CAL at 1 m");
        } else {
            put(ix, iy, 6, LAP_UI_FAINT, "OVER");
            if (fresh) put(ix + 6, iy, iw - 6, A(LAP_CYAN | LAP_BRIGHT), "+%d dB quiet", ((int)lv.fast - hunt_floor) / 10 > 0 ? ((int)lv.fast - hunt_floor) / 10 : 0);
            iy++;
        }
        iy++;
        /* hotter / colder, with the arrows stepping the way it goes */
        const int tr = fresh ? bh_lvl_trend(&lv, 3) : 0;
        const char *word = !fresh ? (lv.started ? "LOST IT" : "LISTEN") : tr > 0 ? "HOTTER" : tr < 0 ? "COLDER" : "STEADY";
        const uint8_t wa = A(!fresh ? LAP_BLUE : tr > 0 ? (LAP_RED | LAP_BRIGHT) : tr < 0 ? (LAP_CYAN | LAP_BRIGHT) : LAP_WHITE);
        const int wl = put(ix, iy, iw, wa, "%s", word);
        if (tr) {
            const int ph2 = (int)((now_ms / 150u) % 3u);
            for (int k = 0; k < 3; k++) cell(ix + wl + 1 + k, iy, tr > 0 ? '^' : 'v', A(k == ph2 ? (LAP_WHITE | LAP_BRIGHT) : (tr > 0 ? LAP_RED : LAP_CYAN)));
        }
        iy++;
        /* the speaker throws a ring per tick */
        const uint32_t since = tick_ms ? now_ms - tick_ms : 9999u;
        const int ring = since < 80u ? 3 : since < 160u ? 2 : since < 240u ? 1 : 0;
        put(ix, iy, 2, A(LAP_WHITE | LAP_BRIGHT), "<|");
        for (int k = 0; k < 3; k++) cell(ix + 2 + k, iy, ')', A(k < ring ? (LAP_YELLOW | LAP_BRIGHT) : LAP_BLUE));
        if (!cfg.sound) put(ix + 6, iy, iw - 6, LAP_UI_FAINT, "muted");
        else if (hunt_tone()) put(ix + 6, iy, iw - 6, LAP_UI_FAINT, "%dHz tone", 300 + (int)(hunt_frac(lv.fast) * 1700.0f));
        else if (fresh) put(ix + 6, iy, iw - 6, LAP_UI_FAINT, "%u ticks/s", (unsigned)(1000u / bh_geiger_ms((int)lv.fast, bh_range_lo(&rng))));
    }
    yy += gh + 1;
    if (iw < 14) {
        /* no room beside the gauge: the readout in two lines under it */
        const int tr = fresh ? bh_lvl_trend(&lv, 3) : 0;
        int k = 0;
        if (fresh) k = put(x, yy, w, A(zone_hue(q)), "%d %s ", (int)lv.fast / 10, unit());
        if (lv.peak != BH_HNONE) put(x + k, yy, w - k, A(LAP_RED | LAP_BRIGHT), "peak %d", lv.peak / 10);
        yy++;
        if (tag && fresh) {
            char dt[24];
            bh_dist_text((int)lv.fast / 10, tag_ref(tag), dt, sizeof(dt));
            k = put(x, yy, w, A(LAP_CYAN | LAP_BRIGHT), "%s ", dt);
        } else k = 0;
        put(x + k, yy, w - k, A(!fresh ? LAP_BLUE : tr > 0 ? (LAP_RED | LAP_BRIGHT) : tr < 0 ? (LAP_CYAN | LAP_BRIGHT) : LAP_WHITE), "%s",
            !fresh ? "" : tr > 0 ? "HOTTER ^" : tr < 0 ? "COLDER v" : "STEADY");
        yy++;
    }
    yy++;
    /* the strip: history, bursts, DUAL */
    yy += hunt_strip(x, yy, w, y + hgt - yy);
    yy++;
    /* the SDR panel: under the strip in portrait, over the dial in
       landscape, leaving the dial 9 or 10 rows */
    if (sdr_hunt()) {
        if (!dcw) {
            int r = y + hgt - yy - 9;
            if (r > 7) r = 7;
            const int got = sdr_panel(x, yy, w, r, false);
            if (got) yy += got + 1;
        } else {
            int r = drh - 10;
            if (r > 6) r = 6;
            const int got = sdr_panel(dx, dyy, full_w - (dx - x), r, true);
            if (got) { dyy += got + 1; drh -= got + 1; }
        }
    }
    /* the dial: under it in portrait, beside it in landscape */
    if (!dcw) { dx = x; dyy = yy; drh = y + hgt - yy; dcw = 0; }
    const bh_dir_fix_t fx = bh_dir2_fix(&dir2, cfg.dmode);
    const int dial_room0 = full_w - (dx - x) - 20;      /* the words beside it take 20 */
    int rows;
    {
        int cw = 0, ch = 0;
        lap_cell_px(&cw, &ch);
        const float ax = cw > 0 && ch > 0 ? ((float)ch / 4.0f) / ((float)cw / 2.0f) : 0.85f;
        const int fit = (int)(((float)(dial_room0 - 3) / ax + 1.0f) / 2.0f) + 2;   /* rows a round dial of that width takes */
        rows = drh < fit ? drh : fit;
    }
    int dc;
    {
        int cw = 0, ch = 0;
        lap_cell_px(&cw, &ch);
        const float ax = cw > 0 && ch > 0 ? ((float)ch / 4.0f) / ((float)cw / 2.0f) : 0.85f;
        dc = (int)(((float)(rows - 2) * 2.0f - 1.0f) * ax) + 3;     /* round on the glass */
    }
    if (dc > dial_room0) dc = dial_room0;
    if (rows < 7 || dc < 12) {
        if (rows > 1) wrap(dx, dyy, wide && dx != x ? full_w - (dx - x) : full_w, rows, LAP_UI_DIM, "No room for the dial: walk and watch the meter.");
        return;
    }
    if (!have_compass) {
        wrap(dx, dyy, wide && dx != x ? full_w - (dx - x) : full_w, rows, LAP_UI_DIM,
             "No compass: walk and watch the meter. Your body blocks the signal, so turn slowly where you stand first.");
        return;
    }
    dial(dx, dyy, dc, rows, &fx, heading);
    dial_x0 = dx; dial_y0 = dyy; dial_x1 = dx + dc; dial_y1 = dyy + rows;
    /* what the dial says, in words, beside it */
    const int tx = dx + dc + 1, tw = full_w - (tx - x), tb = y + hgt;
    int ty2 = dyy;
    put(tx, ty2++, tw, LAP_UI_FAINT, "%s", cfg.dmode == BH_DM_BODY ? "BODY SHIELD" : "LOUDEST WAY");
    if (fx.bearing >= 0 && heading >= 0 && fx.contrast >= 20) {
        const int t = bh_turn((int)heading, fx.bearing);
        const uint8_t qa = A(fx.quality >= 3 ? (LAP_RED | LAP_BRIGHT) : fx.quality == 2 ? (LAP_YELLOW | LAP_BRIGHT) : LAP_WHITE);
        if (t >= -15 && t <= 15) put(tx, ty2++, tw, qa, "AHEAD");
        else if (t >= 160 || t <= -160) put(tx, ty2++, tw, qa, "BEHIND YOU");
        else put(tx, ty2++, tw, qa, "TURN %s %d deg", t > 0 ? "RIGHT" : "LEFT", t > 0 ? t : -t);
        put(tx, ty2++, tw, qa, "%s %+.1f dB", fx.quality >= 3 ? "SHARP" : fx.quality == 2 ? "BROAD" : "UNCLEAR", (double)fx.contrast / 10.0);
        put(tx, ty2++, tw, A(LAP_RED | LAP_BRIGHT), "it is %3d %s", fx.bearing, compass_word(fx.bearing));
    } else if (fx.covered >= BH_DIR2 * 2 / 3) {
        put(tx, ty2++, tw, A(LAP_WHITE), "NO CLEAR WAY");
        put(tx, ty2++, tw, LAP_UI_DIM, "all round alike");
        put(tx, ty2++, tw, LAP_UI_DIM, "(%+.1f dB)", (double)fx.contrast / 10.0);
    } else {
        put(tx, ty2++, tw, A(LAP_WHITE), "TURN ROUND");
        put(tx, ty2++, tw, LAP_UI_DIM, "to find out");
        ty2++;
    }
    if (heading >= 0) put(tx, ty2++, tw, LAP_UI_TEXT, "face %3d %s", (int)heading, compass_word((int)heading));
    {
        /* how much of the circle is done: a dot per 30 degrees */
        const int cov = fx.covered * 100 / BH_DIR2;
        const int k = put(tx, ty2, tw, LAP_UI_DIM, "ring %3d%% ", cov);
        for (int i = 0; i < 12 && k + i < tw; i++) {
            int seen = 0;
            for (int b = i * 3; b < i * 3 + 3; b++) seen += dir2.val[b] != BH_HNONE;
            cell(tx + k + i, ty2, seen ? '*' : '.', A(seen ? (LAP_GREEN | LAP_BRIGHT) : LAP_BLUE));
        }
        ty2++;
    }
    ty2++;
    const char *hint = fx.covered >= BH_DIR2 * 2 / 3 && fx.contrast < 20
        ? (cfg.dmode == BH_DM_PEAK ? "Try BODY: board on your chest, your body as the shield. Or walk 5 m and turn again." : "Walk 5 m and turn again, slower.")
        : fx.covered < BH_DIR2 * 2 / 3
        ? (cfg.dmode == BH_DM_BODY ? "Board flat on your chest, turn slowly all the way round." : "Hold it flat, turn slowly all the way round.")
        : tag && fresh && q >= 0.64f ? "Very close: sweep it slowly over bags, seats and shelves."
        : "Walk that way, tap the dial and turn again: it sharpens as you close in.";
    if (ty2 < tb) wrap(tx, ty2, tw, tb - ty2, LAP_UI_DIM, hint);
}

/* ---- keys ---- */
static void set_keys(void)
{
    static char v0[24], v1[24];
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
            const bw_dev_t *d = bhw_pick() ? bhw_dev(bhw_pick()) : NULL;
            keys[0] = (lap_btn_t){ "HUNT", d ? "THIS TAG" : NULL, 'h', false, !d, false };
            /* MINE never alerts; WATCH says when it is left behind */
            keys[2] = (lap_btn_t){ "MARK", !d ? NULL : d->mark == BW_MARK_MINE ? "MINE" : d->mark == BW_MARK_WATCH ? "WATCH" : "NONE", 'k',
                                   d && d->mark != 0, !d, false };
        }
        keys[1] = (lap_btn_t){ "VIEW", bhw_view_name(bhw_view()), 'v', false, false, false };
        break;
    default:
        if (sdr_hunt()) {
            /* the SDR's tools; SOUND (T) and DIRECTION (B) stay on the keyboard and in OPTIONS */
            mhz(tgt.khz, v0, sizeof(v0));
            keys[0] = (lap_btn_t){ "TUNE", v0, 'u', false, false, false };
            if (pk_khz) mhz(pk_khz, v1, sizeof(v1));
            keys[1] = (lap_btn_t){ "SNAP", pk_khz ? v1 : "NO PEAK", 'p', pk_khz && now_ms - pk_ms < 600u, !pk_khz || pk_khz == tgt.khz, false };
            keys[2] = (lap_btn_t){ "STOP", "HUNT", 'x', true, false, false };
        } else if (hunting) {
            keys[0] = (lap_btn_t){ "SOUND", !cfg.sound ? "OFF" : hunt_tone() ? "TONE" : "TICKS", 't', cfg.sound != 0, false, false };
            keys[1] = (lap_btn_t){ "DIRECTION", cfg.dmode == BH_DM_BODY ? "BODY" : "PEAK", 'b', cfg.dmode == BH_DM_BODY, false, false };
            keys[2] = (lap_btn_t){ "STOP", "HUNT", 'x', true, false, false };
        } else {
            keys[0] = (lap_btn_t){ "HUNT FREQ", "PICK", 'q', false, radio < 0 && sdr < 0, false };
            keys[1] = (lap_btn_t){ "RECEIVER", RX_NAMES[cfg.rx], 'e', cfg.rx != RX_LORA, false, false };
            keys[2] = (lap_btn_t){ "DIRECTION", cfg.dmode == BH_DM_BODY ? "BODY" : "PEAK", 'b', cfg.dmode == BH_DM_BODY, false, false };
        }
        break;
    }
    keys[3] = (lap_btn_t){ "OPTIONS", NULL, 'm', false, false, false };
}

/* ---- OPTIONS: one tree, a level at a time, BACK at the top of each ---- */
enum { MT_TOP, MT_SWEEP, MT_TAGS, MT_KINDS, MT_HUNT, MT_RX };
enum {
    MA_NONE, MA_BACK, MA_GO_SWEEP, MA_GO_TAGS, MA_GO_KINDS, MA_GO_HUNT, MA_GO_RX, MA_HELP,
    MA_REGION, MA_ROOM, MA_CAR, MA_HOP, MA_SOUND,
    MA_FOLLOW, MA_POSSIBLE, MA_WATCH, MA_MARKS, MA_LOG, MA_CLEAR,
    MA_TONE, MA_DMODE, MA_RANGE, MA_CAL, MA_FREQ,
    MA_RX, MA_SDRDIR, MA_FOUND,
    MA_SDRGAIN, MA_SDRSPAN, MA_TUNE, MA_HSOUND,
    MA_KIND0,                           /* + index into KINDS */
};
static const struct { uint8_t cls; const char *name; } KINDS[] = {
    { BW_C_FINDMY, "FIND MY/AIRTAG" }, { BW_C_TILE, "TILE" }, { BW_C_SMARTTAG, "SMARTTAG" },
    { BW_C_FMDN, "GOOGLE FIND HUB" }, { BW_C_DULT, "DULT ANY BRAND" }, { BW_C_CHIPOLO, "CHIPOLO" },
};
#define MROWS 12
static int mlevel;
static char mlabel[MROWS][32];
static int mact[MROWS], mrows;
static void mrow(int act, const char *fmt, ...)
{
    if (mrows >= MROWS) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(mlabel[mrows], sizeof(mlabel[0]), fmt, ap);
    va_end(ap);
    mact[mrows++] = act;
}
static void menu_open(int level)
{
    if (dlg) return;
    mlevel = level;
    mrows = 0;
    uint32_t fm = 0;
    bool pos = false, lg = false;
    bhw_get_config(&fm, &pos, &lg);
    switch (level) {
    case MT_SWEEP:
        mrow(MA_BACK, "< BACK");
        mrow(MA_REGION, "REGION: %s", REGION_NAMES[cfg.region]);
        mrow(MA_ROOM, "ROOM SWEEP: %u MIN", ROOM_MIN[cfg.room_i]);
        mrow(MA_CAR, "CAR SWEEP: %u MIN", CAR_MIN[cfg.car_i]);
        mrow(MA_HOP, "WI-FI HOP, DROP LINK: %s", cfg.hop ? "YES" : "NO");
        mrow(MA_SOUND, "SOUND: %s", cfg.sound ? "ON" : "OFF");
        break;
    case MT_TAGS: {
        int mine = 0, watching = 0, off = 0;
        bhw_marked(&mine, &watching);
        for (int i = 0; i < ROWS(KINDS); i++) off += (cfg.kind_off & bw_kind_bit(KINDS[i].cls)) != 0;
        mrow(MA_BACK, "< BACK");
        mrow(MA_FOLLOW, "FOLLOWING ALERT: %u MIN", (unsigned)(fm / 60000u));
        mrow(MA_POSSIBLE, "POSSIBLE TAGS: %s", pos ? "ALERT" : "LIST ONLY");
        mrow(MA_GO_KINDS, "TRACK KINDS: %s >", off ? "SOME" : "ALL");
        mrow(MA_WATCH, "WATCH: GONE AFTER %u MIN", cfg.watch_min);
        mrow(MA_MARKS, "MARKED: %d MINE %d WATCH", mine, watching);
        mrow(MA_LOG, "TAG LOG TO CARD: %s", bhw_log_on() ? "ON" : "OFF");
        mrow(MA_CLEAR, "CLEAR THE TAG LIST");
        break;
    }
    case MT_KINDS:
        mrow(MA_BACK, "< BACK");
        for (int i = 0; i < ROWS(KINDS); i++)
            mrow(MA_KIND0 + i, "%s: %s", KINDS[i].name, cfg.kind_off & bw_kind_bit(KINDS[i].cls) ? "LIST ONLY" : "ALERT");
        break;
    case MT_HUNT:
        mrow(MA_BACK, "< BACK");
        mrow(MA_FREQ, "HUNT A FREQUENCY...");
        mrow(MA_DMODE, "DIRECTION: %s", cfg.dmode == BH_DM_BODY ? "BODY SHIELD" : "PEAK");
        mrow(MA_RANGE, "METER RANGE: %s", cfg.autorange ? "AUTO" : "FIXED");
        mrow(MA_HSOUND, "SOUND: %s", cfg.sound ? "ON" : "OFF");
        mrow(MA_TONE, "HUNT SOUND: %s", TONE_NAMES[cfg.tone]);
        mrow(MA_CAL, "CAL: TAG IS 1 M AWAY NOW");
        if (sdr_hunt()) mrow(MA_TUNE, "TUNE, SNAP, GAIN (SDR)...");
        break;
    case MT_RX:
        mrow(MA_BACK, "< BACK");
        mrow(MA_RX, "RECEIVER: %s", RX_NAMES[cfg.rx]);
        mrow(MA_SDRDIR, "DIRECTIONAL ON: %s", cfg.sdr_dir ? "SDR" : "LORA");
        mrow(MA_FOUND, "LORA %.10s  SDR %.10s", radio >= 0 ? radio_name : "NONE", sdr >= 0 ? sdr_name : "NONE");
        {
            char g[16];
            sdr_gain_text(g, sizeof(g));
            mrow(MA_SDRGAIN, "SDR GAIN: %s", g);
        }
        mrow(MA_SDRSPAN, "SDR SPAN: %s", cfg.sdr_span ? "NARROW" : "WIDE");
        break;
    default:
        mrow(MA_GO_SWEEP, "SWEEP >");
        mrow(MA_GO_TAGS, "TAGS AND ALERTS >");
        mrow(MA_GO_HUNT, "HUNT >");
        mrow(MA_GO_RX, "RECEIVERS >");
        mrow(MA_HELP, "HOW IT WORKS");
        break;
    }
    const char *l[MROWS];
    for (int i = 0; i < mrows; i++) l[i] = mlabel[i];
    dlg = lap_choose(l, mrows);
    dlg_kind = 1;
}
static void more(void) { menu_open(MT_TOP); }

/* HUNT FREQ: a frequency from the list, or typed */
static const struct { uint32_t khz; const char *name; } PRESETS[] = {
    { 433920, "REMOTES, SENSORS" }, { 315000, "US CAR REMOTES" }, { 868300, "EU ISM" }, { 915000, "US ISM" }, { 446000, "PMR WALKIES" },
};
static char plabel[ROWS(PRESETS) + 2][32];
static uint32_t last_khz;
static void freq_pick(void)
{
    if (dlg) return;
    const char *l[ROWS(PRESETS) + 2];
    int n = 0;
    snprintf(plabel[n], 32, "TYPE A FREQUENCY (MHZ)"); l[n] = plabel[n]; n++;
    for (int i = 0; i < ROWS(PRESETS); i++) {
        char m[16];
        mhz(PRESETS[i].khz, m, sizeof(m));
        snprintf(plabel[n], 32, "%s  %s", m, PRESETS[i].name);
        l[n] = plabel[n]; n++;
    }
    if (last_khz) {
        char m[16];
        mhz(last_khz, m, sizeof(m));
        snprintf(plabel[n], 32, "%s  LAST TYPED", m);
        l[n] = plabel[n]; n++;
    }
    dlg = lap_choose(l, n);
    dlg_kind = 4;
}
/* TUNE (an SDR hunt): SNAP, steps of 1 and 10 kHz, a typed frequency, the
   gain and the span. It opens again after a step, showing where it is. */
static char tlabel[9][32];
static void tune_pick(void)
{
    if (dlg || !sdr_hunt()) return;
    const char *l[9];
    char m[16], g[16];
    int n = 0;
    mhz(tgt.khz, m, sizeof(m));
    if (pk_khz && pk_khz != tgt.khz) { char p[16]; mhz(pk_khz, p, sizeof(p)); snprintf(tlabel[n], 32, "SNAP TO PEAK %s", p); }
    else snprintf(tlabel[n], 32, pk_khz ? "SNAP: ON THE PEAK" : "SNAP: NO PEAK YET");
    n++;
    snprintf(tlabel[n++], 32, "+1 KHZ   (NOW %s)", m);
    snprintf(tlabel[n++], 32, "-1 KHZ");
    snprintf(tlabel[n++], 32, "+10 KHZ");
    snprintf(tlabel[n++], 32, "-10 KHZ");
    snprintf(tlabel[n++], 32, "TYPE A FREQUENCY (MHZ)");
    sdr_gain_text(g, sizeof(g));
    snprintf(tlabel[n++], 32, "SDR GAIN: %s", g);
    snprintf(tlabel[n++], 32, "SDR SPAN: %s", cfg.sdr_span ? "NARROW" : "WIDE");
    for (int i = 0; i < n; i++) l[i] = tlabel[i];
    dlg = lap_choose(l, n);
    dlg_kind = 6;
}
static bool tunable(uint64_t hz)
{
    if (sdr_tunes(hz)) return true;
    if (radio < 0) return false;
    lap_radio_info_t r[LAP_RADIOS_MAX];
    const int n = lap_radio_list(r, LAP_RADIOS_MAX);
    return radio < n && (!r[radio].max_hz || (hz >= r[radio].min_hz && hz <= r[radio].max_hz));
}
/* "433.92", "433,92" or "915" (MHz) to kHz; false when it is not a number */
static bool parse_mhz(const char *t, uint32_t *khz)
{
    while (*t == ' ') t++;
    uint32_t whole = 0, frac = 0, scale = 1000;
    int digits = 0;
    for (; *t >= '0' && *t <= '9'; t++, digits++) { whole = whole * 10u + (uint32_t)(*t - '0'); if (whole > 100000u) return false; }
    if (*t == '.' || *t == ',') {
        t++;
        for (; *t >= '0' && *t <= '9'; t++, digits++) if (scale > 1) { scale /= 10u; frac += (uint32_t)(*t - '0') * scale; }
    }
    while (*t == ' ') t++;
    if ((*t == 'M' || *t == 'm') && (t[1] == 'H' || t[1] == 'h') && (t[2] == 'Z' || t[2] == 'z')) t += 3;
    while (*t == ' ') t++;
    if (!digits || *t) return false;
    *khz = whole * 1000u + frac;
    return true;
}
static void freq_go(uint32_t khz, const char *what)
{
    char m[16];
    mhz(khz, m, sizeof(m));
    if (!tunable((uint64_t)khz * 1000u)) { say("No receiver here tunes %s MHz", m); return; }
    hunt_freq(khz, what);
}

static void dialogs(void)
{
    if (dlg <= 0) { dlg = 0; return; }
    char text[24] = "";
    int v = -1;
    const int r = lap_dialog_poll(dlg, text, sizeof(text), &v);
    if (!r) return;
    const int kind = dlg_kind;
    dlg = 0;
    if (r != 1) {
        if (kind == 1 && mlevel != MT_TOP) menu_open(mlevel == MT_KINDS ? MT_TAGS : MT_TOP);   /* cancel goes up a level */
        return;
    }
    if (kind == 2) {                        /* the link-drop question */
        cfg.hop = v == 1;
        cfg_save();
        if (wifi_h > 0) { lap_capture_stop(wifi_h); wifi_h = 0; }
        sources_stop();
        sources_start();
        say(cfg.hop ? "Wi-Fi hops 1-13 (link dropped)" : "Wi-Fi stays on the link's channel");
        return;
    }
    if (kind == 3) { menu_open(MT_TOP); return; }   /* help read: back to OPTIONS */
    if (kind == 4) {                        /* HUNT FREQ: the list */
        if (v == 0) { dlg = lap_text_input("Frequency in MHz, e.g. 433.92", "", 12, 0); dlg_kind = 5; return; }
        if (v >= 1 && v <= ROWS(PRESETS)) freq_go(PRESETS[v - 1].khz, PRESETS[v - 1].name);
        else if (v == ROWS(PRESETS) + 1 && last_khz) freq_go(last_khz, "LAST TYPED");
        return;
    }
    if (kind == 6) {                        /* TUNE */
        static const int16_t STEP[] = { 0, 1, -1, 10, -10 };
        if (v == 0) { hunt_snap(); return; }
        if (v >= 1 && v <= 4) hunt_tune_by(STEP[v]);
        else if (v == 5) {
            char m[16];
            mhz(tgt.khz, m, sizeof(m));
            dlg = lap_text_input("Hunt frequency in MHz", m, 12, 0);
            dlg_kind = 7;
            return;
        } else if (v == 6) sdr_gain_step();
        else if (v == 7) sdr_span_toggle();
        tune_pick();
        return;
    }
    if (kind == 7) {                        /* TUNE: typed */
        uint32_t k = 0;
        if (!parse_mhz(text, &k) || k < 1000u || k > 6000000u) { say("Not a frequency: \"%s\" (MHz, e.g. 314.98)", text); return; }
        hunt_retune(k);
        return;
    }
    if (kind == 5) {                        /* HUNT FREQ: typed */
        uint32_t k = 0;
        if (!parse_mhz(text, &k) || k < 1000u || k > 6000000u) { say("Not a frequency: \"%s\" (MHz, e.g. 433.92)", text); return; }
        last_khz = k;
        freq_go(last_khz, "TYPED");
        return;
    }
    if (v < 0 || v >= mrows) return;
    const int a = mact[v];
    int stay = mlevel;                      /* re-open the same level, showing the change */
    switch (a) {
    case MA_BACK: stay = mlevel == MT_KINDS ? MT_TAGS : MT_TOP; break;
    case MA_GO_SWEEP: stay = MT_SWEEP; break;
    case MA_GO_TAGS: stay = MT_TAGS; break;
    case MA_GO_KINDS: stay = MT_KINDS; break;
    case MA_GO_HUNT: stay = MT_HUNT; break;
    case MA_GO_RX: stay = MT_RX; break;
    case MA_HELP:
        dlg = lap_confirm("Sweeps tracker and bug bands, names Bluetooth tags and warns when one follows you or one of yours is left behind. "
                          "HUNT finds one: meter, dial, ticks. Receive only.");
        dlg_kind = 3;
        return;
    case MA_REGION:
        cfg.region = (uint8_t)(cfg.region % 3 + 1);
        if (state == ST_IDLE) bh_init(h, BH_ROOM, cfg.region, has24, 240000, now_ms);
        break;
    case MA_ROOM: cfg.room_i = (uint8_t)((cfg.room_i + 1) % 3); break;
    case MA_CAR: cfg.car_i = (uint8_t)((cfg.car_i + 1) % 3); break;
    case MA_HOP:
        if (cfg.hop) { cfg.hop = 0; cfg_save(); sources_stop(); sources_start(); say("Wi-Fi stays on the link's channel"); }
        else {
            dlg = lap_confirm("Hopping Wi-Fi channels drops the board's Wi-Fi link (LINK, remote screens, network apps) while BUGHUNT runs. Allow it?");
            dlg_kind = 2;
            return;
        }
        break;
    case MA_SOUND: cfg.sound = !cfg.sound; if (!cfg.sound) lap_tone_stop(); break;
    case MA_FOLLOW: {
        uint32_t fm = 0;
        bool pos = false, lg = false;
        bhw_get_config(&fm, &pos, &lg);
        if (fm >= 60u * 60000u) bhw_follow_step(-7); else bhw_follow_step(1);   /* up, then round to 2 min */
        break;
    }
    case MA_POSSIBLE: bhw_possible_toggle(); break;
    case MA_WATCH: {
        int i = 0;
        while (i < ROWS(WATCH_MIN) && WATCH_MIN[i] != cfg.watch_min) i++;
        cfg.watch_min = WATCH_MIN[(i + 1) % ROWS(WATCH_MIN)];
        bhw_tracking(cfg.kind_off, (uint32_t)cfg.watch_min * 60000u);
        break;
    }
    case MA_MARKS: say("Mark a tag on TAGS: MARK (or K) steps NONE, MINE, WATCH"); break;
    case MA_LOG: bhw_log_toggle(); break;
    case MA_CLEAR: bhw_clear(); alert_uid = 0; break;
    case MA_TONE:
        cfg.tone = (uint8_t)((cfg.tone + 1) % 3);
        lap_tone_stop();
        say("HUNT sound: %s", cfg.tone == TONE_AUTO ? "tone on a tag, ticks on a frequency" : cfg.tone == TONE_TICKS ? "ticks" : "a rising tone");
        break;
    case MA_DMODE: cfg.dmode = cfg.dmode == BH_DM_BODY ? BH_DM_PEAK : BH_DM_BODY; break;
    case MA_RANGE: cfg.autorange = !cfg.autorange; if (hunting) range_reset(); break;
    case MA_CAL: hunt_cal(); break;
    case MA_FREQ: freq_pick(); cfg_save(); return;
    case MA_RX:
        cfg.rx = (uint8_t)((cfg.rx + 1) % 3);
        if (cfg.rx != RX_LORA && sdr < 0) say("No SDR on USB yet: plug in an RTL-SDR or HackRF");
        if (hunting && tgt.target == BH_T_FREQ) hunt_begin(&tgt, hunt_from);
        break;
    case MA_SDRDIR: cfg.sdr_dir = !cfg.sdr_dir; break;
    case MA_FOUND: find_radio(); find_sdr(); break;
    case MA_SDRGAIN: sdr_gain_step(); break;
    case MA_SDRSPAN: sdr_span_toggle(); break;
    case MA_HSOUND: cfg.sound = !cfg.sound; if (!cfg.sound) lap_tone_stop(); break;
    case MA_TUNE: tune_pick(); return;
    default:
        if (a >= MA_KIND0 && a < MA_KIND0 + ROWS(KINDS)) {
            cfg.kind_off ^= bw_kind_bit(KINDS[a - MA_KIND0].cls);
            bhw_tracking(cfg.kind_off, (uint32_t)cfg.watch_min * 60000u);
        }
        break;
    }
    cfg_save();
    cfg_sync();
    menu_open(stay);
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
    bhl_open();                              /* device names off the card, when the file is there */
    bhw_config((uint32_t)cfg.follow_min * 60000u, cfg.possible != 0, cfg.log != 0);
    find_radio();
    find_sdr();
    bhw_tracking(cfg.kind_off, (uint32_t)cfg.watch_min * 60000u);
    sdr_h = 0;
    cal_ref = 0; cal_uid = 0;
    mlevel = MT_TOP;
    state = ST_IDLE;
    tab = TAB_SWEEP;
    hunting = false;
    hunt_from = TAB_FIND;
    alert_uid = 0;
    rf_h = ble_cap = ble_scan = wifi_h = 0;
    nf = sel = 0;
    dlg = 0;
    bh_dir2_reset(&dir2);
    if (h) bh_init(h, BH_ROOM, cfg.region, has24, 240000, lap_millis());
    sources_start();                         /* BT and Wi-Fi run from the start: the tag watch listens from now */
}
static void leave(void)
{
    if (cfg_dirty && cfg_job < 0) lap_save("cfg", &cfg, sizeof(cfg));     /* a change made just before closing */
    rf_stop();
    sdr_stop();
    sources_stop();
    lap_tone_stop();
    if (dlg > 0) lap_dialog_cancel(dlg);
    dlg = 0;
    bhw_close();
    bhl_close();
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
/* A WATCH tag went quiet (left behind, or taken) or came back. */
static void watch_event(int i, int ev)
{
    const bw_t *t = bhw_table();
    if (!t || i < 0 || i >= t->n) return;
    char label[32];
    bhw_label(&t->dev[i], label, sizeof(label));
    if (ev == BW_EV_GONE) {
        snprintf(alert_text, sizeof(alert_text), "%s not heard for %u min: left behind?", label, cfg.watch_min);
        lap_alert("Tag left behind", alert_text, LAP_ALERT_WARN);
        lap_haptic(LAP_HAPTIC_ALERT);
        alert_uid = t->dev[i].uid;
        alert_at = lap_millis();
    } else say("%s is back in range", label);
}
static bool alert_live(void) { return alert_uid && lap_millis() - alert_at < ALERT_SHOW_MS && bhw_dev(alert_uid); }

static void draw(void)
{
    now_ms = lap_millis();
    if (!h || !buf || !watch_ok) { lap_clear(LAP_ATTR(LAP_WHITE, LAP_BLACK)); lap_put_str(1, 1, "out of memory", LAP_ATTR(LAP_RED | LAP_BRIGHT, LAP_BLACK)); return; }
    if (radio < 0 && !(lap_frame() % 25)) find_radio();
    if (!hunting && !(lap_frame() % 50)) find_sdr();      /* an SDR plugged in while the app is open */
    dialogs();
    cfg_pump();
    bhl_pump();
    if (h && !(lap_frame() % 8)) {
        /* Wi-Fi stations: their makers from the name file; a camera maker counts as one */
        for (int i = 0; i < h->nsta; i++) {
            if (h->sta[i].maker[0]) continue;
            const char *mk = bhl_maker(h->sta[i].mac);
            if (mk) bh_sta_maker(h, i, mk, bhid_camera_maker(mk));
        }
    }
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
        int ev = 0;
        const int wi = bhw_watch_poll(&ev);
        if (wi >= 0) watch_event(wi, ev);
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
        if (tab == TAB_HUNT && sdr_hunt()) { sdr_gain_step(); break; }
        cfg.region = (uint8_t)(cfg.region % 3 + 1);
        cfg_save();
        if (state == ST_IDLE) bh_init(h, BH_ROOM, cfg.region, has24, 240000, now_ms);     /* the plan shown */
        say("Region %s", REGION_NAMES[cfg.region]);
        break;
    case 't': cfg.sound = !cfg.sound; if (!cfg.sound) lap_tone_stop(); cfg_save(); break;
    case 'h': if (tab == TAB_TAGS) hunt_tag(bhw_pick()); else hunt_start(sel); break;
    case 'n': if (nf) { sel = (sel + 1) % nf; sel_moved = true; } break;
    case 'd': bh_dir2_reset(&dir2); lv.peak = BH_HNONE; say("dial cleared: turn round again"); break;
    case 'b': cfg.dmode = cfg.dmode == BH_DM_BODY ? BH_DM_PEAK : BH_DM_BODY; cfg_save();
              say(cfg.dmode == BH_DM_BODY ? "BODY: board on your chest, the quiet way is your back to it" : "PEAK: the loud way is toward it"); break;
    case 'e':
        cfg.rx = (uint8_t)((cfg.rx + 1) % 3);
        cfg_save();
        if (cfg.rx != RX_LORA && sdr < 0) say("No SDR on USB yet: plug in an RTL-SDR or HackRF");
        else say("Receiver: %s", cfg.rx == RX_DUAL ? "DUAL, directional against reference" : cfg.rx == RX_SDR ? "the SDR" : "the LoRa radio");
        break;
    case 'q': freq_pick(); break;
    case 'k':
        if (tab == TAB_TAGS) { if (bhw_pick()) bhw_mark_cycle(bhw_pick()); }
        else hunt_cal();
        break;
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
    case 'p': if (tab == TAB_HUNT && sdr_hunt()) hunt_snap(); else bhw_possible_toggle(); break;
    case 'u': tune_pick(); break;
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
    if (tab == TAB_HUNT && sdr_hunt() && ch) {
        /* the SDR's keys: + - a kHz, < > (or , .) ten, G the gain */
        const int st = ch == '+' || ch == '=' ? 1 : ch == '-' || ch == '_' ? -1 : ch == '>' || ch == '.' ? 10 : ch == '<' || ch == ',' ? -10 : 0;
        if (st) { hunt_tune_by(st); return true; }
        if (ch == 'g' || ch == 'G') { act('g'); return true; }
    }
    /* off the bar during an SDR hunt, still on the keyboard */
    if (tab == TAB_HUNT && hunting && (ch == 't' || ch == 'b')) { act(ch); return true; }
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
    if (tab == TAB_HUNT && hunting && dial_x0 >= 0 && col >= dial_x0 && col < dial_x1 && row >= dial_y0 && row < dial_y1) {
        act('d');                                       /* the dial: turn round again from here */
        return true;
    }
    if (tab == TAB_SWEEP && state == ST_IDLE && row >= py && row < py + 9) {
        act(col < px + pw / 2 ? 'r' : 'c');              /* the room or the car picture */
        return true;
    }
    return true;
}

LAP_ICON_PX(".........99.........", "......99999999......", "....99....9...99....", "...99....22....99...",
            "...9....2f2f....9...", "..9..888.22..88..9..", "..9.....aa2a8....9..", "..9..88aaa2aa88..9..",
            ".99..88aaa2aa88..99.", ".99...aaaa2aaa...999", "..9..8aaaa2aaa8..9..", "..9..88aaa2aa88..9..",
            "..9....aaa2aa....9..", "...9.888aa2a.88.9...", "...99....a2....99...", "....99........99....",
            "......99999999......", ".........99.........");

LAP_APP(.id = "bughunt", .name = "BUGHUNT", .sub = "trackers, tags and bugs", .version = "1.4",
        .author = "LakeShark", .color = LAP_GREEN,
        .icon_art = { " .-'''-.  ", "/  \\ /  \\ ", "| -(@)- | ", "\\  / \\  / ", " '-...-'\\ ", "         \\" },
        .flags = LAP_CAP_RADIO_RX,
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
