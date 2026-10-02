/* CARRIER LEVEL: how strong one frequency is, read off the LoRa chip's
   instantaneous RSSI inside a narrow FSK receive session. The session is
   there only to tune the chip and hold it in receive; its sync word is one
   nothing sends, so it never stops to take a packet. */
#include "../ls_experiments.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "ls_lora.h"

#define MHZ_LO 150.0
#define MHZ_HI 1100.0
/* The bar's ends. */
#define BAR_LO_DBM (-130.0f)
#define BAR_HI_DBM (-30.0f)
#define BAR_CELLS  30

extern const ls_experiment_t exp_carrier;

/* What the next start tunes. One word, so the console or OPTIONS can set it
   while the worker reads it. */
static volatile uint32_t s_hz = 915000000u;

typedef struct { float lo, hi; double sum; uint32_t n; } span_t;

/* What the readout shows, written by the worker under s_lock. */
typedef struct {
    uint32_t hz;
    float now;
    bool have_now;
    span_t second;   /* the last whole second */
    span_t total;    /* since start           */
    uint32_t reads, errors;
} view_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR view_t s_view;

/* The worker's own: the second being gathered, and when to look at the
   FIFO next. */
static EXT_RAM_BSS_ATTR span_t s_acc;
static int64_t s_acc_t0, s_drain_t;

static void span_add(span_t *s, float v)
{
    if (!s->n || v < s->lo) s->lo = v;
    if (!s->n || v > s->hi) s->hi = v;
    s->sum += v;
    s->n++;
}

static bool carrier_start(char *why, size_t n)
{
    const uint32_t caps = ls_lora_caps();
    if (!(caps & LS_LORA_CAP_FSK)) { snprintf(why, n, "No FSK receiver on this chip"); return false; }
    if (!(caps & LS_LORA_CAP_RSSI_INST)) { snprintf(why, n, "This chip gives no RSSI reading"); return false; }
    const uint32_t hz = s_hz;
    const ls_fsk_cfg_t cfg = {
        .freq_hz = hz,
        .bitrate = 4800,
        .deviation_hz = 2400,
        .bandwidth_hz = ls_lora_fsk_bw_snap(12500),
        .sync_word = 0x9D3B6E15u,
        .payload_bytes = 8,
    };
    const esp_err_t err = ls_lora_fsk_begin(&cfg);
    if (err != ESP_OK) {
        snprintf(why, n, "%.4f MHz refused: %s", hz / 1e6, esp_err_to_name(err));
        return false;
    }
    const int64_t now = esp_timer_get_time();
    memset(&s_acc, 0, sizeof(s_acc));
    s_acc_t0 = s_drain_t = now;
    portENTER_CRITICAL(&s_lock);
    memset(&s_view, 0, sizeof(s_view));
    s_view.hz = hz;
    portEXIT_CRITICAL(&s_lock);
    return true;
}

static void carrier_stop(void) { ls_lora_fsk_end(); }

static void carrier_poll(void)
{
    const int64_t now = esp_timer_get_time();
    float dbm = 0;
    const esp_err_t err = ls_lora_rssi_inst(&dbm);
    if (err == ESP_OK) {
        span_add(&s_acc, dbm);
        portENTER_CRITICAL(&s_lock);
        s_view.now = dbm;
        s_view.have_now = true;
        span_add(&s_view.total, dbm);
        s_view.reads++;
        portEXIT_CRITICAL(&s_lock);
    } else {
        portENTER_CRITICAL(&s_lock);
        s_view.errors++;
        portEXIT_CRITICAL(&s_lock);
        /* Out of receive: listen again. */
        if (err == ESP_ERR_INVALID_STATE) ls_lora_fsk_receive();
    }
    /* Anything the demodulator did take is read and dropped, so a full FIFO
       never stops the receiver. */
    if (now - s_drain_t >= 100000) {
        uint8_t buf[8];
        float rssi;
        ls_lora_fsk_poll(buf, sizeof(buf), &rssi);
        s_drain_t = now;
    }
    if (now - s_acc_t0 >= 1000000) {
        portENTER_CRITICAL(&s_lock);
        s_view.second = s_acc;
        portEXIT_CRITICAL(&s_lock);
        memset(&s_acc, 0, sizeof(s_acc));
        s_acc_t0 = now;
    }
}

static void span_line(char *out, const char *label, const span_t *s)
{
    if (!s->n) snprintf(out, LS_EXP_LINE, "%-7s --", label);
    else snprintf(out, LS_EXP_LINE, "%-7s avg %6.1f min %6.1f max %6.1f", label,
                  s->sum / s->n, (double)s->lo, (double)s->hi);
}

static int carrier_lines(char (*out)[LS_EXP_LINE], int max)
{
    view_t v;
    portENTER_CRITICAL(&s_lock);
    v = s_view;
    portEXIT_CRITICAL(&s_lock);
    /* Before the first run, the frequency the next one will use. */
    const uint32_t hz = v.hz ? v.hz : s_hz;
    int n = 0;
    if (n < max) snprintf(out[n++], LS_EXP_LINE, "FREQ    %.4f MHz", hz / 1e6);
    if (n < max) {
        if (v.have_now) snprintf(out[n++], LS_EXP_LINE, "NOW     %6.1f dBm", (double)v.now);
        else snprintf(out[n++], LS_EXP_LINE, "NOW     no reading yet");
    }
    if (n < max) span_line(out[n++], "1 SEC", &v.second);
    if (n < max) span_line(out[n++], "START", &v.total);
    if (n < max) {
        float f = v.have_now ? (v.now - BAR_LO_DBM) / (BAR_HI_DBM - BAR_LO_DBM) : 0.0f;
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        const int fill = (int)(f * BAR_CELLS + 0.5f);
        char bar[BAR_CELLS + 1];
        for (int i = 0; i < BAR_CELLS; i++) bar[i] = i < fill ? '#' : '.';
        bar[BAR_CELLS] = 0;
        snprintf(out[n++], LS_EXP_LINE, "%.0f [%s] %.0f", (double)BAR_LO_DBM, bar, (double)BAR_HI_DBM);
    }
    if (n < max)
        snprintf(out[n++], LS_EXP_LINE, "READS   %lu  errors %lu",
                 (unsigned long)v.reads, (unsigned long)v.errors);
    return n;
}

static bool parse_mhz(const char *text, double *mhz)
{
    char *end = NULL;
    const double v = strtod(text, &end);
    if (end == text || *end || !(v >= MHZ_LO && v <= MHZ_HI)) return false;
    *mhz = v;
    return true;
}

static bool carrier_configure(int argc, char **argv, char *why, size_t n)
{
    double mhz;
    if (argc != 1 || !parse_mhz(argv[0], &mhz)) {
        snprintf(why, n, "one frequency, %.0f-%.0f MHz", MHZ_LO, MHZ_HI);
        return false;
    }
    s_hz = (uint32_t)(mhz * 1e6 + 0.5);
    return true;
}

/* OPTIONS: the frequency, applied at once to a running session. */
static double o_freq(const ls_opt_t *o) { (void)o; return s_hz / 1e6; }
static void o_set_freq(const ls_opt_t *o, double v)
{
    (void)o;
    s_hz = (uint32_t)(v * 1e6 + 0.5);
    if (ls_exp_running() == &exp_carrier) ls_exp_start(&exp_carrier);
}
static void o_show_freq(const ls_opt_t *o, char *out, size_t n) { (void)o; snprintf(out, n, "%.4f MHz", s_hz / 1e6); }

static const ls_opt_t OPTS[] = {
    { .label = "FREQUENCY", .kind = LS_OPT_NUMBER, .num = o_freq, .set_num = o_set_freq,
      .lo = MHZ_LO, .hi = MHZ_HI, .unit = "MHz, 150 to 1100", .show = o_show_freq },
};

const ls_experiment_t exp_carrier = {
    .id = "carrier",
    .name = "CARRIER LEVEL",
    .sub = "RSSI of one frequency, 150-1100 MHz",
    .maturity = LS_EXP_WORKS,
    .start = carrier_start,
    .stop = carrier_stop,
    .poll = carrier_poll,
    .lines = carrier_lines,
    .configure = carrier_configure,
    .opts = OPTS,
    .n_opts = (int)(sizeof(OPTS) / sizeof(OPTS[0])),
};
