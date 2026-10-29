#include "ls_test.h"
#include <math.h>
#include <string.h>
#include "../../components/lakeshark/radio/lr_wf_session.c"

/* Wide mode of lr_wf_session: the 576k IQ8 route at LO = channel - offset,
   expanded and down-converted in the owner to the 125k IQ16 the consumers
   already take, in calls of at most 256 pairs, with the channel at 0 Hz.
   A setting change restarts the stream inside the running session; off
   brings back the source-2 125k path. Fakes follow test_lr_wf_session.c:
   the owner runs inside the first vTaskDelay, each later delay plays one
   3 ms stretch of the capture task. */

#define PI 3.14159265358979323846
#define TONE 60 /* int8 amplitude: 15360 once expanded */

static ls_lora_cfg_t cfg = {.freq_hz = 915000000, .sf = 9, .bw_hz = 125000};
static ls_lora_cfg_t original;
const ls_lora_cfg_t *ls_lora_cfg(void) { return &cfg; }
void ls_lora_cfg_default(ls_lora_cfg_t *out) { *out = cfg; }
uint32_t ls_lora_caps(void) { return 0; }
ls_lora_chip_t ls_lora_chip(void) { return LS_LORA_CHIP_LR20XX; }
bool ls_lora_fsk_active(void) { return false; }
bool ls_lora_scanning(void) { return false; }
bool lr20xx_stream_source2_active(void) { return false; }
esp_err_t ls_lora_configure(const ls_lora_cfg_t *c) { cfg = *c; return ESP_OK; }
esp_err_t lr20xx_set_agc_gain(uint8_t gain) { LS_EQ_INT(gain, 0); return ESP_OK; }
esp_err_t ls_lora_receive(void) { return ESP_OK; }
lr20xx_rx_path_t lr20xx_path_for_hz(uint32_t hz)
{
    return hz >= 1000000000u ? LR20XX_PATH_HF : LR20XX_PATH_LF;
}

/* Routes in the order the owner launched them ('N' source 2, 'W' 576k), and
   the driver calls made when each capture task ran out. */
static char launched[16], streamed[16];
static unsigned n_launched, n_streamed;
static lr20xx_iq_config_t launched_iq, streamed_iq;
esp_err_t lr20xx_stream_source2_live(lr20xx_iq_sink_t sink, void *arg, lr20xx_stream_result_t *out)
{
    (void)sink; (void)arg; memset(out, 0, sizeof(*out));
    if (n_streamed < sizeof(streamed) - 1) streamed[n_streamed++] = 'N';
    return ESP_OK;
}
esp_err_t lr20xx_stream_live(const lr20xx_iq_config_t *c, lr20xx_iq_block_sink_t sink,
                             void *arg, lr20xx_stream_result_t *out)
{
    LS_CHECK(sink == collect_wide && arg == session);
    memset(out, 0, sizeof(*out)); streamed_iq = *c;
    if (n_streamed < sizeof(streamed) - 1) streamed[n_streamed++] = 'W';
    return ESP_OK;
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
        if (n_launched < sizeof(launched) - 1) launched[n_launched++] = s->run_wide ? 'W' : 'N';
        if (s->run_wide) launched_iq = s->iq;
        first = 0; phase = 0;
    }
    return pdPASS;
}
void vTaskDelete(TaskHandle_t task) { (void)task; }

/* Script: at delay `at[i]` apply wide setting i; stop at `stop_at`. */
static struct { unsigned at; bool on; int32_t offset; } script[4];
static unsigned n_script, stop_at, delays;
static uint32_t confirmed_hz;
static void push_wide(session_t *s)
{
    /* One 4 KiB check batch, published as four 1 KiB blocks: a tone at the
       channel, which sits run_offset above the LO. */
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
    if (owner_task) {
        TaskFunction_t task = owner_task; owner_task = NULL; task(owner_arg); return;
    }
    ++delays; ls_shim_time_advance(3000);
    if (atomic_load(&session->capture_stop)) { capture(session); return; }
    for (unsigned i = 0; i < n_script; ++i)
        if (script[i].at == delays) LS_EQ_INT(ESP_OK, lr_wf_set_wide(script[i].on, script[i].offset));
    if (delays >= stop_at) { atomic_store(&session->stop, true); return; }
    if (session->run_wide) push_wide(session);
    else {
        uint8_t data[1280] = {0};
        LS_CHECK(collect(session, data, sizeof(data), first));
        first += sizeof(data) / 4;
    }
    if (lr_spec_center()) confirmed_hz = lr_spec_center();
}

/* Consumer: block sizes, and the tone's frequency and level per run. */
static unsigned calls, resets, max_pairs, settle;
static uint64_t wide_pairs;
static bool have_last;
static double last_i, last_q, turn, amp;
static unsigned measured;
static void consume(void *ctx, const uint8_t *data, size_t bytes, bool spare)
{
    (void)spare;
    LS_CHECK(ctx == &calls);
    if (!data) { ++resets; have_last = false; settle = 0; return; }
    ++calls;
    LS_CHECK(bytes && bytes % 4 == 0);
    LS_EQ_UINT(session->hz, lr_spec_center());
    if (!session->run_wide) return;
    if (bytes / 4 > max_pairs) max_pairs = (unsigned)(bytes / 4);
    const int16_t *iq = (const int16_t *)data;
    for (size_t k = 0; k < bytes / 4; ++k) {
        const double i = iq[2 * k], q = iq[2 * k + 1];
        ++wide_pairs;
        if (++settle < 600) continue; /* filter fill after each (re)start */
        if (have_last) { turn += atan2(q * last_i - i * last_q, i * last_i + q * last_q); ++measured; }
        amp += sqrt(i * i + q * q);
        last_i = i; last_q = q; have_last = true;
    }
}

static unsigned heap_before;
static void fresh(void)
{
    lr_wf_stop(); lr_spec_enable(true); lr_spec_end();
    cfg = (ls_lora_cfg_t){.freq_hz = 915000000, .sf = 9, .bw_hz = 125000}; original = cfg;
    LS_EQ_INT(ESP_OK, lr_wf_set_wide(false, LR_WF_WIDE_OFFSET_HZ));
    memset(launched, 0, sizeof(launched)); memset(streamed, 0, sizeof(streamed));
    n_launched = n_streamed = 0; n_script = 0; stop_at = 60; delays = 0;
    confirmed_hz = calls = resets = max_pairs = settle = measured = 0;
    wide_pairs = 0; have_last = false; turn = amp = 0;
    first = 0; phase = 0; owner_task = NULL; ls_shim_time_set(1000000);
    heap_before = ls_shim_heap_outstanding();
}
static void run_and_stop(uint32_t hz, lr_wf_stats *out)
{
    LS_EQ_INT(ESP_OK, lr_wf_start_consumer(hz, consume, &calls));
    TaskFunction_t task = owner_task; owner_task = NULL; task(owner_arg);
    LS_CHECK(atomic_load(&session->done));
    LS_EQ_INT(ESP_OK, session->error);
    LS_EQ_UINT(original.freq_hz, cfg.freq_hz); LS_EQ_UINT(0, lr_spec_center());
    lr_wf_stop_stats(out);
    LS_EQ_UINT(heap_before, ls_shim_heap_outstanding());
}
/* Mean frequency (Hz) and level of the tone over the measured output. */
static double tone_hz(void) { return measured ? turn / measured * LR_DDC_OUT_HZ / (2 * PI) : 1e9; }
static double tone_level(void) { return measured ? amp / measured : 0; }

LS_CASE(wide_streams_576k_at_lo_below_channel_and_feeds_125k_chunks)
{
    fresh();
    LS_EQ_INT(ESP_OK, lr_wf_set_wide(true, LR_WF_WIDE_OFFSET_HZ));
    lr_wf_stats st; run_and_stop(152600000, &st);
    LS_EQ_STR("W", launched); LS_EQ_STR("W", streamed);
    LS_EQ_INT(LR20XX_IQ_SOURCE0_576, launched_iq.route);
    LS_EQ_UINT(152500000, launched_iq.frequency_hz);
    LS_EQ_INT(LR20XX_PATH_LF, launched_iq.path);
    LS_CHECK(launched_iq.host_gain);
    LS_EQ_UINT(152500000, streamed_iq.frequency_hz);
    LS_EQ_UINT(152600000, confirmed_hz);
    LS_EQ_UINT(0, st.lost_pairs); LS_EQ_UINT(0, st.gaps);
    LS_EQ_UINT(1, resets);
    LS_CHECK(calls > 50); LS_CHECK(max_pairs > 0 && max_pairs <= 256);
    /* The channel's tone arrives at 0 Hz at its input level. */
    LS_CHECK_MSG(fabs(tone_hz()) < 50, "tone at %.1f Hz", tone_hz());
    LS_CHECK_MSG(fabs(tone_level() - TONE * 256) < TONE * 256 * 0.1, "level %.0f", tone_level());
    lr_wf_wide_status w; lr_wf_wide_get(&w);
    LS_CHECK(w.on && !w.active); LS_EQ_INT(LR_WF_WIDE_OFFSET_HZ, w.offset_hz);
    LS_EQ_UINT(1, w.runs); LS_EQ_UINT(0, w.switches); LS_EQ_UINT(0, w.queue_drops);
    LS_EQ_UINT(152500000, w.lo_hz); LS_EQ_UINT(152600000, w.channel_hz);
    LS_EQ_UINT(576000, w.rate_hz); LS_EQ_UINT(WIDE_SLOTS, w.queue_slots);
    LS_CHECK(w.queue_high_water >= 4 && w.queue_high_water <= 8);
    LS_EQ_UINT(0, w.ddc_dropped); LS_EQ_UINT(wide_pairs, w.out_pairs);
    /* 125k out of 576k in, to within the filter's fill. */
    const double ratio = (double)w.out_pairs / w.in_pairs;
    LS_CHECK_MSG(fabs(ratio - 125.0 / 576) < 0.002, "ratio %.5f", ratio);
}

LS_CASE(negative_offset_puts_lo_above_channel)
{
    fresh();
    LS_EQ_INT(ESP_OK, lr_wf_set_wide(true, -150000));
    lr_wf_stats st; run_and_stop(162550000, &st);
    LS_EQ_STR("W", launched);
    LS_EQ_UINT(162700000, launched_iq.frequency_hz);
    LS_CHECK_MSG(fabs(tone_hz()) < 50, "tone at %.1f Hz", tone_hz());
    LS_EQ_UINT(0, st.lost_pairs); LS_EQ_UINT(0, st.gaps);
}

LS_CASE(toggling_restarts_in_session_and_off_restores_source2)
{
    fresh();
    script[0] = (typeof(script[0])){12, true, LR_WF_WIDE_OFFSET_HZ};
    script[1] = (typeof(script[0])){30, false, LR_WF_WIDE_OFFSET_HZ};
    n_script = 2; stop_at = 45;
    lr_wf_stats st; run_and_stop(152600000, &st);
    LS_EQ_STR("NWN", launched); LS_EQ_STR("NWN", streamed);
    LS_EQ_UINT(152500000, launched_iq.frequency_hz);
    /* One reset per stream end; a deliberate switch loses nothing. */
    LS_EQ_UINT(3, resets);
    LS_EQ_UINT(0, st.lost_pairs); LS_EQ_UINT(0, st.gaps);
    LS_CHECK(max_pairs > 0 && max_pairs <= 256);
    LS_CHECK_MSG(fabs(tone_hz()) < 50, "tone at %.1f Hz", tone_hz());
    lr_wf_wide_status w; lr_wf_wide_get(&w);
    LS_CHECK(!w.on); LS_EQ_UINT(1, w.switches); /* counted since the last change */
    /* A fresh start with wide off is plain source 2. */
    fresh();
    run_and_stop(152600000, &st);
    LS_EQ_STR("N", launched); LS_EQ_STR("N", streamed); LS_EQ_UINT(0, wide_pairs);
    LS_EQ_UINT(152600000, confirmed_hz);
}

LS_CASE(offset_change_while_wide_moves_lo_and_ddc)
{
    fresh();
    LS_EQ_INT(ESP_OK, lr_wf_set_wide(true, LR_WF_WIDE_OFFSET_HZ));
    script[0] = (typeof(script[0])){15, true, 180000};
    n_script = 1; stop_at = 40;
    lr_wf_stats st; run_and_stop(433920000, &st);
    LS_EQ_STR("WW", launched);
    LS_EQ_UINT(433920000 - 180000, launched_iq.frequency_hz);
    LS_EQ_UINT(433920000 - 180000, streamed_iq.frequency_hz);
    LS_EQ_UINT(2, resets);
    LS_CHECK_MSG(fabs(tone_hz()) < 50, "tone at %.1f Hz", tone_hz());
}

LS_CASE(full_wide_ring_stops_capture_and_counts_the_drop)
{
    fresh();
    LS_EQ_INT(ESP_OK, lr_wf_set_wide(true, LR_WF_WIDE_OFFSET_HZ));
    LS_EQ_INT(ESP_OK, lr_wf_start_consumer(152600000, consume, &calls));
    LS_EQ_INT(ESP_OK, prepare_run(session)); session->run_wide = true;
    uint8_t data[1024] = {0};
    for (unsigned i = 0; i < WIDE_SLOTS; ++i)
        LS_CHECK(collect_wide(session, data, sizeof(data), i * 512ull, WIDE_RATE_HZ, LR20XX_IQ8));
    LS_CHECK(!collect_wide(session, data, sizeof(data), WIDE_SLOTS * 512ull, WIDE_RATE_HZ, LR20XX_IQ8));
    /* Only 576k IQ8 is accepted. */
    LS_CHECK(!collect_wide(session, data, sizeof(data), 0, 125000, LR20XX_IQ16));
    lr_wf_stats st; lr_wf_get_stats(&st);
    LS_EQ_UINT(1024, st.lost_pairs); LS_EQ_UINT(2, st.gaps);
    lr_wf_wide_status w; lr_wf_wide_get(&w);
    LS_EQ_UINT(WIDE_SLOTS, w.queue_high_water); LS_EQ_UINT(2, w.queue_drops);
    stop_at = 0; lr_wf_stop();
    LS_EQ_UINT(heap_before, ls_shim_heap_outstanding());
}

/* The boot setting, 144 kHz, is 576k/4: the down-converter's sign-and-swap
   mix. A run there streams the channel at 0 Hz at its level like any other
   offset. */
LS_CASE(default_offset_runs_the_ddc_in_over_4_path)
{
    fresh();
    LS_EQ_INT(ESP_OK, lr_wf_set_wide(true, LR_WF_WIDE_DEFAULT_HZ));
    LS_EQ_INT(ESP_OK, lr_wf_start_consumer(152600000, consume, &calls));
    LS_EQ_INT(ESP_OK, prepare_run(session));
    LS_EQ_INT(1, session->dsp->ddc.quarter);
    LS_EQ_UINT(152600000 - LR_WF_WIDE_DEFAULT_HZ, session->iq.frequency_hz);
    stop_at = 0; lr_wf_stop();
    LS_EQ_UINT(heap_before, ls_shim_heap_outstanding());

    fresh();
    LS_EQ_INT(ESP_OK, lr_wf_set_wide(true, LR_WF_WIDE_DEFAULT_HZ));
    lr_wf_stats st; run_and_stop(152600000, &st);
    LS_EQ_STR("W", launched);
    LS_EQ_UINT(152600000 - LR_WF_WIDE_DEFAULT_HZ, launched_iq.frequency_hz);
    LS_EQ_UINT(0, st.lost_pairs); LS_EQ_UINT(0, st.gaps);
    LS_CHECK_MSG(fabs(tone_hz()) < 50, "tone at %.1f Hz", tone_hz());
    LS_CHECK_MSG(fabs(tone_level() - TONE * 256) < TONE * 256 * 0.1, "level %.0f", tone_level());
    lr_wf_wide_status w; lr_wf_wide_get(&w);
    LS_EQ_UINT(0, w.ddc_dropped); LS_EQ_UINT(wide_pairs, w.out_pairs);
    const double ratio = (double)w.out_pairs / w.in_pairs;
    LS_CHECK_MSG(fabs(ratio - 125.0 / 576) < 0.002, "ratio %.5f", ratio);
}

LS_CASE(offsets_and_lo_outside_the_band_are_refused)
{
    fresh();
    LS_EQ_INT(ESP_ERR_INVALID_ARG, lr_wf_set_wide(true, 5000));
    LS_EQ_INT(ESP_ERR_INVALID_ARG, lr_wf_set_wide(true, -230000));
    LS_EQ_INT(ESP_OK, lr_wf_set_wide(true, LR_WF_WIDE_OFFSET_HZ));
    /* 150.05 MHz is receivable, 149.95 is not: the LO is refused up front. */
    LS_EQ_INT(ESP_ERR_INVALID_ARG, lr_wf_start_consumer(150050000, consume, &calls));
    LS_EQ_INT(ESP_OK, lr_wf_set_wide(true, -LR_WF_WIDE_OFFSET_HZ));
    LS_EQ_INT(ESP_OK, lr_wf_start_consumer(150050000, consume, &calls));
    stop_at = 0; lr_wf_stop();
    LS_EQ_UINT(heap_before, ls_shim_heap_outstanding());
}
