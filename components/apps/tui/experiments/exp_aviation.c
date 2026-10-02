/* AVIATION PULSES: receive-only OOK pulse candidates. Detector consumption
   and sample phase still need hardware confirmation; rates are observed
   captures, limited by FIFO service and time spent in each reply view. */
#include "../ls_experiments.h"
#include "aviation_decode.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "ls_lora.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern const ls_experiment_t exp_aviation;
static volatile int s_band = 1090;
static volatile uint32_t s_dme = 1000000000u;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define CLASSES 40
/* One second buckets make MIN a rolling 60 seconds, including this second. */
static EXT_RAM_BSS_ATTR struct {
    uint32_t buckets[60][CLASSES], rate[CLASSES], minute[CLASSES];
    uint32_t hz, errors;
    int band, slot, reply_view;
    float peak;
    bool have_peak;
} s;
static EXT_RAM_BSS_ATTR uint8_t raw[28], chips[30];
static int64_t tick, changed;
static bool radio_open;
#define PULSE_PATTERN      0x0006u   /* 0 1 1 0 */
#define PULSE_PATTERN_BITS 4
#define DME_PATTERN        0x00FEu   /* 0 then 7 x 1 */
#define DME_PATTERN_BITS   8
static esp_err_t tune(void)
{
    if (s.band == 1090 && !s.reply_view) {
        const ls_ook_cfg_t cfg = {.freq_hz = s.hz,
                                  .bitrate = 2000000,
                                  .bandwidth_hz = 3076923,
                                  .pattern = 0x0285,
                                  .pattern_bits = 16,
                                  .frame_bytes = 28,
                                  .gain_step = 13,
                                  .boost = 7};
        return ls_lora_ook_begin(&cfg);
    }
    /* Detector pattern bit zero is first: one quiet chip, then the pulse.
       The part takes a pattern only when its length is even and its first
       two chips differ (measured on the LR2021), so the edge opens it. SSR and reply
       pulses are ~2 chips (quiet, high, high, quiet); a DME pulse is 7.
       The chips it consumed after the quiet one are recreated in software
       so all timing is relative to the triggering pulse. Fixed gain 13, as
       Mode S runs: the part's AGC cannot settle inside a pulse. */
    const ls_ook_cfg_t cfg = {.freq_hz = s.hz,
                              .bitrate = 2000000,
                              .bandwidth_hz = 3076923,
                              .pattern = s.band == 0 ? DME_PATTERN : PULSE_PATTERN,
                              .pattern_bits = s.band == 0 ? DME_PATTERN_BITS : PULSE_PATTERN_BITS,
                              .frame_bytes = 12,
                              .gain_step = 13,
                              .boost = 7};
    return ls_lora_ook_begin(&cfg);
}
static bool start(char *why, size_t n)
{
    if (!(ls_lora_caps() & LS_LORA_CAP_MODES_RX)) {
        snprintf(why, n, "%s", LS_EXP_NEEDS_LR2021);
        return false;
    }
    int band = s_band;
    uint32_t hz = band ? (uint32_t)band * 1000000u : s_dme;
    portENTER_CRITICAL(&s_lock);
    memset(&s, 0, sizeof(s));
    s.band = band;
    s.hz = hz;
    portEXIT_CRITICAL(&s_lock);
    esp_err_t e = tune();
    radio_open = e == ESP_OK;
    if (!radio_open) {
        ls_lora_ook_end();
        snprintf(why, n, "OOK: %s", esp_err_to_name(e));
        return false;
    }
    tick = changed = esp_timer_get_time();
    return true;
}
static void stop(void)
{
    ls_lora_ook_end();
    radio_open = false;
}
static void record(int c, float rssi)
{
    portENTER_CRITICAL(&s_lock);
    s.buckets[s.slot][c]++;
    s.minute[c]++;
    if (isfinite(rssi) && (!s.have_peak || rssi > s.peak)) {
        s.peak = rssi;
        s.have_peak = true;
    }
    portEXIT_CRITICAL(&s_lock);
}
static void poll(void)
{
    int64_t now = esp_timer_get_time();
    if (radio_open)
        for (int drain = 0; drain < 8; drain++) {
            float rssi = NAN;
            int n = ls_lora_ook_poll(raw, sizeof(raw), &rssi);
            if (n < 0) {
                portENTER_CRITICAL(&s_lock);
                s.errors++;
                portEXIT_CRITICAL(&s_lock);
                break;
            }
            if (!n) break;
            if (s.band == 1090 && !s.reply_view) {
                int df = aviation_df(raw, n);
                record(df < 0 ? AV_UNKNOWN : 8 + df, rssi);
            } else {
                const uint16_t pat = s.band == 0 ? DME_PATTERN : PULSE_PATTERN;
                const int prefix = (s.band == 0 ? DME_PATTERN_BITS : PULSE_PATTERN_BITS) - 1;
                memset(chips, 0, sizeof(chips));
                for (int i = 0; i < prefix; i++)
                    if ((pat >> (i + 1)) & 1u) chips[i / 8] |= 1u << (7 - i % 8);
                for (int i = 0; i < n * 8; i++)
                    if ((raw[i / 8] >> (7 - i % 8)) & 1)
                        chips[(i + prefix) / 8] |= 1u << (7 - (i + prefix) % 8);
                record(aviation_classify(chips, (n * 8 + prefix) / 8, s.band), rssi);
            }
        }
    while (now - tick >= 1000000) {
        portENTER_CRITICAL(&s_lock);
        memcpy(s.rate, s.buckets[s.slot], sizeof(s.rate));
        s.slot = (s.slot + 1) % 60;
        for (int i = 0; i < CLASSES; i++) {
            s.minute[i] -= s.buckets[s.slot][i];
            s.buckets[s.slot][i] = 0;
        }
        portEXIT_CRITICAL(&s_lock);
        tick += 1000000;
    }
    if (s.band == 1090 && now - changed >= 5000000) {
        ls_lora_ook_end();
        portENTER_CRITICAL(&s_lock);
        s.reply_view = !s.reply_view;
        portEXIT_CRITICAL(&s_lock);
        radio_open = tune() == ESP_OK;
        changed = now;
        if (!radio_open) {
            portENTER_CRITICAL(&s_lock);
            s.errors++;
            portEXIT_CRITICAL(&s_lock);
        }
    }
}
static int lines(char (*out)[LS_EXP_LINE], int max)
{
    uint32_t rate[CLASSES], minute[CLASSES], hz, errors;
    int band, view;
    float peak;
    bool have;
    portENTER_CRITICAL(&s_lock);
    memcpy(rate, s.rate, sizeof(rate));
    memcpy(minute, s.minute, sizeof(minute));
    hz = s.hz;
    band = s.band;
    view = s.reply_view;
    errors = s.errors;
    peak = s.peak;
    have = s.have_peak;
    portEXIT_CRITICAL(&s_lock);
    if (!hz) {
        band = s_band;
        hz = band ? (uint32_t)band * 1000000u : s_dme;
    }
    int n = 0;
    if (n < max)
        snprintf(out[n++], LS_EXP_LINE, "%.3f MHz  %s", hz / 1e6,
                 band == 1030   ? "SSR"
                 : band == 1090 ? (view ? "A/C framing" : "Mode S DF")
                                : "DME/TACAN");
    if (n < max) snprintf(out[n++], LS_EXP_LINE, "CLASS     last sec   last minute captures");
    const char *names[] = {"unknown", "Mode A", "Mode C", "Mode S", "A/C reply", "X pair", "Y pair"};
    for (int i = 0; i < 7 && n < max; i++)
        if (i == 0 || (band == 1030 && i >= 1 && i <= 3) || (band == 1090 && i == 4) || (band == 0 && i >= 5))
            snprintf(out[n++], LS_EXP_LINE, "%-9s %8lu/s %12lu", names[i], (unsigned long)rate[i],
                     (unsigned long)minute[i]);
    if (band == 1090)
        for (int group = 0; group < 16 && n < max - 2; group++) {
            int a = 8 + group * 2, b = a + 1;
            if (minute[a] || minute[b] || group == 8)
                snprintf(out[n++], LS_EXP_LINE, "DF%02d %lu/s m%lu  DF%02d %lu/s m%lu", group * 2,
                         (unsigned long)rate[a], (unsigned long)minute[a], group * 2 + 1,
                         (unsigned long)rate[b], (unsigned long)minute[b]);
        }
    if (band == 1090 && n < max) {
        uint32_t sum = 0;
        for (int i = 8; i < 40; i++)
            sum += minute[i];
        snprintf(out[n++], LS_EXP_LINE, "DF last minute %lu (5s S / 5s A/C)", (unsigned long)sum);
    }
    if (n < max) {
        if (have)
            snprintf(out[n++], LS_EXP_LINE, "PEAK %.0f dBm  errors %lu", (double)peak, (unsigned long)errors);
        else
            snprintf(out[n++], LS_EXP_LINE, "PEAK -- dBm  errors %lu", (unsigned long)errors);
    }
    if (n < max)
        snprintf(out[n++], LS_EXP_LINE,
                 hz > 1100000000u ? "LF sensitivity above 1100 MHz unverified"
                                  : "Pulse candidates; FIFO/dead time limits rates");
    return n;
}
static bool configure(int argc, char **argv, char *why, size_t n)
{
    if (argc == 1 && (!strcmp(argv[0], "1030") || !strcmp(argv[0], "1090"))) {
        s_band = atoi(argv[0]);
        return true;
    }
    if (argc == 2 && !strcmp(argv[0], "dme")) {
        char *end;
        double v = strtod(argv[1], &end);
        if (end != argv[1] && !*end && v >= 962 && v <= 1213) {
            s_dme = (uint32_t)(v * 1e6 + 0.5);
            s_band = 0;
            return true;
        }
    }
    snprintf(why, n, "start 1030 | 1090 | dme MHz (962-1213)");
    return false;
}
static void restart(void)
{
    if (ls_exp_running() == &exp_aviation) ls_exp_start(&exp_aviation);
}
static int get_view(const ls_opt_t *o)
{
    (void)o;
    return s_band == 1030 ? 0 : s_band == 1090 ? 1 : 2;
}
static void set_view(const ls_opt_t *o, int v)
{
    (void)o;
    s_band = v == 0 ? 1030 : v == 1 ? 1090 : 0;
    restart();
}
static double freq(const ls_opt_t *o)
{
    (void)o;
    return s_dme / 1e6;
}
static void set_freq(const ls_opt_t *o, double v)
{
    (void)o;
    s_dme = (uint32_t)(v * 1e6 + 0.5);
    restart();
}
/* Local navaid assumptions: CON 76X (112.90 VOR), MHT 91X (114.40
   VOR), ENE 118X (117.10 VOR). X reply = interrogation +63 MHz for
   channels 64..126; ground replies above 1100 MHz have unverified LF sensitivity. */
static const uint32_t stations[] = {1000000000u, 1100000000u, 1163000000u, 1178000000u, 1205000000u};
static const char *const station_names[] = {
    "CUSTOM / 1000 MHz", "CON 76X aircraft 1100", "CON 76X ground 1163 (unverified)",
    "MHT 91X ground 1178 (unverified)", "ENE 118X ground 1205 (unverified)"};
static int get_station(const ls_opt_t *o)
{
    (void)o;
    for (int i = 1; i < 5; i++)
        if (s_dme == stations[i]) return i;
    return 0;
}
static void set_station(const ls_opt_t *o, int v)
{
    (void)o;
    if (v < 0 || v >= 5) return;
    s_dme = stations[v];
    s_band = 0;
    restart();
}
static const char *const views[] = {"1030 SSR", "1090 REPLIES", "DME/TACAN"};
static const ls_opt_t opts[] = {
    {.label = "VIEW", .kind = LS_OPT_CYCLE, .names = views, .n = 3, .get = get_view, .set = set_view},
    {.label = "DME PRESET",
     .kind = LS_OPT_CYCLE,
     .names = station_names,
     .n = 5,
     .get = get_station,
     .set = set_station},
    {.label = "DME FREQUENCY",
     .kind = LS_OPT_NUMBER,
     .num = freq,
     .set_num = set_freq,
     .lo = 962,
     .hi = 1213,
     .unit = "MHz; LF sensitivity >1100 unverified"},
};
const ls_experiment_t exp_aviation = {.id = "aviation",
                                      .name = "AVIATION PULSES",
                                      .sub = "1030 SSR, 1090 replies, DME pulse pairs",
                                      .maturity = LS_EXP_TRYING,
                                      .lr2021_only = true,
                                      .needs = "Air traffic; DME >1100 MHz unverified",
                                      .start = start,
                                      .stop = stop,
                                      .poll = poll,
                                      .lines = lines,
                                      .configure = configure,
                                      .opts = opts,
                                      .n_opts = 3};
