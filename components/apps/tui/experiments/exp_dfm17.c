/* RADIOSONDE: GRAW DFM-17 weather balloons (and DFM-09/06, which frame the
   same way) heard on the LR2021's FSK engine, 400-406 MHz. The National
   Weather Service launches them twice a day, near 00Z and 12Z, and each one
   transmits its GPS position continuously for the two hours or so it is up.

   The chip does the demodulation: 2500 bps over the Manchester chips, the
   frame header as a 32-chip sync word, and the 66 bytes behind it. Which
   way round the chip's FSK polarity is against the sonde's is not known, so
   until a frame decodes the session alternates the header and its inverse.
   dfm_decode.c turns the chips into telemetry on this worker.

   AUTO sweeps the band in 10 kHz steps, tunes to the strongest carrier
   above the floor, and gives up on it for a while if no frame comes. */
#include "../ls_experiments.h"
#include "../ls_geo.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dfm_decode.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "ls_gps.h"
#include "ls_lora.h"
#include "settings.h"

#define MHZ_LO 400.0
#define MHZ_HI 406.0
#define DEFAULT_HZ 403410000u

/* Tones about 3 kHz either side of the carrier at 2500 chips/s: some 9 kHz
   of signal. The sonde drifts a few kHz as it cools on the way up and the
   board's own crystal adds its share, so the default filter is the rung near
   24 kHz - room for about 7 kHz of combined offset either way - at roughly
   3 dB of sensitivity against the 12 kHz one, which only holds a carrier
   that is within 1.5 kHz. */
#define DEVIATION_HZ 3000u
static const uint32_t FILTER_HZ[] = { 12000u, 24000u, 36000u };
static const char *const FILTER_NAMES[] = { "12 kHz", "24 kHz", "36 kHz" };
#define N_FILTERS ((int)(sizeof(FILTER_HZ) / sizeof(FILTER_HZ[0])))

/* The sweep: 400.000 to 406.000 MHz, one bin every 10 kHz. */
#define SCAN_LO_HZ  400000000u
#define SCAN_HI_HZ  406000000u
#define SCAN_STEP   10000u
#define SCAN_BINS   ((int)((SCAN_HI_HZ - SCAN_LO_HZ) / SCAN_STEP) + 1)
/* A carrier this far over the band's median is worth listening to. */
#define SCAN_OVER_DB 8.0f

/* Timings, in microseconds. A frame is 224 ms and they come without a
   break, so each polarity gets about nine of them before the other is
   tried; every switch is a new session, so not much faster than this. */
#define POLARITY_US  2000000
/* A held polarity that hears nothing for this long is tried both ways again. */
#define LOST_US      10000000
/* AUTO: a carrier that gives no frame in this long is set aside... */
#define DWELL_US     8000000
/* ...for this long, and one that did give frames is left after this much
   silence, when the sonde has landed or gone out of range. */
#define TRIED_US     (300 * 1000000LL)
#define GONE_US      30000000
#define RSSI_US      100000
#define HOME_US      1000000

#define TRIED_MAX 8

extern const ls_experiment_t exp_dfm17;

/* What the next start uses, set from the console or OPTIONS. */
static volatile uint32_t s_hz = DEFAULT_HZ;
static volatile bool s_auto;
static volatile int s_filter = 1;

typedef enum { M_LISTEN, M_SCAN } rx_state_t;

/* What the readout shows, written by the worker under s_lock. */
typedef struct {
    bool running;
    rx_state_t mode;
    bool autoscan;
    uint32_t hz;
    bool inverted, held;

    /* The sweep. */
    uint32_t rows;
    uint32_t best_hz;
    float best_dbm, floor_dbm;
    bool have_row;
    uint32_t tried[TRIED_MAX];
    int n_tried;

    /* The carrier, read between frames. */
    bool have_carrier;
    float carrier_dbm;

    /* Frames. */
    uint32_t heard, good, blocks_bad, bits_fixed, chip_errors, retunes;
    bool have_frame_rssi;
    float frame_rssi;
    int64_t last_good_us, last_fix_us;

    dfm_report_t rep;

    bool have_home, home_live;
    double home_lat, home_lon;
} view_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR view_t s_view;

/* The worker's own. */
static EXT_RAM_BSS_ATTR dfm_t s_dfm;
static EXT_RAM_BSS_ATTR uint8_t s_chips[DFM_CHIP_BYTES];
static EXT_RAM_BSS_ATTR float s_dbm[SCAN_BINS];
static EXT_RAM_BSS_ATTR ls_gps_state_t s_gps;

typedef struct { uint32_t hz; int64_t until; } tried_t;
static EXT_RAM_BSS_ATTR tried_t s_tried[TRIED_MAX];

static rx_state_t s_mode;
static bool s_listening, s_scanning;
static uint32_t s_listen_hz;
static bool s_inv, s_held;
static int64_t s_pol_t, s_dwell_t, s_last_good, s_rssi_t, s_home_t;
static float s_rssi_sum;
static int s_rssi_n;
static bool s_auto_run;
static int s_filter_run;

/* --------------------------------------------------------- the radio -- */

static esp_err_t listen_on(uint32_t hz, bool inverted)
{
    if (s_listening) { ls_lora_fsk_end(); s_listening = false; }
    if (s_scanning) { ls_lora_scan_end(); s_scanning = false; }
    const ls_fsk_cfg_t cfg = {
        .freq_hz = hz,
        .bitrate = DFM_CHIP_RATE,
        .deviation_hz = DEVIATION_HZ,
        .bandwidth_hz = ls_lora_fsk_bw_snap(FILTER_HZ[s_filter_run]),
        .sync_word = inverted ? DFM_SYNC_INV : DFM_SYNC,
        .sync_bits = 32,
        .payload_bytes = DFM_CHIP_BYTES,
    };
    const esp_err_t err = ls_lora_fsk_begin(&cfg);
    if (err != ESP_OK) return err;
    s_listening = true;
    s_mode = M_LISTEN;
    s_listen_hz = hz;
    s_inv = inverted;
    s_pol_t = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    s_view.mode = M_LISTEN;
    s_view.hz = hz;
    s_view.inverted = inverted;
    s_view.held = s_held;
    s_view.retunes++;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

static esp_err_t scan_on(void)
{
    if (s_listening) { ls_lora_fsk_end(); s_listening = false; }
    const esp_err_t err = ls_lora_scan_begin(SCAN_LO_HZ, SCAN_HI_HZ);
    if (err != ESP_OK) return err;
    s_scanning = true;
    s_mode = M_SCAN;
    s_held = false;
    portENTER_CRITICAL(&s_lock);
    s_view.mode = M_SCAN;
    s_view.held = false;
    s_view.have_carrier = false;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

static void radio_off(void)
{
    if (s_listening) ls_lora_fsk_end();
    if (s_scanning) ls_lora_scan_end();
    s_listening = s_scanning = false;
}

/* ------------------------------------------------------------ the sweep -- */

static bool is_tried(uint32_t hz, int64_t now)
{
    for (int i = 0; i < TRIED_MAX; i++) {
        if (!s_tried[i].hz || s_tried[i].until <= now) continue;
        const uint32_t d = hz > s_tried[i].hz ? hz - s_tried[i].hz : s_tried[i].hz - hz;
        if (d <= 2 * SCAN_STEP) return true;
    }
    return false;
}

static void set_tried(uint32_t hz, int64_t now)
{
    int slot = 0;
    for (int i = 0; i < TRIED_MAX; i++) {
        if (!s_tried[i].hz || s_tried[i].until <= now) { slot = i; break; }
        if (s_tried[i].until < s_tried[slot].until) slot = i;
    }
    s_tried[slot].hz = hz;
    s_tried[slot].until = now + TRIED_US;
}

/* The band's median, from a histogram in whole dB: the floor a carrier has
   to stand clear of. */
static float band_floor(const float *dbm, int n)
{
    enum { LO = -160, HI = 0 };
    static EXT_RAM_BSS_ATTR uint16_t hist[HI - LO + 1];
    memset(hist, 0, sizeof(hist));
    for (int i = 0; i < n; i++) {
        int b = (int)lroundf(dbm[i]);
        if (b < LO) b = LO;
        if (b > HI) b = HI;
        hist[b - LO]++;
    }
    int seen = 0;
    for (int b = 0; b <= HI - LO; b++) {
        seen += hist[b];
        if (2 * seen >= n) return (float)(b + LO);
    }
    return (float)HI;
}

/* A whole row is in: pick the strongest carrier not set aside. True when
   there is one, in *hz. */
static bool scan_pick(int64_t now, uint32_t *hz)
{
    const float floor_dbm = band_floor(s_dbm, SCAN_BINS);
    int best = -1, any = 0;
    for (int i = 0; i < SCAN_BINS; i++) {
        if (s_dbm[i] > s_dbm[any]) any = i;
        const uint32_t f = SCAN_LO_HZ + (uint32_t)i * SCAN_STEP;
        if (s_dbm[i] < floor_dbm + SCAN_OVER_DB || is_tried(f, now)) continue;
        if (best < 0 || s_dbm[i] > s_dbm[best]) best = i;
    }
    portENTER_CRITICAL(&s_lock);
    s_view.rows++;
    s_view.have_row = true;
    s_view.floor_dbm = floor_dbm;
    s_view.best_hz = SCAN_LO_HZ + (uint32_t)any * SCAN_STEP;
    s_view.best_dbm = s_dbm[any];
    portEXIT_CRITICAL(&s_lock);
    if (best < 0) return false;
    *hz = SCAN_LO_HZ + (uint32_t)best * SCAN_STEP;
    return true;
}

static void publish_tried(int64_t now)
{
    portENTER_CRITICAL(&s_lock);
    s_view.n_tried = 0;
    for (int i = 0; i < TRIED_MAX; i++)
        if (s_tried[i].hz && s_tried[i].until > now) s_view.tried[s_view.n_tried++] = s_tried[i].hz;
    portEXIT_CRITICAL(&s_lock);
}

/* ------------------------------------------------------------ lifecycle -- */

static bool dfm17_start(char *why, size_t n)
{
    const uint32_t caps = ls_lora_caps();
    if (!(caps & LS_LORA_CAP_FSK)) { snprintf(why, n, "No FSK receiver on this chip"); return false; }
    s_auto_run = s_auto;
    s_filter_run = s_filter >= 0 && s_filter < N_FILTERS ? s_filter : 1;
    s_listening = s_scanning = false;
    s_held = false;
    dfm_init(&s_dfm);
    memset(s_tried, 0, sizeof(s_tried));
    const int64_t now = esp_timer_get_time();
    s_dwell_t = s_last_good = 0;
    s_rssi_t = s_home_t = 0;
    s_rssi_sum = 0;
    s_rssi_n = 0;
    portENTER_CRITICAL(&s_lock);
    memset(&s_view, 0, sizeof(s_view));
    s_view.running = true;
    s_view.autoscan = s_auto_run;
    s_view.hz = s_hz;
    s_view.rep = s_dfm.out;
    portEXIT_CRITICAL(&s_lock);

    esp_err_t err;
    if (s_auto_run) {
        err = scan_on();
        if (err != ESP_OK) {
            snprintf(why, n, "Sweep refused: %s", esp_err_to_name(err));
            return false;
        }
    } else {
        err = listen_on(s_hz, false);
        if (err != ESP_OK) {
            snprintf(why, n, "%.3f MHz refused: %s", s_hz / 1e6, esp_err_to_name(err));
            return false;
        }
        s_dwell_t = now;
    }
    return true;
}

static void dfm17_stop(void)
{
    radio_off();
    portENTER_CRITICAL(&s_lock);
    s_view.running = false;
    portEXIT_CRITICAL(&s_lock);
}

/* Where range and bearing are measured from: HOME if one is set, else a
   GPS fix less than ten seconds old. RAM reads only, so the worker can. */
static void refresh_home(void)
{
    float hlat, hlon;
    bool have = false, live = false;
    double lat = 0, lon = 0;
    if (settings_get_home(&hlat, &hlon) && fabsf(hlat) <= 85.0f && fabsf(hlon) <= 180.0f) {
        have = true;
        lat = hlat;
        lon = hlon;
    } else {
        ls_gps_get(&s_gps);
        const int64_t now = esp_timer_get_time();
        if (s_gps.fix && s_gps.last_fix_us && now >= s_gps.last_fix_us &&
            now - s_gps.last_fix_us <= 10000000 && isfinite(s_gps.lat_deg) && isfinite(s_gps.lon_deg)) {
            have = live = true;
            lat = s_gps.lat_deg;
            lon = s_gps.lon_deg;
        }
    }
    portENTER_CRITICAL(&s_lock);
    s_view.have_home = have;
    s_view.home_live = live;
    s_view.home_lat = lat;
    s_view.home_lon = lon;
    portEXIT_CRITICAL(&s_lock);
}

static void took_frame(float rssi, int64_t now)
{
    dfm_frame_info_t fi;
    const bool good = dfm_frame(&s_dfm, s_chips, s_inv, &fi);
    if (good) {
        s_last_good = now;
        s_held = true;
    }
    portENTER_CRITICAL(&s_lock);
    s_view.heard++;
    s_view.blocks_bad += (uint32_t)fi.blocks_bad;
    s_view.bits_fixed += (uint32_t)fi.corrected;
    s_view.chip_errors += (uint32_t)fi.violations;
    s_view.have_frame_rssi = true;
    s_view.frame_rssi = rssi;
    if (good) {
        s_view.good++;
        s_view.last_good_us = now;
        s_view.held = true;
    }
    if (fi.position) s_view.last_fix_us = now;
    s_view.rep = s_dfm.out;
    portEXIT_CRITICAL(&s_lock);
}

static void poll_scan(int64_t now)
{
    bool done = false;
    const int got = ls_lora_scan_pass(s_dbm, SCAN_BINS, &done);
    if (got == 0) {
        /* A pass that failed starts the sweep over. */
        ls_lora_scan_end();
        s_scanning = false;
        if (scan_on() != ESP_OK) s_dwell_t = now;
        return;
    }
    if (!done) return;
    uint32_t hz;
    if (scan_pick(now, &hz) && listen_on(hz, s_inv) == ESP_OK) {
        s_held = false;
        s_dwell_t = now;
        s_last_good = 0;
    }
}

static void poll_listen(int64_t now)
{
    float rssi = 0;
    const int got = ls_lora_fsk_poll(s_chips, sizeof(s_chips), &rssi);
    if (got == DFM_CHIP_BYTES) took_frame(rssi, now);

    if (now - s_rssi_t >= RSSI_US) {
        s_rssi_t = now;
        float dbm;
        const esp_err_t err = ls_lora_rssi_inst(&dbm);
        if (err == ESP_OK) {
            s_rssi_sum += dbm;
            s_rssi_n++;
            if (s_rssi_n >= 10) {
                portENTER_CRITICAL(&s_lock);
                s_view.have_carrier = true;
                s_view.carrier_dbm = s_rssi_sum / (float)s_rssi_n;
                portEXIT_CRITICAL(&s_lock);
                s_rssi_sum = 0;
                s_rssi_n = 0;
            }
        } else if (err == ESP_ERR_INVALID_STATE) {
            /* Out of receive: listen again. */
            ls_lora_fsk_receive();
        }
    }

    /* A polarity that has heard nothing for a while is no longer held. */
    if (s_held && now - s_last_good >= LOST_US) {
        s_held = false;
        s_pol_t = now;
        portENTER_CRITICAL(&s_lock);
        s_view.held = false;
        portEXIT_CRITICAL(&s_lock);
    }

    if (s_auto_run) {
        const bool never = s_last_good < s_dwell_t;
        if ((never && now - s_dwell_t >= DWELL_US) || (!never && now - s_last_good >= GONE_US)) {
            if (never) set_tried(s_listen_hz, now);
            publish_tried(now);
            scan_on();
            return;
        }
    }

    if (!s_held && now - s_pol_t >= POLARITY_US) listen_on(s_listen_hz, !s_inv);
}

static void dfm17_poll(void)
{
    const int64_t now = esp_timer_get_time();
    if (now - s_home_t >= HOME_US || !s_home_t) {
        s_home_t = now;
        refresh_home();
    }
    if (s_mode == M_SCAN) {
        if (s_scanning) poll_scan(now);
        else if (now - s_dwell_t >= 1000000) {
            /* The sweep would not start; try again once a second. */
            s_dwell_t = now;
            scan_on();
        }
    } else if (s_listening) {
        poll_listen(now);
    }
}

/* ------------------------------------------------------------ readout -- */

static void age(char *out, size_t n, int64_t us)
{
    const double s = us / 1e6;
    if (s < 60) snprintf(out, n, "%.1f s", s);
    else snprintf(out, n, "%dm%02ds", (int)(s / 60), (int)fmod(s, 60));
}

static int dfm17_lines(char (*out)[LS_EXP_LINE], int max)
{
    view_t v;
    portENTER_CRITICAL(&s_lock);
    v = s_view;
    portEXIT_CRITICAL(&s_lock);
    const int64_t now = esp_timer_get_time();
    const uint32_t hz = v.running ? v.hz : s_hz;
    const bool autoscan = v.running ? v.autoscan : s_auto;
    const dfm_report_t *r = &v.rep;
    int n = 0;
#define LINE(...) do { if (n < max) snprintf(out[n++], LS_EXP_LINE, __VA_ARGS__); } while (0)

    if (!v.running) {
        /* What START will do. */
        if (autoscan) LINE("FREQ    AUTO, sweeping %.0f-%.0f MHz", MHZ_LO, MHZ_HI);
        else LINE("FREQ    %.3f MHz", hz / 1e6);
        LINE("RADIO   %u bps  dev %.1f kHz  filter %s", (unsigned)DFM_CHIP_RATE, DEVIATION_HZ / 1e3,
             FILTER_NAMES[s_filter >= 0 && s_filter < N_FILTERS ? s_filter : 1]);
        return n;
    }

    /* A sonde heard this run: FOUND while listening to it, LOST once the
       sweep is looking for it again, with what it last said kept below. */
    const bool found = v.good > 0;
    if (found) {
        char id[16];
        if (r->serial) snprintf(id, sizeof(id), r->serial_hex ? "D%lX" : "D%lu", (unsigned long)r->serial);
        else snprintf(id, sizeof(id), "pending");
        LINE("%s %s  ID %s", r->type ? r->type : "DFM", v.mode == M_SCAN ? "LOST" : "FOUND", id);
    } else if (v.mode == M_SCAN) {
        LINE("scanning %.3f-%.3f MHz", SCAN_LO_HZ / 1e6, SCAN_HI_HZ / 1e6);
    } else {
        LINE("listening %.3f MHz", hz / 1e6);
    }

    if (v.mode == M_SCAN) {
        if (v.have_row) {
            LINE("SCAN    row %lu  floor %.0f dBm", (unsigned long)v.rows, (double)v.floor_dbm);
            LINE("BEST    %.3f MHz  %.0f dBm", v.best_hz / 1e6, (double)v.best_dbm);
        } else {
            LINE("SCAN    first row, 10 kHz steps");
        }
    } else {
        LINE("FREQ    %.3f MHz  %s  pol %c %s", hz / 1e6, autoscan ? "AUTO" : "FIXED",
             v.inverted ? '-' : '+', v.held ? "held" : "trying");
        char fr[10], ca[10];
        if (v.have_frame_rssi) snprintf(fr, sizeof(fr), "%.1f", (double)v.frame_rssi);
        else snprintf(fr, sizeof(fr), "--");
        if (v.have_carrier) snprintf(ca, sizeof(ca), "%.1f", (double)v.carrier_dbm);
        else snprintf(ca, sizeof(ca), "--");
        LINE("RSSI    %s dBm frame  %s carrier", fr, ca);
    }

    if (found) {
        if (r->have_fix) {
            LINE("ALT     %.0f m   ASC %+.1f m/s", (double)r->alt_m, (double)r->vel_v);
            LINE("POS     %.5f %c  %.5f %c", fabs(r->lat), r->lat < 0 ? 'S' : 'N',
                 fabs(r->lon), r->lon < 0 ? 'W' : 'E');
            LINE("SPEED   %.1f m/s  HDG %03.0f", (double)r->vel_h, (double)r->heading);
            if (v.have_home) {
                double brg = 0, rng = 0;
                ls_geo_bearing_range(v.home_lat, v.home_lon, r->lat, r->lon, &brg, &rng);
                LINE("RANGE   %.1f km  BRG %03.0f %s  %s", rng / 1000.0, brg, ls_geo_compass(brg),
                     v.home_live ? "gps" : "home");
            } else {
                LINE("RANGE   no HOME and no GPS fix");
            }
            LINE("TIME    %04u-%02u-%02u %02u:%02u:%02.0fZ  sats %u", (unsigned)r->year,
                 (unsigned)r->month, (unsigned)r->day, (unsigned)r->hour, (unsigned)r->minute,
                 floor((double)r->sec), (unsigned)r->sats);
        } else {
            LINE("POS     waiting for a whole GPS cycle");
        }
        if (r->have_batt || r->have_temp) {
            char b[20] = "", t[20] = "";
            if (r->have_batt) snprintf(b, sizeof(b), "BATT %.2f V  ", (double)r->batt_v);
            if (r->have_temp) snprintf(t, sizeof(t), "TEMP %.1f C", (double)r->temp_c);
            LINE("SENSOR  %s%s", b, t);
        }
    }

    if (v.heard || v.mode == M_LISTEN) {
        LINE("FRAMES  %lu heard  %lu good  %lu bad blocks", (unsigned long)v.heard,
             (unsigned long)v.good, (unsigned long)v.blocks_bad);
        LINE("ECC     %lu bits fixed  %lu chip errors", (unsigned long)v.bits_fixed,
             (unsigned long)v.chip_errors);
        if (found) {
            char a[12], f[12] = "--";
            age(a, sizeof(a), now - v.last_good_us);
            if (v.last_fix_us) age(f, sizeof(f), now - v.last_fix_us);
            LINE("SEEN    frame %.8s ago  fix %.8s ago", a, f);
        }
    }
    if (v.n_tried) {
        char list[LS_EXP_LINE - 8] = "";
        size_t at = 0;
        for (int i = 0; i < v.n_tried && i < 5 && at + 9 < sizeof(list); i++)
            at += (size_t)snprintf(list + at, sizeof(list) - at, " %.3f", v.tried[i] / 1e6);
        LINE("QUIET  %s", list);
    }
#undef LINE
    return n;
}

/* ------------------------------------------------------------ settings -- */

static bool parse_mhz(const char *text, double *mhz)
{
    char *end = NULL;
    const double v = strtod(text, &end);
    if (end == text || *end || !(v >= MHZ_LO && v <= MHZ_HI)) return false;
    *mhz = v;
    return true;
}

static bool dfm17_configure(int argc, char **argv, char *why, size_t n)
{
    double mhz;
    if (argc == 1 && !strcmp(argv[0], "auto")) {
        s_auto = true;
        return true;
    }
    if (argc != 1 || !parse_mhz(argv[0], &mhz)) {
        snprintf(why, n, "a frequency, %.0f-%.0f MHz, or auto", MHZ_LO, MHZ_HI);
        return false;
    }
    s_hz = (uint32_t)(mhz * 1e6 + 0.5);
    s_auto = false;
    return true;
}

static void restart_if_running(void)
{
    if (ls_exp_running() == &exp_dfm17) ls_exp_start(&exp_dfm17);
}

static double o_freq(const ls_opt_t *o) { (void)o; return s_hz / 1e6; }
static void o_set_freq(const ls_opt_t *o, double v)
{
    (void)o;
    s_hz = (uint32_t)(v * 1e6 + 0.5);
    s_auto = false;
    restart_if_running();
}
static void o_show_freq(const ls_opt_t *o, char *out, size_t n)
{
    (void)o;
    snprintf(out, n, "%.3f MHz", s_hz / 1e6);
}

static int o_auto(const ls_opt_t *o) { (void)o; return s_auto ? 1 : 0; }
static void o_set_auto(const ls_opt_t *o, int v)
{
    (void)o;
    s_auto = v != 0;
    restart_if_running();
}

static int o_filter(const ls_opt_t *o) { (void)o; return s_filter; }
static void o_set_filter(const ls_opt_t *o, int v)
{
    (void)o;
    s_filter = v >= 0 && v < N_FILTERS ? v : 1;
    restart_if_running();
}

static const ls_opt_t OPTS[] = {
    { .label = "FREQUENCY", .kind = LS_OPT_NUMBER, .num = o_freq, .set_num = o_set_freq,
      .lo = MHZ_LO, .hi = MHZ_HI, .unit = "MHz, 400 to 406", .show = o_show_freq },
    { .label = "AUTO SWEEP", .kind = LS_OPT_TOGGLE, .get = o_auto, .set = o_set_auto },
    { .label = "FILTER", .kind = LS_OPT_CYCLE, .names = FILTER_NAMES, .n = N_FILTERS,
      .get = o_filter, .set = o_set_filter },
};

const ls_experiment_t exp_dfm17 = {
    .id = "dfm17",
    .name = "RADIOSONDE",
    .sub = "DFM-17 balloons, 400-406 MHz",
    .maturity = LS_EXP_TRYING,
    .needs = "a sonde aloft: launches near 00Z and 12Z",
    .start = dfm17_start,
    .stop = dfm17_stop,
    .poll = dfm17_poll,
    .lines = dfm17_lines,
    .configure = dfm17_configure,
    .opts = OPTS,
    .n_opts = (int)(sizeof(OPTS) / sizeof(OPTS[0])),
};
