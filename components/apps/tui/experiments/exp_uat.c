/* UAT 978: one hardware sync at a time. Equal one-second basic, long and
   uplink windows expose listening time; the uplink is only a prefix capture
   because a 432-byte payload exceeds the session API's 255-byte limit. */
#include "../ls_experiments.h"
#include "uat_decode.h"
#include "uat_stream_rx.h"
#include "uat_settings.h"
#include <stdlib.h>
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "ls_lora.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR struct {
    uint32_t sync[2], valid[2], bad, corrected, errors;
    uint64_t listen_us[2];
    float rssi[2], peak;
    bool have[2], have_peak;
    int window;
    bool active;
    uat_fix_t fix;
    bool have_fix;
} s;
static EXT_RAM_BSS_ATTR uint8_t raw[48];
static int64_t changed, last, read_at;
extern const ls_experiment_t exp_uat;
static uat_settings_t settings = {.rate = 2000000};
static bool configured;
static atomic_bool run_stream;
static atomic_uint run_rate;
static uat_settings_t selected(void)
{
    portENTER_CRITICAL(&lock); uat_settings_t v = settings; portEXIT_CRITICAL(&lock); return v;
}
/* start is the experiments worker, never the rendering task. The SD object
   supplies boot defaults; explicit OPTIONS/console choices take precedence. */
static bool load_settings(char *why, size_t cap)
{
#ifdef ESP_PLATFORM
    portENTER_CRITICAL(&lock); bool have = configured; portEXIT_CRITICAL(&lock);
    if (!have) {
        FILE *f = fopen("/sdcard/uat/settings.json", "rb");
        if (f) {
            char text[513]; size_t n = fread(text, 1, sizeof(text), f);
            bool ok = !ferror(f); fclose(f);
            uat_settings_t v;
            if (!ok || !uat_settings_parse(text, n, &v)) {
                snprintf(why, cap, "Invalid /sdcard/uat/settings.json v1"); return false;
            }
            portENTER_CRITICAL(&lock); if (!configured) settings = v; portEXIT_CRITICAL(&lock);
        }
        portENTER_CRITICAL(&lock); configured = true; portEXIT_CRITICAL(&lock);
    }
#else
    (void)why; (void)cap;
#endif
    return true;
}
static void choose(uat_settings_t v)
{
    portENTER_CRITICAL(&lock); settings = v; configured = true; portEXIT_CRITICAL(&lock);
    if (ls_exp_running() == &exp_uat) ls_exp_start(&exp_uat);
}
static bool configure(int argc, char **argv, char *why, size_t n)
{
    uat_settings_t v = selected();
    if (argc == 1 && !strcmp(argv[0], "packet")) { v.stream = false; choose(v); return true; }
    if (argc >= 1 && argc <= 2 && !strcmp(argv[0], "stream")) {
        v.stream = true;
        if (argc == 2) {
            char *end; unsigned long kbps = strtoul(argv[1], &end, 10);
            if ((end == argv[1] || *end) && strcmp(argv[1], "1041.667")) goto bad;
            /* The bug demonstration accepts the API's exact bps spelling
               as well as 1041.667 kbps. Ordinary choices use integer kbps. */
            if (!strcmp(argv[1], "1041.667") || kbps == 1041667) v.rate = 1041667;
            else if (kbps <= 2000) v.rate = (uint32_t)kbps * 1000;
            else goto bad;
            if (!uat_stream_rate_valid(v.rate)) goto bad;
        }
        choose(v); return true;
    }
bad:
    snprintf(why, n, "packet | stream [2000|1500|1250|1041.667]"); return false;
}
static int get_mode(const ls_opt_t *o) { (void)o; return selected().stream; }
static void set_mode(const ls_opt_t *o, int v) { (void)o; uat_settings_t c = selected(); c.stream = v != 0; choose(c); }
static uint32_t rate_at_index(int i) { return i == 0 ? 2000000 : i == 1 ? 1500000 : i == 2 ? 1250000 : 1041667; }
static int get_rate(const ls_opt_t *o)
{
    (void)o; uint32_t r = selected().rate;
    for (int i = 0; i < 4; ++i) if (rate_at_index(i) == r) return i;
    return 0;
}
static void set_rate(const ls_opt_t *o, int v)
{
    (void)o; if (v < 0 || v > 3) return;
    uat_settings_t c = selected(); c.rate = rate_at_index(v); choose(c);
}
static const char *const modes[] = {"PACKET", "STREAM"};
static const char *const rates[] = {"2000k", "1500k", "1250k", "1041667 BUG"};
static const ls_opt_t opts[] = {
    {.label="MODE", .kind=LS_OPT_CYCLE, .names=modes, .n=2, .get=get_mode, .set=set_mode, .step=1},
    {.label="STREAM SAMPLE RATE", .kind=LS_OPT_CYCLE, .names=rates, .n=4, .get=get_rate, .set=set_rate, .step=1}
};
static esp_err_t tune(int window)
{
    const ls_fsk_cfg_t cfg = {.freq_hz = 978000000,
                              .bitrate = 1041667,
                              .deviation_hz = 312500,
                              .bandwidth_hz = 2222222,
                              .sync_word = window == 2 ? ~0xACDDA4E2u : 0xACDDA4E2u,
                              .sync_bits = 32,
                              .payload_bytes = window == 0 ? 30 : 48,
                              .preamble_bits = 8,
                              .rssi_at_sync = true};
    return ls_lora_fsk_begin(&cfg);
}
static bool start(char *why, size_t n)
{
    /* 978 MHz is past the SX126x's 960; only a part that listens wide can open it. */
    if (!(ls_lora_caps() & LS_LORA_CAP_FSK) || !ls_lora_rx_range_ok(ls_lora_caps(), 978000000u, 978000000u)) {
        snprintf(why, n, "%s", LS_EXP_NEEDS_LR2021);
        return false;
    }
    if (!load_settings(why, n)) return false;
    uat_settings_t v = selected(); run_stream = v.stream; run_rate = v.rate;
    if (run_stream && !(ls_lora_caps() & LS_LORA_CAP_FSK_STREAM)) {
        snprintf(why, n, "Needs LR2021 FSK stream"); return false;
    }
    portENTER_CRITICAL(&lock);
    memset(&s, 0, sizeof(s));
    portEXIT_CRITICAL(&lock);
    ls_fsk_cfg_t cfg;
    if (run_stream) uat_stream_rx_config(&cfg, run_rate);
    esp_err_t e = run_stream ? ls_lora_fsk_begin(&cfg) : tune(0);
    if (e != ESP_OK) {
        ls_lora_fsk_end();
        snprintf(why, n, "UAT FSK: %s", esp_err_to_name(e));
        return false;
    }
    if (run_stream && !uat_stream_rx_start(run_rate)) {
        uat_stream_rx_stop(); ls_lora_fsk_end();
        snprintf(why, n, "UAT stream workers unavailable"); return false;
    }
    portENTER_CRITICAL(&lock);
    s.active = true;
    portEXIT_CRITICAL(&lock);
    last = changed = read_at = esp_timer_get_time();
    return true;
}
static void stop(void)
{
    if (run_stream) uat_stream_rx_stop();
    ls_lora_fsk_end();
    portENTER_CRITICAL(&lock);
    s.active = false;
    portEXIT_CRITICAL(&lock);
}
static void poll(void)
{
    if (run_stream) {
#ifndef ESP_PLATFORM
        /* Host simulator has no RF tasks; its worker advances the same steps. */
        uat_stream_rx_read();
        while (uat_stream_rx_decode()) {}
#endif
        return;
    }
    int64_t now = esp_timer_get_time();
    int polarity = s.window == 2;
    portENTER_CRITICAL(&lock);
    if (s.active) s.listen_us[polarity] += (uint64_t)(now - last);
    portEXIT_CRITICAL(&lock);
    last = now;
    if (s.active)
        for (int drain = 0; drain < 8; drain++) {
            float dbm = NAN;
            int n = ls_lora_fsk_poll(raw, sizeof(raw), &dbm);
            if (n < 0) {
                portENTER_CRITICAL(&lock);
                s.errors++;
                portEXIT_CRITICAL(&lock);
                break;
            }
            if (!n) break;
            uat_fix_t fix;
            int errors = 0;
            bool valid = !polarity && uat_frame_decode(raw, n, &fix, &errors);
            portENTER_CRITICAL(&lock);
            s.sync[polarity]++;
            if (isfinite(dbm)) {
                s.rssi[polarity] = dbm;
                s.have[polarity] = true;
            }
            if (!polarity) {
                if (valid) {
                    s.valid[fix.type != 0]++;
                    s.corrected += errors;
                    s.fix = fix;
                    s.have_fix = true;
                } else
                    s.bad++;
            }
            portEXIT_CRITICAL(&lock);
        }
    if (s.active && now - read_at >= 2000) {
        float dbm;
        if (ls_lora_rssi_inst(&dbm) == ESP_OK && isfinite(dbm)) {
            portENTER_CRITICAL(&lock);
            if (!s.have_peak || dbm > s.peak) {
                s.peak = dbm;
                s.have_peak = true;
            }
            portEXIT_CRITICAL(&lock);
        }
        read_at = now;
    }
    if (now - changed >= 1000000) {
        ls_lora_fsk_end();
        int next = (s.window + 1) % 3;
        esp_err_t e = tune(next);
        portENTER_CRITICAL(&lock);
        s.window = next;
        s.active = e == ESP_OK;
        if (e != ESP_OK) s.errors++;
        portEXIT_CRITICAL(&lock);
        changed = last = esp_timer_get_time();
    }
}
static int stream_lines(char (*out)[LS_EXP_LINE], int max)
{
    uat_stream_rx_stats_t v; uat_stream_rx_snapshot(&v);
    uat_settings_t c = selected();
    int n = 0;
#define LINE(...) do { if (n < max) snprintf(out[n++], LS_EXP_LINE, __VA_ARGS__); } while (0)
    LINE("MODE STREAM / %lu bps", (unsigned long)(v.rate ? v.rate : c.rate));
    LINE("978 MHz LF / +/-312.5 kHz / RX ONLY");
    LINE("BYTE/s %lu / %lu expected", (unsigned long)v.bytes_sec, (unsigned long)((v.rate ? v.rate : c.rate)/8));
    LINE("READS %lu / bytes %llu", (unsigned long)v.reads, (unsigned long long)v.bytes);
    LINE("RESTARTS %lu / loss seams %lu", (unsigned long)v.restarts, (unsigned long)v.seams);
    LINE("RING drops %lu / faults %lu", (unsigned long)v.drops, (unsigned long)v.faults);
    LINE("DN SYNC %lu / UP SYNC %lu", (unsigned long)v.decoded.sync[0], (unsigned long)v.decoded.sync[1]);
    LINE("RS basic %lu long %lu bad %lu", (unsigned long)v.decoded.valid[0], (unsigned long)v.decoded.valid[1], (unsigned long)v.decoded.bad);
    LINE("Corrected %lu symbols", (unsigned long)v.decoded.corrected);
    if (v.decoded.have_fix) LINE("ADDRESS %06lX type %u", (unsigned long)v.decoded.fix.address, v.decoded.fix.type);
    else LINE("ADDRESS -- / awaiting valid RS");
    if (v.decoded.have_fix && v.decoded.fix.position) LINE("POSITION %+.5f %+.5f", v.decoded.fix.lat, v.decoded.fix.lon);
    else LINE("POSITION --");
    LINE("36-bit +/-4 sync; UP prefix only");
#undef LINE
    return n;
}
static int lines(char (*out)[LS_EXP_LINE], int max)
{
    portENTER_CRITICAL(&lock); bool active = s.active; portEXIT_CRITICAL(&lock);
    if (active ? run_stream : selected().stream) return stream_lines(out, max);
    uint32_t sync[2], valid[2], bad, corrected, errors;
    uint64_t listen[2];
    float rssi[2], peak;
    bool have[2], have_peak, have_fix;
    int window;
    uat_fix_t fix;
    portENTER_CRITICAL(&lock);
    memcpy(sync, s.sync, sizeof(sync));
    memcpy(valid, s.valid, sizeof(valid));
    memcpy(listen, s.listen_us, sizeof(listen));
    memcpy(rssi, s.rssi, sizeof(rssi));
    memcpy(have, s.have, sizeof(have));
    bad = s.bad;
    corrected = s.corrected;
    errors = s.errors;
    peak = s.peak;
    have_peak = s.have_peak;
    window = s.window;
    fix = s.fix;
    have_fix = s.have_fix;
    portEXIT_CRITICAL(&lock);
    int n = 0;
    if (n < max) snprintf(out[n++], LS_EXP_LINE, "MODE PACKET / 978 MHz / 1041667 bps");
    if (n < max)
        snprintf(out[n++], LS_EXP_LINE, "WINDOW %s / 1s",
                 window == 0   ? "basic 30B"
                 : window == 1 ? "long 48B"
                               : "uplink prefix 48B");
    for (int p = 0; p < 2 && n < max; p++)
        snprintf(out[n++], LS_EXP_LINE, "%s SYNC %lu  listen %.1fs", p ? "UP" : "DN", (unsigned long)sync[p],
                 listen[p] / 1e6);
    for (int p = 0; p < 2 && n < max; p++) {
        if (have[p])
            snprintf(out[n++], LS_EXP_LINE, "%s sync RSSI %.0f dBm", p ? "UP" : "DN", (double)rssi[p]);
        else
            snprintf(out[n++], LS_EXP_LINE, "%s sync RSSI --", p ? "UP" : "DN");
    }
    if (n < max)
        snprintf(out[n++], LS_EXP_LINE, "RS basic %lu long %lu bad %lu", (unsigned long)valid[0],
                 (unsigned long)valid[1], (unsigned long)bad);
    if (n < max)
        snprintf(out[n++], LS_EXP_LINE, "Corrected %lu symbols; radio errors %lu", (unsigned long)corrected,
                 (unsigned long)errors);
    if (n < max) {
        if (have_peak)
            snprintf(out[n++], LS_EXP_LINE, "RSSI sampled peak %.0f dBm", (double)peak);
        else
            snprintf(out[n++], LS_EXP_LINE, "RSSI sampled peak --");
    }
    if (n < max) {
        if (have_fix)
            snprintf(out[n++], LS_EXP_LINE, "ADDRESS %06lX qualifier %u type %u", (unsigned long)fix.address,
                     fix.qualifier, fix.type);
        else
            snprintf(out[n++], LS_EXP_LINE, "ADDRESS -- (awaiting valid RS frame)");
    }
    if (n < max) {
        if (have_fix && fix.position)
            snprintf(out[n++], LS_EXP_LINE, "POSITION %+.5f %+.5f", fix.lat, fix.lon);
        else
            snprintf(out[n++], LS_EXP_LINE, "POSITION --");
    }
    if (n < max) snprintf(out[n++], LS_EXP_LINE, "32-bit sync capture counts; uplink prefix only");
    return n;
}
const ls_experiment_t exp_uat = {.id = "uat",
                                 .name = "UAT 978",
                                 .sub = "978 MHz ADS-B RS checks and sync diagnostics",
                                 .maturity = LS_EXP_TRYING,
                                 .lr2021_only = true,
                                 .needs = "UAT aircraft or ground uplink in range",
                                 .start = start,
                                 .stop = stop,
                                 .poll = poll,
                                 .lines = lines,
                                 .configure = configure, .opts = opts, .n_opts = sizeof(opts)/sizeof(opts[0])};
