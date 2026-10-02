/* UAT 978: one hardware sync at a time. Equal one-second basic, long and
   uplink windows expose listening time; the uplink is only a prefix capture
   because a 432-byte payload exceeds the session API's 255-byte limit. */
#include "../ls_experiments.h"
#include "uat_decode.h"
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
    portENTER_CRITICAL(&lock);
    memset(&s, 0, sizeof(s));
    portEXIT_CRITICAL(&lock);
    esp_err_t e = tune(0);
    if (e != ESP_OK) {
        ls_lora_fsk_end();
        snprintf(why, n, "UAT FSK: %s", esp_err_to_name(e));
        return false;
    }
    portENTER_CRITICAL(&lock);
    s.active = true;
    portEXIT_CRITICAL(&lock);
    last = changed = read_at = esp_timer_get_time();
    return true;
}
static void stop(void)
{
    ls_lora_fsk_end();
    portENTER_CRITICAL(&lock);
    s.active = false;
    portEXIT_CRITICAL(&lock);
}
static void poll(void)
{
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
static int lines(char (*out)[LS_EXP_LINE], int max)
{
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
    if (n < max) snprintf(out[n++], LS_EXP_LINE, "978 MHz 1.041667 Mbps FSK  +/-312.5 kHz");
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
                                 .lines = lines};
