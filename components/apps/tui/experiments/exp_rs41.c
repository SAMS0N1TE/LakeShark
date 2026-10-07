/* Vaisala RS41 reception on the LR2021 GFSK stream, with no transmit path. */
#include "../ls_experiments.h"
#include "rs41_stream.h"
#include "rs41_store.h"
#include "ls_lora.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
extern const ls_experiment_t exp_rs41;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_hz = 403000000;
static uint32_t s_run_hz;
static bool s_inverted, s_listening, s_pol_locked;
static int64_t s_tune_us, s_good_us;
static EXT_RAM_BSS_ATTR rs41_decoder_t s_decoder;
static EXT_RAM_BSS_ATTR rs41_stream_t s_stream;
static EXT_RAM_BSS_ATTR uint8_t s_rx[256];
typedef struct { unsigned good, fixed, bad, encrypted; } stats_t;
static stats_t s_stats;
static uint32_t frequency(void)
{
    portENTER_CRITICAL(&s_lock); uint32_t hz = s_hz; portEXIT_CRITICAL(&s_lock); return hz;
}
static esp_err_t listen(bool inverted)
{
    if (s_listening) { ls_lora_fsk_end(); s_listening = false; }
    const ls_fsk_cfg_t c = {
        .freq_hz = s_run_hz, .bitrate = 4800, .deviation_hz = 2400,
        .bandwidth_hz = ls_lora_fsk_bw_snap(24000), .pulse_shape = 0x05,
        .sync_word = inverted ? ~0x4469481fu : 0x4469481fu,
        .sync_bits = 32, .stream = true,
    };
    esp_err_t err = ls_lora_fsk_begin(&c);
    s_listening = err == ESP_OK; s_inverted = inverted;
    s_tune_us = esp_timer_get_time(); rs41_stream_reset(&s_stream,false);
    return err;
}
static bool start(char *why, size_t n)
{
    if (!(ls_lora_caps() & LS_LORA_CAP_FSK_STREAM)) { snprintf(why,n,"Needs LR2021 FSK streaming"); return false; }
    rs41_init(&s_decoder); s_run_hz = frequency(); s_good_us = 0; s_pol_locked = false;
    portENTER_CRITICAL(&s_lock); memset(&s_stats,0,sizeof(s_stats)); portEXIT_CRITICAL(&s_lock);
    esp_err_t err = listen(false);
    if (err != ESP_OK) { snprintf(why,n,"RS41 RX: %s",esp_err_to_name(err)); return false; }
    return true;
}
static void stop(void) { if (s_listening) ls_lora_fsk_end(); s_listening = false; }
static void frame(const rs41_report_t *r, void *arg)
{
    (void)arg; s_good_us = esp_timer_get_time(); s_pol_locked = true;
    rs41_store_put(r,s_good_us);
    portENTER_CRITICAL(&s_lock);
    ++s_stats.good; s_stats.fixed += r->corrected; s_stats.bad += r->bad_blocks;
    if (r->encrypted) ++s_stats.encrypted;
    portEXIT_CRITICAL(&s_lock);
}
static void poll(void)
{
    bool restarted = false;
    int got = ls_lora_fsk_stream_read(s_rx,sizeof(s_rx),&restarted);
    if (got < 0) rs41_stream_reset(&s_stream,false);
    if (got > 0) {
        if (restarted) rs41_stream_reset(&s_stream,true);
        rs41_stream_feed(&s_stream,&s_decoder,s_rx,(size_t)got,s_inverted,frame,NULL);
    }
    int64_t now = esp_timer_get_time();
    /* The receiver's polarity is fixed, so it is searched for only until the first good
       frame; after that a quiet spell never restarts the listener (and never hands the
       3 s that follow to the wrong polarity). Never mid-frame either. */
    if (!s_pol_locked && !s_stream.collecting && now-s_tune_us > 3000000) (void)listen(!s_inverted);
}
static int lines(char (*out)[LS_EXP_LINE], int max)
{
    stats_t v; portENTER_CRITICAL(&s_lock); v = s_stats; portEXIT_CRITICAL(&s_lock);
    int n = 0;
#define LINE(...) do { if (n < max) snprintf(out[n++],LS_EXP_LINE,__VA_ARGS__); } while (0)
    LINE("RX %.3f MHz ONLY / 4800 GFSK, no scan",frequency()/1e6);
    LINE("SYNC %s",s_pol_locked ? (s_inverted ? "locked, inverted" : "locked") : "searching polarity");
    LINE("FRAMES %u / ECC %u symbols / CRC %u bad",v.good,v.fixed,v.bad);
    if (v.encrypted) LINE("RS41-SGM ENCRYPTED / %u frames",v.encrypted);
    LINE("SONDES: H opens list, profile and recovery");
    LINE("TRACKS %d hours / MAP %s",rs41_keep_hours(),rs41_show_map() ? "on" : "off");
#undef LINE
    return n;
}
static void set_frequency(double mhz)
{
    if (!isfinite(mhz)) return;
    uint32_t hz = (uint32_t)(fmax(400,fmin(406,mhz))*100+0.5)*10000;
    portENTER_CRITICAL(&s_lock); s_hz = hz; portEXIT_CRITICAL(&s_lock);
    if (ls_exp_running() == &exp_rs41) ls_exp_start(&exp_rs41);
}
static bool configure(int argc, char **argv, char *why, size_t n)
{
    char *end = NULL; double v = argc == 1 ? strtod(argv[0],&end) : 0;
    if (argc != 1 || end == argv[0] || *end || !isfinite(v) || v < 400 || v > 406) {
        snprintf(why,n,"frequency 400.000-406.000 MHz"); return false;
    }
    set_frequency(v); return true;
}
static double o_freq(const ls_opt_t *o) { (void)o; return frequency()/1e6; }
static void o_set_freq(const ls_opt_t *o, double v) { (void)o; set_frequency(v); }
static void o_show_freq(const ls_opt_t *o, char *out, size_t n) { (void)o; snprintf(out,n,"%.3f",frequency()/1e6); }
/* Editable starting points; local launch frequencies vary by station. */
static const char *const PRESET_NAMES[] = { "400.500", "401.000", "402.000", "403.000", "403.500", "404.000", "405.000" };
static const uint32_t PRESETS[] = {400500000,401000000,402000000,403000000,403500000,404000000,405000000};
static int o_preset(const ls_opt_t *o)
{
    (void)o; uint32_t hz = frequency();
    for (int i = 0; i < 7; ++i) if (PRESETS[i] == hz) return i;
    return -1;
}
static void o_show_preset(const ls_opt_t *o, char *out, size_t n)
{
    int v = o_preset(o);
    snprintf(out,n,"%s",v >= 0 ? PRESET_NAMES[v] : "CUSTOM");
}
static void o_set_preset(const ls_opt_t *o, int v) { (void)o; if (v >= 0 && v < 7) set_frequency(PRESETS[v]/1e6); }
static void o_show_keep(const ls_opt_t *o, char *out, size_t n) { (void)o; snprintf(out,n,"%d h",rs41_keep_hours()); }
static double o_keep(const ls_opt_t *o) { (void)o; return rs41_keep_hours(); }
static void o_set_keep(const ls_opt_t *o, double v) { (void)o; if (isfinite(v)) rs41_set_keep_hours((int)lround(v)); }
static int o_map(const ls_opt_t *o) { (void)o; return rs41_show_map(); }
static void o_set_map(const ls_opt_t *o, int v) { (void)o; rs41_set_show_map(v != 0); }
static const ls_opt_t RADIO[] = {
    { .label="FREQ MHz", .kind=LS_OPT_LEVEL, .num=o_freq, .set_num=o_set_freq,
      .lo=400, .hi=406, .step=.01, .unit="MHz / 10 kHz steps", .show=o_show_freq },
    { .label="PRESET MHz", .kind=LS_OPT_CYCLE, .names=PRESET_NAMES, .n=7, .get=o_preset, .set=o_set_preset, .show=o_show_preset },
};
static const ls_opt_t TRACKS[] = {
    { .label="KEEP HOURS", .kind=LS_OPT_LEVEL, .num=o_keep, .set_num=o_set_keep, .lo=1, .hi=24, .step=1, .unit="hours", .show=o_show_keep },
    { .label="SHOW ON MAP", .kind=LS_OPT_TOGGLE, .get=o_map, .set=o_set_map },
};
static const ls_opt_ctx_t RADIO_CTX = { .name="RECEIVER", .radio=LS_RSEL_LORA, .job=-1, LS_OPT_ROWS(RADIO) };
static const ls_opt_ctx_t TRACK_CTX = { .name="TRACKS", .radio=LS_RSEL_LORA, .job=-1, LS_OPT_ROWS(TRACKS) };
static const ls_opt_t OPTS[] = {
    { .label="RECEIVER", .kind=LS_OPT_MENU, .sub=&RADIO_CTX },
    { .label="TRACKS", .kind=LS_OPT_MENU, .sub=&TRACK_CTX },
};
const ls_experiment_t exp_rs41 = {
    .id="rs41", .name="RS41 SONDES", .sub="Vaisala balloons / 400-406 MHz / RX only",
    .maturity=LS_EXP_TRYING, .lr2021_only=true,
    .start=start, .stop=stop, .poll=poll, .lines=lines, .configure=configure,
    .opts=OPTS, .n_opts=2,
};
