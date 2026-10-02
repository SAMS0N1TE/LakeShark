/* LS_TEST_SOURCES: experiments/exp_rf.c and experiments/exp_iridium.c with
   ls_experiments.c, against an HF listener that hears 8 ms bursts every
   90 ms on one simplex channel, which moves two channels over halfway
   through, as a pass's Doppler shift would carry it: the hop plan, burst
   counting per channel, and the frequency trace. */

#include "ls_test.h"

#include "ls_experiments.h"
#include "ls_lora.h"
#include "esp_timer.h"
#include "experiments/exp_rf.h"

#include <stdio.h>
#include <string.h>

void ls_shim_time_set(int64_t us);
void ls_shim_time_advance(int64_t us);

#define SX_CAPS   (LS_LORA_CAP_LORA | LS_LORA_CAP_FSK | LS_LORA_CAP_RSSI_INST)
#define LR21_CAPS (SX_CAPS | LS_LORA_CAP_RX_WIDE | LS_LORA_CAP_BAND_1G5_2G5)

const char *ls_exp_hw_radio_busy(void) { return NULL; }
bool ls_exp_hw_radio_take(void) { return true; }
void ls_exp_hw_radio_give(void) { }
bool ls_exp_hw_wake(void) { return false; }

static uint32_t s_caps = LR21_CAPS;
static bool s_fsk;
static ls_fsk_cfg_t s_cfg;
static uint32_t s_freq;
static int s_retunes;
/* Which channel the satellite is on, by centre frequency, and from when the
   second one takes over. */
static uint32_t s_sat_a, s_sat_b;
static int64_t s_switch_us;

uint32_t ls_lora_caps(void) { return s_caps; }
uint32_t ls_lora_fsk_bw_snap(uint32_t hz) { return hz; }
esp_err_t ls_lora_fsk_begin(const ls_fsk_cfg_t *cfg)
{
    if (!ls_lora_rx_range_ok(s_caps, cfg->freq_hz, cfg->freq_hz)) return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;
    s_freq = cfg->freq_hz;
    s_fsk = true;
    return ESP_OK;
}
esp_err_t ls_lora_fsk_end(void) { s_fsk = false; return ESP_OK; }
esp_err_t ls_lora_fsk_receive(void) { return ESP_OK; }
esp_err_t ls_lora_fsk_retune(uint32_t hz) { if (!s_fsk) return ESP_ERR_INVALID_STATE; s_freq = hz; s_retunes++; return ESP_OK; }
int ls_lora_fsk_poll(uint8_t *buf, size_t size, float *rssi) { (void)buf; (void)size; (void)rssi; return 0; }

esp_err_t ls_lora_rssi_inst(float *dbm)
{
    if (!s_fsk) return ESP_ERR_INVALID_STATE;
    ls_shim_time_advance(100);
    const int64_t t = esp_timer_get_time();
    const uint32_t sat = t < s_switch_us ? s_sat_a : s_sat_b;
    const bool on = s_freq == sat && t % 90000 < 8000;
    *dbm = on ? -97.0f : -116.0f + (float)((t / 100) % 3);
    return ESP_OK;
}

static const ls_experiment_t *iridium(uint32_t caps)
{
    extern const ls_experiment_t exp_iridium;
    ls_exp_forget();
    ls_exp_register(&exp_iridium);
    s_caps = caps;
    s_fsk = false;
    s_retunes = 0;
    ls_shim_time_set(1000000);
    return &exp_iridium;
}

static const char *find_line(char (*out)[LS_EXP_LINE], int n, const char *want)
{
    for (int i = 0; i < n; i++) if (strstr(out[i], want)) return out[i];
    return NULL;
}

LS_CASE(iridium_needs_the_hf_input_and_says_so)
{
    const ls_experiment_t *e = iridium(SX_CAPS);
    ls_exp_start(e);
    ls_exp_service();
    LS_CHECK(ls_exp_running() == NULL);
    char state[96];
    ls_exp_state_line(e, state, sizeof(state));
    LS_CHECK(strstr(state, "no 1.5-2.5 GHz input") != NULL);
    LS_CHECK(e->needs && strstr(e->needs, "2.4 GHz"));
    static char out[4][LS_EXP_LINE];
    const int n = ls_exp_read_lines(e, out, 4);
    LS_EQ_INT(n, 2);
    LS_EQ_STR(out[0], "SIMPLEX 1626.0-1626.5 MHz in 12 channels");
}

LS_CASE(iridium_hops_the_simplex_channels_and_traces_the_strongest)
{
    const ls_experiment_t *e = iridium(LR21_CAPS);
    /* Channel 6 is the ring alert channel, 1626.270833 MHz; the satellite
       drifts to channel 8 after fourteen seconds. */
    s_sat_a = 1626000000u + 41666u / 2 + 41666u * 6;
    s_sat_b = 1626000000u + 41666u / 2 + 41666u * 8;
    s_switch_us = 15000000;
    ls_exp_start(e);
    ls_exp_service();
    LS_CHECK(ls_exp_running() == e);
    LS_EQ_UINT(s_cfg.freq_hz, 1626020833u);
    LS_EQ_UINT(s_cfg.bandwidth_hz, 41666u);
    LS_CHECK(s_cfg.bitrate + 2 * s_cfg.deviation_hz <= s_cfg.bandwidth_hz);
    /* Twenty seconds: polls of up to 15 ms with the worker's 2 ms between. */
    while (esp_timer_get_time() < 21000000) { ls_exp_service(); ls_shim_time_advance(2000); }
    LS_CHECK(s_retunes > 200);
    static char out[24][LS_EXP_LINE];
    const int n = ls_exp_read_lines(e, out, 24);
    for (int i = 0; i < n; i++) printf("  | %s\n", out[i]);   /* the readout, for the log */
    for (int i = 0; i < n; i++) LS_CHECK_MSG(strlen(out[i]) <= 48, "line %d too long: %s", i, out[i]);
    LS_EQ_STR(out[0], "SIMPLEX 1626.0-1626.5 MHz, 12 x 41.7 kHz");
    LS_CHECK(strstr(out[1], "DWELL 90 ms  CYCLE 1.1 s  +8 dB") == out[1]);
    LS_CHECK(strstr(out[2], "BURSTS ") == out[2]);
    LS_CHECK(strstr(out[2], "PEAK -97 dBm") != NULL);
    const char *best = find_line(out, n, "BEST ");
    LS_CHECK(best && (strstr(best, "1626.271") || strstr(best, "1626.354")));
    /* Every burst on the two channels the satellite used, none elsewhere. */
    const char *ch = find_line(out, n, "CH [");
    LS_CHECK(ch != NULL);
    if (ch) {
        for (int c = 0; c < 12; c++) {
            const char mark = ch[4 + c];
            if (c == 6 || c == 8) LS_CHECK_MSG(mark != ' ' && mark != '.', "channel %d: '%c'", c, mark);
            else LS_CHECK_MSG(mark == ' ', "channel %d: '%c'", c, mark);
        }
    }
    LS_CHECK(find_line(out, n, "LEN ~") != NULL);
    /* The newest trace rows are on channel 8, older ones on channel 6. */
    const char *head = find_line(out, n, "TRACE");
    LS_CHECK(head != NULL);
    if (head) {
        const char *newest = head + LS_EXP_LINE;
        LS_CHECK(strstr(newest, "1626.354") != NULL);
        LS_CHECK(strstr(newest, "........*...") != NULL);
        LS_CHECK(find_line(out, n, "......*.....") != NULL);   /* before the drift */
    }
    ls_exp_stop();
    ls_exp_service();
    LS_CHECK(!s_fsk);
}

LS_CASE(iridium_options_pick_the_plan_and_dwell)
{
    const ls_experiment_t *e = iridium(LR21_CAPS);
    LS_EQ_INT(e->n_opts, 3);
    e->opts[0].set(&e->opts[0], 1);
    ls_exp_start(e);
    ls_exp_service();
    LS_EQ_UINT(s_cfg.freq_hz, 1616250000u);                  /* the first 500 kHz channel */
    LS_EQ_UINT(s_cfg.bandwidth_hz, 500000u);
    char shown[32];
    e->opts[1].set_num(&e->opts[1], 180);
    e->opts[1].show(&e->opts[1], shown, sizeof(shown));
    LS_EQ_STR(shown, "180 ms");
    ls_exp_service();
    static char out[4][LS_EXP_LINE];
    ls_exp_read_lines(e, out, 4);
    LS_EQ_STR(out[0], "WHOLE BAND 1616.0-1626.5 MHz, 21 x 500.0 kHz");
    ls_exp_stop();
    ls_exp_service();
    e->opts[0].set(&e->opts[0], 0);
    e->opts[1].set_num(&e->opts[1], 90);
}

LS_CASE(iridium_is_marked_lr2021_only)
{
    extern const ls_experiment_t exp_iridium;
    LS_CHECK(exp_iridium.lr2021_only);
}
