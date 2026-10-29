#include "ls_test.h"
#include <math.h>
#include <string.h>
#include "../../components/lakeshark/radio/lr_wf_session.c"

/* lr_wf_retune: a running session moves to a new channel without ending,
   the way P25 follows a trunking grant. Narrow re-programs the LoRa modem
   at the new channel; wide only moves the LO and keeps the down-converter's
   filter. Also the WIDE SPAN feed (lr_span_*): the whole 576k route, LO in
   the middle, the channel at +offset. Fakes follow test_lr_wf_wide.c. */

#define PI 3.14159265358979323846
#define TONE 60

static ls_lora_cfg_t cfg = {.freq_hz = 915000000, .sf = 9, .bw_hz = 125000};
static ls_lora_cfg_t original;
static unsigned configured;
static uint32_t configured_at[8];
const ls_lora_cfg_t *ls_lora_cfg(void) { return &cfg; }
void ls_lora_cfg_default(ls_lora_cfg_t *out) { *out = cfg; }
uint32_t ls_lora_caps(void) { return 0; }
ls_lora_chip_t ls_lora_chip(void) { return LS_LORA_CHIP_LR20XX; }
bool ls_lora_fsk_active(void) { return false; }
bool ls_lora_scanning(void) { return false; }
bool lr20xx_stream_source2_active(void) { return false; }
esp_err_t ls_lora_configure(const ls_lora_cfg_t *c)
{
    if (configured < 8) configured_at[configured] = c->freq_hz;
    ++configured; cfg = *c; return ESP_OK;
}
esp_err_t lr20xx_set_agc_gain(uint8_t gain) { LS_EQ_INT(gain, 0); return ESP_OK; }
esp_err_t ls_lora_receive(void) { return ESP_OK; }
lr20xx_rx_path_t lr20xx_path_for_hz(uint32_t hz) { return hz >= 1000000000u ? LR20XX_PATH_HF : LR20XX_PATH_LF; }

/* Each launched capture: its route and the channel the modem was set to. */
static char launched[16];
static uint32_t launched_lo[16], launched_modem[16];
static unsigned n_launched;
esp_err_t lr20xx_stream_source2_live(lr20xx_iq_sink_t sink, void *arg, lr20xx_stream_result_t *out)
{
    (void)sink; (void)arg; memset(out, 0, sizeof(*out)); return ESP_OK;
}
esp_err_t lr20xx_stream_live(const lr20xx_iq_config_t *c, lr20xx_iq_block_sink_t sink,
                             void *arg, lr20xx_stream_result_t *out)
{
    (void)c; (void)sink; (void)arg; memset(out, 0, sizeof(*out)); return ESP_OK;
}

static void (*owner_task)(void *);
static void *owner_arg;
static uint64_t first;
static double phase;
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task, const char *name,
    uint32_t stack, void *arg, UBaseType_t priority, TaskHandle_t *handle, BaseType_t core)
{
    (void)name; (void)stack; (void)priority; (void)handle; (void)core;
    if (task == owner) { owner_task = task; owner_arg = arg; }
    if (task == capture) {
        const session_t *s = arg;
        if (n_launched < sizeof(launched) - 1) {
            launched_lo[n_launched] = s->run_wide ? s->iq.frequency_hz : s->hz;
            launched_modem[n_launched] = cfg.freq_hz;
            launched[n_launched++] = s->run_wide ? 'W' : 'N';
        }
        first = 0; phase = 0;
    }
    return pdPASS;
}
void vTaskDelete(TaskHandle_t task) { (void)task; }

/* Script: at delay `at` retune to hz (or set wide) ; stop at stop_at. */
static struct { unsigned at; uint32_t hz; int wide; } script[4];
static unsigned n_script, stop_at, delays;
static esp_err_t retune_result[4];
/* Read the span once, at delay probe_at, while the stream is live. */
static unsigned probe_at;
static bool probed;
static float probe_bins[LR_SPAN_BINS];
static lr_span_line probe_line;
static void push_wide(session_t *s)
{
    const double w = 2 * PI * (double)s->run_offset / WIDE_RATE_HZ;
    for (int b = 0; b < 4; ++b) {
        uint8_t data[1024];
        for (int k = 0; k < 512; ++k, phase += w) {
            data[2 * k] = (uint8_t)(int8_t)lrint(TONE * cos(phase));
            data[2 * k + 1] = (uint8_t)(int8_t)lrint(TONE * sin(phase));
        }
        phase = fmod(phase, 2 * PI);
        LS_CHECK(collect_wide(s, data, sizeof(data), first, WIDE_RATE_HZ, LR20XX_IQ8));
        first += 512;
    }
}
void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
    if (owner_task) { TaskFunction_t t = owner_task; owner_task = NULL; t(owner_arg); return; }
    ++delays; ls_shim_time_advance(3000);
    if (atomic_load(&session->capture_stop)) { capture(session); return; }
    for (unsigned i = 0; i < n_script; ++i) {
        if (script[i].at != delays) continue;
        if (script[i].wide >= 0) LS_EQ_INT(ESP_OK, lr_wf_set_wide(script[i].wide != 0, LR_WF_WIDE_DEFAULT_HZ));
        else retune_result[i] = lr_wf_retune(script[i].hz);
    }
    if (probe_at && delays == probe_at)
        probed = lr_span_read(probe_bins, &probe_line, (uint32_t)(esp_timer_get_time() / 1000));
    if (delays >= stop_at) { atomic_store(&session->stop, true); return; }
    if (session->run_wide) push_wide(session);
    else {
        uint8_t data[1280] = {0};
        LS_CHECK(collect(session, data, sizeof(data), first));
        first += sizeof(data) / 4;
    }
}

static unsigned calls, resets;
static uint32_t channel_seen[8];
static unsigned n_channels;
static void consume(void *ctx, const uint8_t *data, size_t bytes, bool spare)
{
    (void)ctx; (void)bytes; (void)spare;
    if (!data) { ++resets; return; }
    ++calls;
    /* Every block is tagged with the session's channel; note each change. */
    if (!n_channels || channel_seen[n_channels - 1] != lr_spec_center())
        if (n_channels < 8) channel_seen[n_channels++] = lr_spec_center();
}

static unsigned heap_before;
static void fresh(void)
{
    lr_wf_stop(); lr_spec_enable(true); lr_spec_end(); lr_span_enable(false);
    cfg = (ls_lora_cfg_t){.freq_hz = 915000000, .sf = 9, .bw_hz = 125000}; original = cfg;
    LS_EQ_INT(ESP_OK, lr_wf_set_wide(false, LR_WF_WIDE_DEFAULT_HZ));
    memset(launched, 0, sizeof(launched)); n_launched = 0;
    memset(script, 0, sizeof(script)); n_script = 0; stop_at = 60; delays = 0;
    configured = calls = resets = n_channels = 0;
    first = 0; phase = 0; owner_task = NULL; ls_shim_time_set(1000000);
    heap_before = ls_shim_heap_outstanding();
}
static void run(uint32_t hz, lr_wf_stats *st)
{
    LS_EQ_INT(ESP_OK, lr_wf_start_consumer(hz, consume, NULL));
    TaskFunction_t t = owner_task; owner_task = NULL; t(owner_arg);
    LS_CHECK(atomic_load(&session->done));
    LS_EQ_INT(ESP_OK, session->error);
    LS_EQ_UINT(original.freq_hz, cfg.freq_hz);
    lr_wf_stop_stats(st);
    LS_EQ_UINT(heap_before, ls_shim_heap_outstanding());
}

LS_CASE(narrow_retune_moves_the_modem_inside_the_session)
{
    fresh();
    script[0] = (typeof(script[0])){20, 152800000, -1}; n_script = 1; stop_at = 45;
    lr_wf_stats st; run(152600000, &st);
    LS_EQ_INT(ESP_OK, retune_result[0]);
    LS_EQ_STR("NN", launched);
    /* Start, the retune, the restore: three, and the second capture ran at
       the new channel. */
    LS_EQ_UINT(3, configured);
    LS_EQ_UINT(152600000, configured_at[0]); LS_EQ_UINT(152800000, configured_at[1]);
    LS_EQ_UINT(152600000, launched_modem[0]); LS_EQ_UINT(152800000, launched_modem[1]);
    LS_EQ_UINT(2, n_channels);
    LS_EQ_UINT(152600000, channel_seen[0]); LS_EQ_UINT(152800000, channel_seen[1]);
    /* A deliberate move loses nothing: one reset per stream end. */
    LS_EQ_UINT(0, st.lost_pairs); LS_EQ_UINT(0, st.gaps); LS_EQ_UINT(2, resets);
    lr_wf_wide_status w; lr_wf_wide_get(&w);
    LS_EQ_UINT(1, w.retunes); LS_CHECK(w.retune_us > 0 && w.retune_us <= w.retune_max_us);
    LS_EQ_UINT(0, w.switches);
}

LS_CASE(wide_retune_moves_only_the_lo_and_keeps_the_filter)
{
    fresh();
    LS_EQ_INT(ESP_OK, lr_wf_set_wide(true, LR_WF_WIDE_DEFAULT_HZ));
    script[0] = (typeof(script[0])){15, 155000000, -1}; n_script = 1; stop_at = 40;
    lr_wf_stats st; run(152600000, &st);
    LS_EQ_STR("WW", launched);
    LS_EQ_UINT(152600000 - LR_WF_WIDE_DEFAULT_HZ, launched_lo[0]);
    LS_EQ_UINT(155000000 - LR_WF_WIDE_DEFAULT_HZ, launched_lo[1]);
    /* Wide never re-programs the modem for a hop: start and restore only. */
    LS_EQ_UINT(2, configured);
    LS_EQ_UINT(2, resets); LS_EQ_UINT(0, st.lost_pairs); LS_EQ_UINT(0, st.gaps);
    LS_EQ_UINT(155000000, channel_seen[n_channels - 1]);
    lr_wf_wide_status w; lr_wf_wide_get(&w);
    LS_EQ_UINT(1, w.retunes); LS_EQ_UINT(2, w.runs); LS_EQ_UINT(155000000, w.channel_hz);
    LS_CHECK(w.out_pairs > 0); LS_EQ_UINT(0, w.ddc_dropped);
}

/* A wide hop leaves the modem on the old channel; going narrow afterwards
   must program the channel the session is on now. */
LS_CASE(narrow_after_a_wide_hop_programs_the_current_channel)
{
    fresh();
    LS_EQ_INT(ESP_OK, lr_wf_set_wide(true, LR_WF_WIDE_DEFAULT_HZ));
    script[0] = (typeof(script[0])){10, 160000000, -1};
    script[1] = (typeof(script[0])){20, 0, 0};
    n_script = 2; stop_at = 35;
    lr_wf_stats st; run(152600000, &st);
    LS_EQ_STR("WWN", launched);
    LS_EQ_UINT(160000000, launched_modem[2]);
    LS_EQ_UINT(0, st.lost_pairs);
}

/* `p25 lrwide on`: an owner pins wide for its session with the setting off. */
LS_CASE(an_owner_pin_runs_wide_and_leaves_the_setting_alone)
{
    fresh();
    lr_wf_set_wide_force(true);
    script[0] = (typeof(script[0])){15, 152900000, -1}; n_script = 1; stop_at = 30;
    lr_wf_stats st; run(152600000, &st);
    lr_wf_set_wide_force(false);
    LS_EQ_STR("WW", launched);
    LS_CHECK(!lr_wf_wide_on());
    lr_wf_wide_status w; lr_wf_wide_get(&w);
    LS_CHECK(!w.on); LS_EQ_UINT(1, w.retunes);
    LS_EQ_UINT(0, st.lost_pairs);
}

LS_CASE(retune_is_refused_without_a_session_or_off_band)
{
    fresh();
    LS_EQ_INT(ESP_ERR_INVALID_STATE, lr_wf_retune(152600000));
    LS_EQ_INT(ESP_OK, lr_wf_start_consumer(152600000, consume, NULL));
    LS_EQ_INT(ESP_ERR_INVALID_ARG, lr_wf_retune(1000));
    LS_EQ_INT(ESP_OK, lr_wf_set_wide(true, LR_WF_WIDE_OFFSET_HZ));
    /* The wide LO, 100 kHz below, would fall under the band. */
    LS_EQ_INT(ESP_ERR_INVALID_ARG, lr_wf_retune(150050000));
    LS_EQ_INT(ESP_OK, lr_wf_retune(150200000));
    stop_at = 0; lr_wf_stop();
    LS_EQ_UINT(heap_before, ls_shim_heap_outstanding());
}

/* WIDE SPAN: the owner hands the 576k route to the span spectrum, LO in
   the middle, the channel's tone at +144 kHz: three quarters across. */
LS_CASE(wide_span_shows_the_channel_three_quarters_across)
{
    fresh();
    LS_EQ_INT(ESP_OK, lr_wf_set_wide(true, LR_WF_WIDE_DEFAULT_HZ));
    LS_CHECK(lr_span_enable(true));
    probe_at = 150; probed = false; stop_at = 160;
    lr_wf_stats st; run(152600000, &st);
    LS_CHECK(probed);
    LS_EQ_UINT(152600000 - LR_WF_WIDE_DEFAULT_HZ, probe_line.lo_hz);
    LS_EQ_UINT(152600000, probe_line.channel_hz);
    LS_EQ_UINT(576000, probe_line.span_hz);
    unsigned peak = 0;
    for (unsigned i = 1; i < LR_SPAN_BINS; ++i) if (probe_bins[i] > probe_bins[peak]) peak = i;
    /* 0.5 + 144/576 of the way across. */
    LS_CHECK_MSG(peak >= 190 && peak <= 194, "peak bin %u", peak);
    LS_CHECK(probe_bins[peak] > 0.5f);
    lr_span_stats_t sp; lr_span_stats(&sp);
    LS_CHECK(sp.lines >= 3); LS_CHECK(sp.windows >= 4 * (sp.lines - 1));
    /* Ended: the last line is not offered as live. */
    float bins[LR_SPAN_BINS]; lr_span_line line;
    LS_CHECK(!lr_span_read(bins, &line, (uint32_t)(esp_timer_get_time() / 1000)));
    lr_span_enable(false);
    probe_at = 0;
}

LS_CASE(span_off_costs_nothing_and_narrow_runs_feed_no_span)
{
    fresh();
    lr_span_stats_t before; lr_span_stats(&before);
    LS_EQ_INT(ESP_OK, lr_wf_set_wide(true, LR_WF_WIDE_DEFAULT_HZ));
    lr_wf_stats st; run(152600000, &st);
    lr_span_stats_t after; lr_span_stats(&after);
    LS_EQ_UINT(before.windows, after.windows);
    /* Enabled, but the run is narrow: still nothing. */
    fresh(); LS_CHECK(lr_span_enable(true));
    run(152600000, &st);
    lr_span_stats(&after);
    LS_EQ_UINT(before.windows, after.windows);
    lr_span_enable(false);
}
