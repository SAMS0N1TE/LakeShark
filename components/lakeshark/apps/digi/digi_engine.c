/* DIGITAL MODES runtime. See digi_engine.h. */
#include "digi_engine.h"
#include "digi_fft.h"
#include "digi_ftx.h"
#include "digi_msg.h"
#include "digi_wspr.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "ls_textpack.h"

#ifdef ESP_PLATFORM
#include <sys/stat.h>
#include <sys/time.h>
#include "esp_attr.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ls_gps.h"
#include "ls_sdcard.h"
#include "ls_task_reap.h"
#include "ls_time.h"
static const char *TAG = "digi";
static int64_t up_ms(void) { return esp_timer_get_time() / 1000; }
/* The time base and the audio anchor are a few words, shared with the
   producer's task: a spinlock. The view is kilobytes: a mutex. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
#define TLOCK()   portENTER_CRITICAL(&s_mux)
#define TUNLOCK() portEXIT_CRITICAL(&s_mux)
static SemaphoreHandle_t s_vmux;
static void LOCK(void)
{
    if (!s_vmux) s_vmux = xSemaphoreCreateMutex();
    if (s_vmux) xSemaphoreTake(s_vmux, portMAX_DELAY);
}
static void UNLOCK(void) { if (s_vmux) xSemaphoreGive(s_vmux); }
#else
#include "esp_timer.h"   /* the bench shim */
#define EXT_RAM_BSS_ATTR
static int64_t up_ms(void) { return esp_timer_get_time() / 1000; }
#define LOCK()   do {} while (0)
#define UNLOCK() do {} while (0)
#endif

#define RATE       12000
#define RING       (RATE * 8)          /* 8 s of 12 kHz audio            */
#define SPEC_N     1024                /* activity spectrum, 11.7 Hz bins */
#define DT_KEEP    32

/* ---- bands: standard dial frequencies (USB) --------------------------------- */

static const digi_band_t BANDS[] = {
    {"160m",   1840000,        0,   1836600},
    {"80m",    3573000,  3575000,   3568600},
    {"60m",    5357000,        0,   5287200},
    {"40m",    7074000,  7047500,   7038600},
    {"30m",   10136000, 10140000,  10138700},
    {"20m",   14074000, 14080000,  14095600},
    {"17m",   18100000, 18104000,  18104600},
    {"15m",   21074000, 21140000,  21094600},
    {"12m",   24915000, 24919000,  24924600},
    {"10m",   28074000, 28180000,  28124600},
    {"6m",    50313000, 50318000,  50293000},
    {"2m",   144174000,144170000, 144489000},
};
_Static_assert(sizeof(BANDS) / sizeof(BANDS[0]) == DIGI_BAND_COUNT, "band table and DIGI_BAND_COUNT");
const char *const digi_band_names[DIGI_BAND_COUNT] = {
    "160m", "80m", "60m", "40m", "30m", "20m", "17m", "15m", "12m", "10m", "6m", "2m",
};
int digi_band_count(void) { return DIGI_BAND_COUNT; }
const digi_band_t *digi_band(int i) { return i >= 0 && i < digi_band_count() ? &BANDS[i] : NULL; }
uint64_t digi_band_dial(int i, digi_mode_t m)
{
    const digi_band_t *b = digi_band(i);
    if (!b) return 0;
    return m == DIGI_MODE_FT8 ? b->ft8_hz : m == DIGI_MODE_FT4 ? b->ft4_hz : b->wspr_hz;
}

const char *digi_mode_name(digi_mode_t m)
{
    static const char *const N[] = {"FT8", "FT4", "WSPR"};
    return m < DIGI_MODE_COUNT ? N[m] : "?";
}
const char *digi_time_name(digi_time_src_t t)
{
    static const char *const N[] = {"NONE", "RTC", "NTP", "GPS"};
    return t <= DIGI_TIME_GPS ? N[t] : "?";
}

static int slot_ms_of(digi_mode_t m) { return m == DIGI_MODE_FT8 ? 15000 : m == DIGI_MODE_FT4 ? 7500 : 120000; }

/* ---- grid squares ------------------------------------------------------------- */

bool digi_grid_to_ll(const char *g, double *lat, double *lon)
{
    size_t n = strlen(g);
    if (n != 4 && n != 6) return false;
    int a = toupper((unsigned char)g[0]) - 'A', b = toupper((unsigned char)g[1]) - 'A';
    if (a < 0 || a > 17 || b < 0 || b > 17 || !isdigit((unsigned char)g[2]) || !isdigit((unsigned char)g[3]))
        return false;
    double lo = -180.0 + a * 20.0 + (g[2] - '0') * 2.0, la = -90.0 + b * 10.0 + (g[3] - '0');
    if (n == 6) {
        int c = toupper((unsigned char)g[4]) - 'A', d = toupper((unsigned char)g[5]) - 'A';
        if (c < 0 || c > 23 || d < 0 || d > 23) return false;
        lo += c * (2.0 / 24) + 1.0 / 24;
        la += d * (1.0 / 24) + 0.5 / 24;
    } else {
        lo += 1.0;
        la += 0.5;
    }
    *lat = la; *lon = lo;
    return true;
}

void digi_ll_to_grid(double lat, double lon, char out[7])
{
    double lo = lon + 180.0, la = lat + 90.0;
    lo = lo < 0 ? 0 : lo >= 360 ? 359.9999 : lo;
    la = la < 0 ? 0 : la >= 180 ? 179.9999 : la;
    out[0] = (char)('A' + (int)(lo / 20)); out[1] = (char)('A' + (int)(la / 10));
    out[2] = (char)('0' + (int)fmod(lo / 2, 10)); out[3] = (char)('0' + (int)fmod(la, 10));
    out[4] = (char)('a' + (int)(fmod(lo, 2) * 12)); out[5] = (char)('a' + (int)(fmod(la, 1) * 24));
    out[6] = 0;
}

/* Great-circle bearing and distance (haversine; ls_geo has the same in the
   TUI component, which this one cannot reach). */
void digi_bearing_range(double la0, double lo0, double la1, double lo1, float *brg, float *km)
{
    const double r = 3.141592653589793 / 180.0;
    double p0 = la0 * r, p1 = la1 * r, dl = (lo1 - lo0) * r;
    double a = sin((p1 - p0) / 2) * sin((p1 - p0) / 2) + cos(p0) * cos(p1) * sin(dl / 2) * sin(dl / 2);
    *km = (float)(6371.0 * 2 * atan2(sqrt(a), sqrt(1 - a)));
    double y = sin(dl) * cos(p1), x = cos(p0) * sin(p1) - sin(p0) * cos(p1) * cos(dl);
    double b = atan2(y, x) / r;
    *brg = (float)(b < 0 ? b + 360 : b);
}

/* ---- shared state ------------------------------------------------------------- */

static volatile bool s_want;            /* sink accepts audio              */
static const digi_tuner_t *s_tuner;
static digi_mode_t s_mode = DIGI_MODE_FT8;
static int s_band = 5;                  /* 20 m */
static float s_trim;
static int s_gps_lag = 250;
static bool s_logging = true;
static volatile bool s_test;

/* What the UI copies. Rows newest first. */
EXT_RAM_BSS_ATTR static digi_view_t s_view;
static uint32_t s_seq;

static void add_row(const digi_row_t *r)
{
    LOCK();
    int n = s_view.n < DIGI_VIEW_ROWS ? s_view.n + 1 : DIGI_VIEW_ROWS;
    memmove(&s_view.row[1], &s_view.row[0], sizeof(digi_row_t) * (size_t)(n - 1));
    s_view.row[0] = *r;
    s_view.row[0].seq = ++s_seq;
    s_view.row[0].at_ms = (uint32_t)up_ms();
    s_view.n = n;
    s_view.total++;
    UNLOCK();
}

void digi_host_add(const digi_row_t *r)
{
    add_row(r);
    if (r->at_ms) { LOCK(); s_view.row[0].at_ms = r->at_ms; UNLOCK(); }
}
void digi_host_set(const digi_view_t *v) { LOCK(); s_view = *v; s_seq = v->n ? v->row[0].seq : 0; UNLOCK(); }

void digi_clear(void)
{
    LOCK();
    s_view.n = 0;
    s_view.total = 0;
    s_view.slots = 0;
    memset(s_view.slot_hist, 0, sizeof(s_view.slot_hist));
    UNLOCK();
}

bool digi_sink_wanted(void) { return s_want; }
void digi_set_tuner(const digi_tuner_t *t) { s_tuner = t; }
digi_mode_t digi_mode(void) { return s_mode; }
int digi_band_index(void) { return s_band; }
float digi_time_trim(void) { return s_trim; }
void digi_set_time_trim(float s) { s_trim = s < -2 ? -2 : s > 2 ? 2 : s; }
int digi_gps_lag_ms(void) { return s_gps_lag; }
void digi_set_gps_lag_ms(int ms) { s_gps_lag = ms < 0 ? 0 : ms > 900 ? 900 : ms; }
bool digi_logging(void) { return s_logging; }
void digi_set_logging(bool on) { s_logging = on; }
bool digi_test_source(void) { return s_test; }

static void retune(void)
{
    uint64_t hz = digi_band_dial(s_band, s_mode);
    if (hz && s_tuner && s_tuner->tune) s_tuner->tune(hz, true);
    LOCK();
    s_view.dial_hz = hz;
    s_view.band = s_band;
    s_view.mode = s_mode;
    s_view.slot_ms = slot_ms_of(s_mode);
    UNLOCK();
}

void digi_set_band(int i)
{
    if (!digi_band(i)) return;
    s_band = i;
    /* A band without this mode keeps the mode and moves to FT8. */
    if (!digi_band_dial(i, s_mode)) s_mode = DIGI_MODE_FT8;
    retune();
}

void digi_view(digi_view_t *out)
{
    LOCK();
    *out = s_view;
    UNLOCK();
    out->trim_s = s_trim;
    out->logging = s_logging;
    for (int i = 0; i < 16 && s_tuner && s_tuner->name; ++i) {
        snprintf(out->tuner, sizeof(out->tuner), "%s", s_tuner->name());
        break;
    }
}

#ifndef ESP_PLATFORM
/* The simulator and the host tests drive the view directly. */
bool digi_start(void) { s_view.running = true; retune(); return true; }
void digi_stop(void) { s_view.running = false; }
bool digi_running(void) { return s_view.running; }
void digi_set_mode(digi_mode_t m) { if (m < DIGI_MODE_COUNT) { s_mode = m; retune(); } }
void digi_set_test_source(bool on) { s_test = on; s_view.source = on ? DIGI_SRC_TEST : DIGI_SRC_NONE; }
void digi_sink_push(const int16_t *pcm, int n, int rate_hz, uint64_t dial_hz)
{ (void)pcm; (void)n; (void)rate_hz; (void)dial_hz; }
void digi_console_register(void) {}
#else

/* ---- time ---------------------------------------------------------------------- */

/* UTC = tb_utc + (esp_timer - tb_us); refreshed by the worker each second. */
static int64_t s_tb_us, s_tb_utc;
static digi_time_src_t s_tb_src;
static bool s_have_pos;
static double s_lat, s_lon;
static TaskHandle_t s_test_task;

static void time_refresh(void)
{
    EXT_RAM_BSS_ATTR static ls_gps_state_t g;   /* worker-only; large */
    ls_gps_get(&g);
    int64_t now = esp_timer_get_time();
    int64_t us, utc;
    digi_time_src_t src;
    if (g.utc_s && now / 1000 - (int64_t)g.utc_ms < 3000) {
        /* RMC time is the whole second the fix belongs to; the sentence
           arrives after it by the receiver's output lag. */
        us = (int64_t)g.utc_ms * 1000;
        utc = (int64_t)g.utc_s * 1000 + s_gps_lag;
        src = DIGI_TIME_GPS;
    } else {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        us = now;
        utc = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
        int ls = ls_time_source();
        src = !ls_time_is_synced() ? DIGI_TIME_NONE : ls == 2 ? DIGI_TIME_NTP : DIGI_TIME_RTC;
    }
    bool pos = g.fix;
    TLOCK();
    s_tb_us = us; s_tb_utc = utc; s_tb_src = src;
    s_have_pos = pos;
    if (pos) { s_lat = g.lat_deg; s_lon = g.lon_deg; }
    TUNLOCK();
}

static int64_t utc_now_ms(void)
{
    int64_t now = esp_timer_get_time();
    TLOCK();
    int64_t v = s_tb_utc + (now - s_tb_us) / 1000;
    TUNLOCK();
    return v;
}

/* ---- the sink ------------------------------------------------------------------- */

EXT_RAM_BSS_ATTR static int16_t s_ring[RING];
static volatile uint32_t s_wr, s_rd;   /* absolute sample counts */
static volatile uint32_t s_drops;
static int64_t s_anchor_idx;           /* ring sample index ...            */
static double s_anchor_utc;            /* ... and its UTC in ms            */
static bool s_anchor_ok;
static int64_t s_last_push_ms;
static volatile uint64_t s_dial_in;
/* Resampler: integrate-and-dump down, linear up. */
static double s_rs_pos, s_rs_acc, s_rs_w;
static int16_t s_rs_prev;
static int s_rs_rate;

static inline void ring_put(int16_t v)
{
    uint32_t w = s_wr;
    if (w - s_rd >= RING) { s_drops++; return; }
    s_ring[w % RING] = v;
    s_wr = w + 1;
}

void digi_sink_push(const int16_t *pcm, int n, int rate_hz, uint64_t dial_hz)
{
    if (!s_want || !pcm || n <= 0 || rate_hz < 8000 || rate_hz > 48000) return;
    /* One producer at a time: while the test source runs, a radio is ignored. */
    if (s_test && xTaskGetCurrentTaskHandle() != s_test_task) return;
    if (!s_test) s_dial_in = dial_hz;
    if (rate_hz != s_rs_rate) { s_rs_rate = rate_hz; s_rs_pos = 0; s_rs_acc = 0; s_rs_w = 0; }
    const double step = (double)RATE / rate_hz;   /* output samples per input */
    uint32_t before = s_wr;
    if (rate_hz == RATE) {
        for (int i = 0; i < n; ++i) ring_put(pcm[i]);
    } else if (rate_hz > RATE) {
        for (int i = 0; i < n; ++i) {
            double next = s_rs_pos + step;
            if (next < 1.0) { s_rs_acc += pcm[i] * step; s_rs_w += step; s_rs_pos = next; continue; }
            double part = 1.0 - s_rs_pos;
            s_rs_acc += pcm[i] * part; s_rs_w += part;
            double v = s_rs_acc / (s_rs_w > 0 ? s_rs_w : 1);
            ring_put((int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v));
            s_rs_pos = next - 1.0;
            s_rs_acc = pcm[i] * s_rs_pos; s_rs_w = s_rs_pos;
        }
    } else {
        for (int i = 0; i < n; ++i) {
            while (s_rs_pos < 1.0) {
                double v = s_rs_prev + (pcm[i] - s_rs_prev) * s_rs_pos;
                ring_put((int16_t)v);
                s_rs_pos += 1.0 / step;
            }
            s_rs_pos -= 1.0;
            s_rs_prev = pcm[i];
        }
    }
    /* Anchor the newest sample to now; a slow loop absorbs jitter, a jump
       (a gap in the audio) re-anchors. */
    uint32_t w = s_wr;
    if (w == before) return;
    int64_t now = utc_now_ms();
    TLOCK();
    double predicted = s_anchor_utc + (double)((int64_t)w - s_anchor_idx) * 1000.0 / RATE;
    double err = (double)now - predicted;
    if (!s_anchor_ok || fabs(err) > 300.0) {
        s_anchor_idx = w; s_anchor_utc = (double)now; s_anchor_ok = true;
    } else {
        s_anchor_utc += err * 0.01;
    }
    TUNLOCK();
    s_last_push_ms = up_ms();
}

/* UTC (ms) of absolute ring sample `idx`. */
static double sample_utc(uint32_t idx)
{
    TLOCK();
    double v = s_anchor_utc + (double)((int64_t)idx - s_anchor_idx) * 1000.0 / RATE;
    TUNLOCK();
    return v + s_trim * 1000.0;
}

/* ---- log ------------------------------------------------------------------------ */

static void log_rows(const digi_row_t *rows, int n, uint64_t dial)
{
    if (!s_logging || n <= 0) return;
    if (!ls_sdcard_mounted()) { snprintf(s_view.log_err, sizeof(s_view.log_err), "no SD card"); return; }
    mkdir("/sdcard/lakeshark", 0775);
    mkdir("/sdcard/lakeshark/digi", 0775);
    time_t t = (time_t)rows[0].slot_utc;
    struct tm tm;
    gmtime_r(&t, &tm);
    char path[64];
    LS_SNPRINTF(t_digi_engine_sdcard_lakeshark_digi_04d, path, sizeof(path), "/sdcard/lakeshark/digi/%04d%02d%02d.csv",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    struct stat st;
    bool fresh = stat(path, &st) != 0;
    FILE *f = fopen(path, "a");
    if (!f) { snprintf(s_view.log_err, sizeof(s_view.log_err), "open failed"); return; }
    if (fresh) LS_FPUTS(t_digi_engine_utc_mode_dial_hz, "utc,mode,dial_hz,audio_hz,snr_db,dt_s,message,call,grid,km,bearing\n", f);
    for (int i = 0; i < n; ++i) {
        const digi_row_t *r = &rows[i];
        LS_FPRINTF(fmt_digi_engine_fmt, f, "%04d-%02d-%02dT%02d:%02d:%02dZ,%s,%llu,%.0f,%d,%.2f,\"%s\",%s,%s,",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                digi_mode_name(r->mode), (unsigned long long)dial, r->freq, r->snr, r->dt,
                r->text, r->call, r->grid);
        if (r->has_pos) fprintf(f, "%.0f,%.0f\n", r->km, r->bearing);
        else fprintf(f, ",\n");
    }
    fclose(f);
    s_view.log_err[0] = 0;
    s_view.logged += (uint32_t)n;
}

/* ---- worker --------------------------------------------------------------------- */

EXT_RAM_BSS_ATTR static digi_hash_t s_hash;
EXT_RAM_BSS_ATTR static digi_ftx_result_t s_fres[64];
EXT_RAM_BSS_ATTR static digi_wspr_result_t s_wres[24];
EXT_RAM_BSS_ATTR static digi_row_t s_batch[64];
EXT_RAM_BSS_ATTR static float s_dts[DT_KEEP];
static int s_ndt, s_dt_pos;
static TaskHandle_t s_task;
static volatile bool s_stop;
static SemaphoreHandle_t s_done;

static void fill_pos(digi_row_t *r)
{
    double la, lo;
    bool have;
    double my_la, my_lo;
    TLOCK();
    have = s_have_pos; my_la = s_lat; my_lo = s_lon;
    TUNLOCK();
    if (have && r->grid[0] && digi_grid_to_ll(r->grid, &la, &lo)) {
        digi_bearing_range(my_la, my_lo, la, lo, &r->bearing, &r->km);
        r->has_pos = true;
    }
}

static void note_dt(float dt)
{
    s_dts[s_dt_pos] = dt;
    s_dt_pos = (s_dt_pos + 1) % DT_KEEP;
    if (s_ndt < DT_KEEP) s_ndt++;
    float v[DT_KEEP];
    memcpy(v, s_dts, sizeof(float) * (size_t)s_ndt);
    for (int i = 1; i < s_ndt; ++i)
        for (int j = i; j > 0 && v[j] < v[j - 1]; --j) { float t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
    LOCK();
    s_view.clock_off_s = v[s_ndt / 2];
    s_view.clock_off_n = s_ndt;
    UNLOCK();
}

static void publish(digi_mode_t m, uint32_t slot_utc, int n, uint64_t dial, uint32_t cost_ms)
{
    int k = 0;
    for (int i = 0; i < n && k < 64; ++i) {
        digi_row_t *r = &s_batch[k++];
        memset(r, 0, sizeof(*r));
        r->slot_utc = slot_utc;
        r->mode = m;
        if (m == DIGI_MODE_WSPR) {
            const digi_wspr_result_t *w = &s_wres[i];
            r->snr = (int8_t)lroundf(w->snr_db);
            r->dt = w->dt_s; r->freq = w->freq_hz;
            snprintf(r->text, sizeof(r->text), "%s", w->text);
            snprintf(r->call, sizeof(r->call), "%s", w->call);
            if (w->type1) snprintf(r->grid, sizeof(r->grid), "%s", w->grid);
        } else {
            const digi_ftx_result_t *f = &s_fres[i];
            r->snr = (int8_t)lroundf(f->snr_db);
            r->dt = f->dt_s; r->freq = f->freq_hz;
            snprintf(r->text, sizeof(r->text), "%s", f->text);
            snprintf(r->call, sizeof(r->call), "%s", f->f.call_de);
            snprintf(r->to, sizeof(r->to), "%s", f->f.call_to);
            snprintf(r->grid, sizeof(r->grid), "%s", f->f.grid);
            r->cq = f->f.cq;
        }
        fill_pos(r);
        if (r->snr > -21) note_dt(r->dt);
    }
    /* Weakest first, so the strongest lands on top of the list. */
    for (int i = k - 1; i >= 0; --i) add_row(&s_batch[i]);
    LOCK();
    s_view.slots++;
    memmove(s_view.slot_hist, s_view.slot_hist + 1, DIGI_SLOT_HIST - 1);
    s_view.slot_hist[DIGI_SLOT_HIST - 1] = (uint8_t)(k > 255 ? 255 : k);
    s_view.last_decodes = k;
    s_view.last_cost_ms = cost_ms;
    UNLOCK();
    log_rows(s_batch, k, dial);
}

/* Activity spectrum of the newest audio, for the band view. */
static void activity(digi_fft_t *fft, digi_cpx *in, digi_cpx *out)
{
    uint32_t w = s_wr;
    if (w < SPEC_N) return;
    double ss = 0;
    for (int i = 0; i < SPEC_N; ++i) {
        float v = s_ring[(w - SPEC_N + (uint32_t)i) % RING] * (1.0f / 32768.0f);
        ss += (double)v * v;
        float h = 0.5f - 0.5f * cosf(6.2831853f * i / SPEC_N);
        in[i] = (digi_cpx){v * h, 0};
    }
    digi_fft_run(fft, in, out);
    float cell[DIGI_SPEC_CELLS];
    const float bin_hz = (float)RATE / SPEC_N;
    for (int c = 0; c < DIGI_SPEC_CELLS; ++c) {
        int b0 = (int)(c * 3000.0f / DIGI_SPEC_CELLS / bin_hz), b1 = (int)((c + 1) * 3000.0f / DIGI_SPEC_CELLS / bin_hz);
        float m = 0;
        for (int b = b0; b <= b1 && b < SPEC_N / 2; ++b) {
            float p = out[b].re * out[b].re + out[b].im * out[b].im;
            if (p > m) m = p;
        }
        cell[c] = 10.0f * log10f(m + 1e-20f);
    }
    float sorted[DIGI_SPEC_CELLS];
    memcpy(sorted, cell, sizeof(sorted));
    for (int i = 1; i < DIGI_SPEC_CELLS; ++i)
        for (int j = i; j > 0 && sorted[j] < sorted[j - 1]; --j) { float t = sorted[j]; sorted[j] = sorted[j - 1]; sorted[j - 1] = t; }
    float floor_db = sorted[DIGI_SPEC_CELLS / 3];
    LOCK();
    for (int c = 0; c < DIGI_SPEC_CELLS; ++c) {
        float v = (cell[c] - floor_db) * 8.0f + 24.0f;
        uint8_t u = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        s_view.spec[c] = (uint8_t)((s_view.spec[c] + 3 * u) / 4);
        s_view.spec_peak[c] = s_view.spec[c] > s_view.spec_peak[c] ? s_view.spec[c]
                              : (uint8_t)(s_view.spec_peak[c] > 3 ? s_view.spec_peak[c] - 3 : 0);
    }
    s_view.level_db = ss > 0 ? (float)(10.0 * log10(ss / SPEC_N)) : -120.0f;
    UNLOCK();
}

static void worker(void *arg)
{
    (void)arg;
    digi_ftx_dec_t *ftx = digi_alloc(sizeof(*ftx));
    digi_wspr_dec_t *wspr = digi_alloc(sizeof(*wspr));
    digi_fft_t afft;
    digi_cpx *ain = digi_alloc(sizeof(digi_cpx) * SPEC_N), *aout = digi_alloc(sizeof(digi_cpx) * SPEC_N);
    int16_t *chunk = digi_alloc(sizeof(int16_t) * 2048);
    bool ok = ftx && wspr && ain && aout && chunk && digi_fft_init(&afft, SPEC_N);
    digi_mode_t cur = (digi_mode_t)-1;
    bool capturing = false, dec_ok = false;
    int64_t slot_start = 0;
    long in_slot = 0, slot_len = 0;
    int64_t next_time = 0, next_spec = 0;
    while (ok && !s_stop) {
        int64_t now = up_ms();
        if (now >= next_time) { time_refresh(); next_time = now + 1000; }
        if (now >= next_spec) { activity(&afft, ain, aout); next_spec = now + 250; }
        digi_mode_t m = s_mode;
        if (m != cur) {
            /* A new mode: drop the old decoder, make the new one. */
            if (cur == DIGI_MODE_WSPR) digi_wspr_dec_free(wspr); else if ((int)cur >= 0) digi_ftx_dec_free(ftx);
            dec_ok = m == DIGI_MODE_WSPR ? digi_wspr_dec_init(wspr)
                                         : digi_ftx_dec_init(ftx, m == DIGI_MODE_FT8 ? DIGI_FT8 : DIGI_FT4, 100.0f, 3000.0f);
            if (!dec_ok) ESP_LOGE(TAG, "decoder allocation failed");
            cur = m;
            capturing = false;
            s_rd = s_wr;
        }
        int64_t utc = utc_now_ms();
        LOCK();
        s_view.utc_ms = utc;
        s_view.tsrc = s_tb_src;
        s_view.time_ok = s_tb_src != DIGI_TIME_NONE;
        s_view.slot_ms = slot_ms_of(cur);
        s_view.slot_pos_ms = (int)(((utc + (int64_t)(s_trim * 1000)) % s_view.slot_ms + s_view.slot_ms) % s_view.slot_ms);
        s_view.capturing = capturing;
        s_view.audio_age_ms = s_last_push_ms ? now - s_last_push_ms : -1;
        s_view.drops = s_drops;
        s_view.have_pos = s_have_pos;
        if (s_have_pos) digi_ll_to_grid(s_lat, s_lon, s_view.my_grid);
        if (s_dial_in && !s_test) s_view.dial_hz = s_dial_in;
        s_view.source = s_test ? DIGI_SRC_TEST : s_view.audio_age_ms >= 0 && s_view.audio_age_ms < 2000 ? DIGI_SRC_RADIO : DIGI_SRC_NONE;
        UNLOCK();
        uint32_t avail = s_wr - s_rd;
        if (!avail || !dec_ok) { vTaskDelay(pdMS_TO_TICKS(40)); continue; }
        if (avail > 2048) avail = 2048;
        uint32_t rd = s_rd;
        for (uint32_t i = 0; i < avail; ++i) chunk[i] = s_ring[(rd + i) % RING];
        double u0 = sample_utc(rd);
        uint32_t used = avail;
        if (!capturing) {
            /* Start at the next boundary inside this chunk (20 ms grace). */
            const int sm = slot_ms_of(cur);
            int64_t b = (int64_t)ceil((u0 - 20.0) / sm) * sm;
            double off = ((double)b - u0) * RATE / 1000.0;
            if (off < 0) off = 0;
            if (off < avail) {
                uint32_t k = (uint32_t)off;
                used = k;
                capturing = true;
                slot_start = b;
                in_slot = 0;
                slot_len = cur == DIGI_MODE_WSPR ? 116L * RATE : (long)sm * (RATE / 1000);
                if (cur == DIGI_MODE_WSPR) digi_wspr_dec_reset(wspr); else digi_ftx_dec_reset(ftx);
            }
        } else {
            long take = slot_len - in_slot;
            if ((long)used > take) used = (uint32_t)take;
            if (cur == DIGI_MODE_WSPR) digi_wspr_dec_feed_i16(wspr, chunk, (int)used);
            else digi_ftx_dec_feed_i16(ftx, chunk, (int)used);
            in_slot += used;
            if (in_slot >= slot_len) {
                LOCK(); s_view.decoding = true; UNLOCK();
                int64_t t0 = up_ms();
                int n = cur == DIGI_MODE_WSPR ? digi_wspr_dec_run(wspr, s_wres, 24)
                                              : digi_ftx_dec_run(ftx, s_fres, 64, &s_hash);
                uint32_t cost = (uint32_t)(up_ms() - t0);
                ESP_LOGI(TAG, "%s slot %lld: %d decodes in %u ms", digi_mode_name(cur),
                         (long long)(slot_start / 1000), n, (unsigned)cost);
                publish(cur, (uint32_t)(slot_start / 1000), n, s_view.dial_hz, cost);
                LOCK(); s_view.decoding = false; UNLOCK();
                capturing = false;
            }
        }
        s_rd = rd + used;
    }
    if ((int)cur >= 0) { if (cur == DIGI_MODE_WSPR) digi_wspr_dec_free(wspr); else digi_ftx_dec_free(ftx); }
    digi_fft_free(&afft);
    digi_free(ftx); digi_free(wspr); digi_free(ain); digi_free(aout); digi_free(chunk);
    LOCK(); s_view.running = false; s_view.capturing = false; s_view.decoding = false; UNLOCK();
    s_task = NULL;
    ls_task_retire_self();
}

/* ---- test source: synthetic traffic, streamed in real time ---------------------- */

static const char *const TEST_MSG[] = {
    "CQ K1ABC FN42", "CQ DX G4ABC IO91", "K1ABC W9XYZ EN37", "W9XYZ K1ABC -11",
    "K1ABC W9XYZ R-09", "W9XYZ K1ABC RR73", "CQ JA1XYZ PM95", "CQ VK2ABC QF56",
    "PY2XYZ EA4ABC IN80", "EA4ABC PY2XYZ -15", "CQ ZS6ABC KG33", "LU1ABC K1ABC 73",
};

static uint32_t rng_state = 12345;
static uint32_t rnd(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }
static float gauss(void)
{
    float u = ((float)(rnd() & 0xFFFF) + 1.0f) / 65537.0f, v = ((float)(rnd() & 0xFFFF) + 1.0f) / 65537.0f;
    return sqrtf(-2.0f * logf(u)) * cosf(6.2831853f * v);
}

static void test_task(void *arg)
{
    (void)arg;
    enum { CH = 240, TEST_SIGS = 6 };      /* 20 ms blocks */
    digi_ftx_tx_t *tx = digi_alloc(sizeof(digi_ftx_tx_t) * TEST_SIGS);
    float *buf = digi_alloc(sizeof(float) * CH);
    int16_t *pcm = digi_alloc(sizeof(int16_t) * CH);
    if (!tx || !buf || !pcm) goto out;
    int nsig = 0, nw = 0;
    int64_t slot = 0;
    /* WSPR beacons: symbols, tone-0 frequency, amplitude and phase. */
    static const char *const WCALL[] = {"K1ABC", "G4ABC", "VK2ABC", "JA1XYZ", "PY2XYZ"};
    static const char *const WGRID[] = {"FN42", "IO91", "QF56", "PM95", "GG66"};
    EXT_RAM_BSS_ATTR static uint8_t wsym[3][162];
    float wf[3] = {0}, wa[3] = {0};
    double wph[3] = {0};
    long sample = 0;                  /* samples since `slot` started */
    int64_t t_next = utc_now_ms();
    rng_state ^= (uint32_t)esp_timer_get_time();
    while (!s_stop && s_test) {
        const digi_mode_t m = s_mode;
        const int sm = slot_ms_of(m);
        int64_t now = utc_now_ms() + (int64_t)(s_trim * 1000);
        if (now < t_next) { vTaskDelay(pdMS_TO_TICKS(t_next - now > 20 ? 20 : t_next - now)); continue; }
        int64_t cur = now / sm * sm;
        if (cur != slot) {
            /* A new slot: pick this slot's traffic. */
            slot = cur;
            sample = (long)((now - cur) * (RATE / 1000));
            nsig = m == DIGI_MODE_WSPR ? 0 : 2 + (int)(rnd() % (TEST_SIGS - 1));
            nw = m == DIGI_MODE_WSPR ? 3 : 0;
            for (int i = 0; i < nw; ++i) {
                uint8_t d[7];
                int k = (int)(rnd() % 5);
                digi_wspr_pack(WCALL[k], WGRID[k], 23 + 7 * i, d);
                digi_wspr_encode(d, wsym[i]);
                wf[i] = 1420.0f + 50.0f * i + (float)(rnd() % 30);
                wa[i] = sqrtf(2.0f * 1e-4f * (2500.0f / 6000.0f) * powf(10.0f, (-14.0f - 4.0f * i) / 10.0f));
                wph[i] = 0;
            }
            for (int i = 0; i < nsig; ++i) {
                uint8_t p[10];
                digi_msg_pack(TEST_MSG[rnd() % (sizeof(TEST_MSG) / sizeof(TEST_MSG[0]))], p, NULL);
                float snr = -20.0f + (float)(rnd() % 22);
                float a = sqrtf(2.0f * 0.01f * 0.01f * (2500.0f / 6000.0f) * powf(10.0f, snr / 10.0f));
                float f = 300.0f + (float)(rnd() % 2400);
                long start = (long)((0.5f + ((float)(rnd() % 100) / 100.0f - 0.3f)) * RATE);
                digi_ftx_tx_begin(&tx[i], m == DIGI_MODE_FT8 ? DIGI_FT8 : DIGI_FT4, p, f, a, start);
            }
        }
        for (int i = 0; i < CH; ++i) buf[i] = 0.01f * gauss();
        for (int i = 0; i < nsig; ++i) digi_ftx_tx_add(&tx[i], buf, CH, sample);
        for (int i = 0; i < nw; ++i)
            for (int k = 0; k < CH; ++k) {
                long t = sample + k - RATE;             /* one second after the even minute */
                if (t < 0 || t >= 162L * DIGI_WSPR_SPS) continue;
                wph[i] += 6.283185307179586 * (wf[i] + DIGI_WSPR_TONE_HZ * wsym[i][t / DIGI_WSPR_SPS]) / RATE;
                buf[k] += wa[i] * (float)sin(wph[i]);
            }
        for (int i = 0; i < CH; ++i) {
            float v = buf[i] * 32768.0f;
            pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        digi_sink_push(pcm, CH, RATE, digi_band_dial(s_band, m));
        sample += CH;
        t_next += 20;
        if (now - t_next > 500) t_next = now;   /* fell behind: catch up, don't burst */
    }
out:
    digi_free(tx); digi_free(buf); digi_free(pcm);
    s_test_task = NULL;
    ls_task_retire_self();
}

/* ---- control -------------------------------------------------------------------- */

bool digi_running(void) { return s_task != NULL && !s_stop; }

bool digi_start(void)
{
    if (s_task) return !s_stop;   /* still winding down from a stop: try again */
    if (!s_done) s_done = xSemaphoreCreateBinary();
    s_stop = false;
    s_rd = s_wr;
    s_anchor_ok = false;
    time_refresh();
    if (xTaskCreatePinnedToCoreWithCaps(worker, "digi", 12288, NULL, 2, &s_task, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_task = NULL;
        return false;
    }
    s_want = true;
    LOCK(); s_view.running = true; UNLOCK();
    retune();
    if (s_test) digi_set_test_source(true);
    return true;
}

/* Never waits: the worker sees the flag between chunks (or after a decode
   in progress), frees its buffers and clears s_task itself. */
void digi_stop(void)
{
    if (!s_task) return;
    s_want = false;
    s_stop = true;
}

void digi_set_mode(digi_mode_t m)
{
    if (m >= DIGI_MODE_COUNT) return;
    s_mode = m;
    if (!digi_band_dial(s_band, m))
        for (int i = 0; i < digi_band_count(); ++i) if (digi_band_dial(i, m)) { s_band = i; break; }
    retune();
}

void digi_set_test_source(bool on)
{
    s_test = on;
    if (on && s_task && !s_test_task)
        if (xTaskCreatePinnedToCoreWithCaps(test_task, "digi_test", 6144, NULL, 3, &s_test_task, 1,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
            s_test_task = NULL;
}

/* ---- console -------------------------------------------------------------------- */

typedef struct { int kind; char path[96]; digi_mode_t mode; } job_t;
static job_t s_job;

/* Decode synthetic or recorded audio as one slot, off the console task. */
static void job_task(void *arg)
{
    (void)arg;
    const digi_mode_t m = s_job.mode;
    const int n = m == DIGI_MODE_WSPR ? 116 * RATE : slot_ms_of(m) * (RATE / 1000);
    int16_t *pcm = digi_alloc(sizeof(int16_t) * (size_t)n);
    int got = 0;
    if (!pcm) { LS_PRINTF(t_digi_engine_digi_no_psram_for, "digi: no PSRAM for %d samples\n", n); goto done; }
    if (s_job.kind == 0) {
        /* Synthetic slot: four FT8/FT4 signals or three WSPR beacons in noise. */
        float *f = digi_alloc(sizeof(float) * 4096);
        if (!f) goto done;
        const char *const M[] = {"CQ K1ABC FN42", "K1ABC W9XYZ -11", "W9XYZ K1ABC R-09", "CQ DX G4ABC IO91"};
        const float SNR[] = {-8, -12, -15, -18}, FR[] = {600, 1210, 1800, 2400};
        digi_ftx_tx_t tx[4];
        uint8_t wsym[3][162];
        const char *const WC[] = {"K1ABC", "G4ABC", "VK2ABC"}, *const WG[] = {"FN42", "IO91", "QF56"};
        double wph[3] = {0};
        for (int i = 0; i < 4 && m != DIGI_MODE_WSPR; ++i) {
            uint8_t p[10];
            digi_msg_pack(M[i], p, NULL);
            float a = sqrtf(2.0f * 1e-4f * (2500.0f / 6000.0f) * powf(10.0f, SNR[i] / 10.0f));
            digi_ftx_tx_begin(&tx[i], m == DIGI_MODE_FT8 ? DIGI_FT8 : DIGI_FT4, p, FR[i], a, RATE / 2 + i * 1200);
        }
        for (int i = 0; i < 3 && m == DIGI_MODE_WSPR; ++i) {
            uint8_t d[7];
            digi_wspr_pack(WC[i], WG[i], 37 - 7 * i, d);
            digi_wspr_encode(d, wsym[i]);
        }
        for (int at = 0; at < n; at += 4096) {
            int k = n - at < 4096 ? n - at : 4096;
            for (int i = 0; i < k; ++i) f[i] = 0.01f * gauss();
            if (m != DIGI_MODE_WSPR) {
                for (int s = 0; s < 4; ++s) digi_ftx_tx_add(&tx[s], f, k, at);
            } else {
                for (int s = 0; s < 3; ++s) {
                    float a = sqrtf(2.0f * 1e-4f * (2500.0f / 6000.0f) * powf(10.0f, (-18.0f - 3 * s) / 10.0f));
                    for (int i = 0; i < k; ++i) {
                        long t = at + i - RATE;
                        if (t < 0 || t >= 162L * DIGI_WSPR_SPS) continue;
                        wph[s] += 6.283185307179586 * (1450.0 + 60 * s + DIGI_WSPR_TONE_HZ * wsym[s][t / DIGI_WSPR_SPS]) / RATE;
                        f[i] += a * (float)sin(wph[s]);
                    }
                }
            }
            for (int i = 0; i < k; ++i) pcm[at + i] = (int16_t)(f[i] * 32768.0f);
            /* A slot is 180k samples of noise and tones: give IDLE1 its turn
               (without it the board logged a task watchdog for IDLE1). */
            vTaskDelay(1);
        }
        digi_free(f);
        got = n;
    } else {
        /* A 16-bit mono WAV; 12 kHz as WSJT-X saves it. */
        FILE *fp = fopen(s_job.path, "rb");
        if (!fp) { LS_PRINTF(t_digi_engine_digi_cannot_open_s, "digi: cannot open %s\n", s_job.path); goto done; }
        uint8_t h[44];
        int rate = 0;
        if (fread(h, 1, 12, fp) == 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) {
            while (fread(h, 1, 8, fp) == 8) {
                uint32_t len = h[4] | h[5] << 8 | h[6] << 16 | (uint32_t)h[7] << 24;
                if (!memcmp(h, "fmt ", 4)) {
                    uint8_t fm[16];
                    if (len < 16 || fread(fm, 1, 16, fp) != 16) break;
                    int ch = fm[2] | fm[3] << 8, bits = fm[14] | fm[15] << 8;
                    rate = (int)(fm[4] | fm[5] << 8 | fm[6] << 16 | (uint32_t)fm[7] << 24);
                    if (ch != 1 || bits != 16) { printf("digi: need 16-bit mono\n"); rate = 0; break; }
                    fseek(fp, (long)len - 16, SEEK_CUR);
                } else if (!memcmp(h, "data", 4)) {
                    break;
                } else fseek(fp, (long)len, SEEK_CUR);
            }
        }
        if (rate != RATE) { LS_PRINTF(t_digi_engine_digi_s_is_not, "digi: %s is not a 12 kHz 16-bit mono WAV\n", s_job.path); fclose(fp); goto done; }
        got = (int)fread(pcm, sizeof(int16_t), (size_t)n, fp);
        fclose(fp);
    }
    {
        int64_t t0 = up_ms();
        int nd = 0;
        if (m == DIGI_MODE_WSPR) {
            digi_wspr_dec_t *d = digi_alloc(sizeof(*d));
            if (d && digi_wspr_dec_init(d)) {
                digi_wspr_dec_reset(d);
                digi_wspr_dec_feed_i16(d, pcm, got);
                nd = digi_wspr_dec_run(d, s_wres, 24);
                for (int i = 0; i < nd; ++i)
                    LS_PRINTF(t_digi_engine_4_0f_5_1f, "  %+4.0f %+5.1f %7.1f %+4.1f  %s\n", s_wres[i].snr_db, s_wres[i].dt_s,
                           s_wres[i].freq_hz, s_wres[i].drift_hz, s_wres[i].text);
                digi_wspr_dec_free(d);
            }
            digi_free(d);
        } else {
            digi_ftx_dec_t *d = digi_alloc(sizeof(*d));
            if (d && digi_ftx_dec_init(d, m == DIGI_MODE_FT8 ? DIGI_FT8 : DIGI_FT4, 100, 3000)) {
                int64_t f0 = up_ms();
                digi_ftx_dec_reset(d);
                digi_ftx_dec_feed_i16(d, pcm, got);
                int64_t f1 = up_ms();
                nd = digi_ftx_dec_run(d, s_fres, 64, &s_hash);
                for (int i = 0; i < nd; ++i)
                    LS_PRINTF(t_digi_engine_4_0f_5_2f, "  %+4.0f %+5.2f %6.0f  %s\n", s_fres[i].snr_db, s_fres[i].dt_s, s_fres[i].freq_hz, s_fres[i].text);
                LS_PRINTF(fmt_digi_engine_digi_waterfall_ms_search, "digi: waterfall %lld ms, search+LDPC %u ms (%d candidates, %d LDPC)\n",
                       (long long)(f1 - f0), (unsigned)(d->last_us / 1000), d->last_candidates, d->last_ldpc);
                digi_ftx_dec_free(d);
            }
            digi_free(d);
        }
        LS_PRINTF(fmt_digi_engine_digi_decodes_from_samples, "digi: %s %d decodes from %d samples in %lld ms\n", digi_mode_name(m), nd, got,
               (long long)(up_ms() - t0));
    }
done:
    digi_free(pcm);
    xSemaphoreGive(s_done);
    ls_task_retire_self();
}

static digi_mode_t parse_mode(const char *s, digi_mode_t dflt)
{
    if (!s) return dflt;
    if (!strcasecmp(s, "ft8")) return DIGI_MODE_FT8;
    if (!strcasecmp(s, "ft4")) return DIGI_MODE_FT4;
    if (!strcasecmp(s, "wspr")) return DIGI_MODE_WSPR;
    return dflt;
}

static void print_status(void)
{
    EXT_RAM_BSS_ATTR static digi_view_t v;
    digi_view(&v);
    LS_PRINTF(t_digi_engine_digi_s_s_s, "digi: %s %s %s dial %.4f MHz, source %s\n", v.running ? "running" : "stopped",
           digi_mode_name(v.mode), digi_band(v.band) ? digi_band(v.band)->band : "?",
           v.dial_hz / 1e6, v.source == DIGI_SRC_TEST ? "TEST" : v.source == DIGI_SRC_RADIO ? "RADIO" : "none");
    LS_PRINTF(fmt_digi_engine_time_slot_ms_clock, "  time %s, slot %d/%d ms%s%s, clock offset %+.2f s (%d), trim %+.1f s, gps lag %d ms\n",
           digi_time_name(v.tsrc), v.slot_pos_ms, v.slot_ms, v.capturing ? " capturing" : "",
           v.decoding ? " decoding" : "", v.clock_off_s, v.clock_off_n, v.trim_s, s_gps_lag);
    LS_PRINTF(fmt_digi_engine_audio_dbfs_age_ms, "  audio %.1f dBFS, age %lld ms, drops %u; %u decodes in %u slots, last %d in %u ms\n",
           v.level_db, (long long)v.audio_age_ms, (unsigned)v.drops, (unsigned)v.total,
           (unsigned)v.slots, v.last_decodes, (unsigned)v.last_cost_ms);
    LS_PRINTF(t_digi_engine_log_s_u_lines, "  log %s, %u lines %s; position %s %s\n", v.logging ? "on" : "off", (unsigned)v.logged,
           v.log_err, v.have_pos ? "GPS" : "none", v.my_grid);
    for (int i = 0; i < v.n && i < 10; ++i) {
        LS_PRINTF(t_digi_engine_4d_5_1f_6, "  %+4d %+5.1f %6.0f %-24s", v.row[i].snr, v.row[i].dt, v.row[i].freq, v.row[i].text);
        if (v.row[i].has_pos) printf(" %5.0f km %3.0f deg", v.row[i].km, v.row[i].bearing);
        printf("\n");
    }
}

static int cmd_digi(int argc, char **argv)
{
    const char *a = argc > 1 ? argv[1] : "status";
    if (!strcmp(a, "status")) { print_status(); return 0; }
    if (!strcmp(a, "start")) {
        if (argc > 2) digi_set_mode(parse_mode(argv[2], s_mode));
        printf("digi: %s\n", digi_start() ? "started" : "could not start");
        return 0;
    }
    if (!strcmp(a, "stop")) { digi_stop(); printf("digi: stopped\n"); return 0; }
    if (!strcmp(a, "mode") && argc > 2) { digi_set_mode(parse_mode(argv[2], s_mode)); print_status(); return 0; }
    if (!strcmp(a, "band") && argc > 2) {
        for (int i = 0; i < digi_band_count(); ++i)
            if (!strcasecmp(argv[2], BANDS[i].band)) { digi_set_band(i); print_status(); return 0; }
        printf("digi: bands:");
        for (int i = 0; i < digi_band_count(); ++i) printf(" %s", BANDS[i].band);
        printf("\n");
        return 1;
    }
    if (!strcmp(a, "test")) { digi_set_test_source(argc > 2 ? !strcmp(argv[2], "on") : !s_test); LS_PRINTF(t_digi_engine_digi_test_source_s, "digi: test source %s\n", s_test ? "on" : "off"); return 0; }
    if (!strcmp(a, "log")) { digi_set_logging(argc > 2 ? !strcmp(argv[2], "on") : !s_logging); printf("digi: log %s\n", s_logging ? "on" : "off"); return 0; }
    if (!strcmp(a, "trim") && argc > 2) { digi_set_time_trim((float)atof(argv[2])); printf("digi: trim %+.2f s\n", s_trim); return 0; }
    if (!strcmp(a, "lag") && argc > 2) { digi_set_gps_lag_ms(atoi(argv[2])); LS_PRINTF(t_digi_engine_digi_gps_lag_d, "digi: gps lag %d ms\n", s_gps_lag); return 0; }
    if (!strcmp(a, "clear")) { digi_clear(); return 0; }
    if ((!strcmp(a, "selftest") || (!strcmp(a, "wav") && argc > 2))) {
        if (!s_done) s_done = xSemaphoreCreateBinary();
        if (s_task) { printf("digi: stop the receiver first (digi stop)\n"); return 1; }
        memset(&s_job, 0, sizeof(s_job));
        s_job.kind = !strcmp(a, "wav");
        if (s_job.kind) snprintf(s_job.path, sizeof(s_job.path), "%s", argv[2]);
        s_job.mode = parse_mode(argc > (s_job.kind ? 3 : 2) ? argv[s_job.kind ? 3 : 2] : NULL, DIGI_MODE_FT8);
        TaskHandle_t t;
        if (xTaskCreatePinnedToCoreWithCaps(job_task, "digi_job", 12288, NULL, 2, &t, 1,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            printf("digi: no task\n");
            return 1;
        }
        xSemaphoreTake(s_done, pdMS_TO_TICKS(60000));
        return 0;
    }
    printf("digi status | start [ft8|ft4|wspr] | stop | mode <m> | band <20m> | test [on|off]\n"
           "     selftest [ft8|ft4|wspr] | wav <file.wav> [ft8|ft4|wspr] | log [on|off] | trim <s> | lag <ms> | clear\n");
    return 1;
}

void digi_console_register(void)
{
    const esp_console_cmd_t c = {
        .command = "digi",
        .help = LS_TXT(con_digi_engine_digi_help, "Digital modes (FT8, FT4, WSPR): status, start/stop, band, test source, selftest decode, WAV decode"),
        .hint = LS_TXT(con_digi_engine_digi_hint, "[status|start|stop|mode|band|test|selftest|wav|log|trim|lag|clear]"),
        .func = &cmd_digi,
    };
    esp_console_cmd_register(&c);
}
#endif
