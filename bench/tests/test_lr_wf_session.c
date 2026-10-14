#include "ls_test.h"
#include <string.h>
#include "../../components/lakeshark/radio/lr_wf_session.c"
static ls_lora_cfg_t cfg = {.freq_hz = 915000000, .sf = 9, .bw_hz = 125000};
static ls_lora_cfg_t original;
static unsigned configured, starts, delays, frees_seen;
static bool fail_config, fail_receive, fail_task, fail_capture, no_samples, bad_pair, stop_requested;
static void (*owner_task)(void *);
static void *owner_arg;
static uint64_t first;
static uint32_t confirmed_hz;
const ls_lora_cfg_t *ls_lora_cfg(void) { return &cfg; }
void ls_lora_cfg_default(ls_lora_cfg_t *out) { *out = cfg; }
static uint32_t test_caps;
uint32_t ls_lora_caps(void) { return test_caps; }
ls_lora_chip_t ls_lora_chip(void) { return LS_LORA_CHIP_LR20XX; }
bool ls_lora_fsk_active(void) { return false; }
bool ls_lora_scanning(void) { return false; }
bool lr20xx_stream_source2_active(void) { return false; }
esp_err_t ls_lora_configure(const ls_lora_cfg_t *c)
{
    ++configured;
    if (configured == 1) {
        LS_EQ_UINT(31250, c->bw_hz); LS_EQ_INT(7, c->sf); LS_CHECK(!c->invert_iq);
        if (fail_config) return ESP_FAIL;
    }
    cfg = *c; return ESP_OK;
}
esp_err_t lr20xx_set_agc_gain(uint8_t gain) { LS_EQ_INT(gain,0); return ESP_OK; }
esp_err_t ls_lora_receive(void) { return fail_receive ? ESP_FAIL : ESP_OK; }
esp_err_t lr20xx_stream_source2_live(lr20xx_iq_sink_t sink, void *arg, lr20xx_stream_result_t *out)
{
    (void)sink; (void)arg; memset(out, 0, sizeof(*out)); return ESP_OK;
}
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task, const char *name,
    uint32_t stack, void *arg, UBaseType_t priority, TaskHandle_t *handle, BaseType_t core)
{
    (void)name; (void)stack; (void)priority; (void)handle; (void)core;
    if (fail_task || (fail_capture && task == capture)) return pdFAIL;
    ++starts;
    if (task == owner) { owner_task = task; owner_arg = arg; }
    return pdPASS;
}
void vTaskDelete(TaskHandle_t task) { (void)task; }
void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
    if (owner_task) {
        TaskFunction_t task = owner_task; owner_task = NULL; task(owner_arg); return;
    }
    ++delays; ls_shim_time_advance(3000);
    if (atomic_load(&session->capture_stop)) { capture(session); return; }
    if (no_samples) return;
    if (delays > 70 || stop_requested) { atomic_store(&session->stop, true); return; }
    uint8_t data[1280] = {0};
    LS_CHECK(collect(session, data, sizeof(data), first + (bad_pair ? 1 : 0)));
    first += sizeof(data)/4;
    if (lr_spec_center()) confirmed_hz = lr_spec_center();
}
static void fresh(void)
{
    lr_wf_stop(); lr_spec_enable(true); lr_spec_end();
    cfg = (ls_lora_cfg_t){.freq_hz=915000000, .sf=9, .bw_hz=125000}; original = cfg;
    configured = starts = delays = confirmed_hz = 0;
    fail_config = fail_receive = fail_task = fail_capture = no_samples = bad_pair = stop_requested = false;
    first = 0; owner_task = NULL; ls_shim_time_set(1000000);
    frees_seen = ls_shim_heap_outstanding();
}
static void run_owner(void)
{
    TaskFunction_t task = owner_task; owner_task = NULL; task(owner_arg);
    LS_CHECK(atomic_load(&session->done));
    LS_EQ_UINT(original.freq_hz, cfg.freq_hz); LS_EQ_INT(original.sf, cfg.sf);
    LS_EQ_UINT(original.bw_hz, cfg.bw_hz); LS_EQ_UINT(0, lr_spec_center());
}
LS_CASE(session_confirms_every_tune_and_restores_after_stop)
{
    fresh();
    for (int k=0;k<2;++k) {
        configured = delays = 0; first = 0;
        LS_EQ_INT(ESP_OK, lr_wf_start(433920000));
        LS_EQ_UINT(0, lr_spec_center());
        run_owner(); LS_EQ_UINT(433920000, confirmed_hz); LS_EQ_UINT(2, configured);
        lr_wf_stop(); LS_EQ_UINT(frees_seen, ls_shim_heap_outstanding());
    }
}
LS_CASE(configuration_receive_and_task_failures_clean_up)
{
    fresh(); fail_config = true;
    LS_EQ_INT(ESP_OK, lr_wf_start(433920000)); run_owner();
    LS_EQ_INT(ESP_FAIL, lr_wf_start(433920000)); lr_wf_stop();
    fresh(); fail_receive = true;
    LS_EQ_INT(ESP_OK, lr_wf_start(433920000)); run_owner(); lr_wf_stop();
    fresh(); fail_task = true;
    LS_EQ_INT(ESP_ERR_NO_MEM, lr_wf_start(433920000));
    LS_EQ_UINT(frees_seen, ls_shim_heap_outstanding());
}
LS_CASE(discontinuous_blocks_are_never_published_and_release_waits_for_owner)
{
    fresh(); bad_pair = true;
    LS_EQ_INT(ESP_OK, lr_wf_start(433920000)); run_owner();
    LS_EQ_INT(ESP_ERR_INVALID_RESPONSE, session->error); LS_EQ_UINT(0, confirmed_hz);
    lr_wf_stop();
    fresh(); LS_EQ_INT(ESP_OK, lr_wf_start(433920000));
    lr_wf_stop(); LS_EQ_UINT(frees_seen, ls_shim_heap_outstanding());
    LS_EQ_UINT(original.freq_hz, cfg.freq_hz);
    LS_EQ_INT(ESP_ERR_INVALID_ARG, lr_wf_start(149999999));
    LS_EQ_INT(ESP_ERR_INVALID_ARG, lr_wf_start(960000001));
}

LS_CASE(capture_task_failure_and_missing_samples_restore_configuration)
{
    fresh(); fail_capture = true;
    LS_EQ_INT(ESP_OK, lr_wf_start(433920000)); run_owner();
    LS_EQ_INT(ESP_ERR_NO_MEM, session->error); lr_wf_stop();
    fresh(); no_samples = true;
    LS_EQ_INT(ESP_OK, lr_wf_start(433920000)); run_owner();
    LS_EQ_INT(ESP_ERR_TIMEOUT, session->error); LS_EQ_UINT(0, confirmed_hz);
    lr_wf_stop(); LS_EQ_UINT(frees_seen, ls_shim_heap_outstanding());
}

static unsigned consumed, reset_seen;
static void consume_checked(void *ctx, const uint8_t *data, size_t bytes, bool spare)
{
    LS_CHECK(ctx == &consumed);
    if (!data) { ++reset_seen; return; }
    const lr2021_iq_block *b = lr2021_iq_queue_peek(&session->queue);
    LS_CHECK(b && data == b->data); LS_EQ_UINT(bytes, b->bytes);
    LS_CHECK(lr_spec_center() == session->hz);
    (void)spare; ++consumed;
}
LS_CASE(lr_fm_consumer_sees_owner_held_blocks_and_resets_before_release)
{
    fresh(); consumed = reset_seen = 0;
    LS_EQ_INT(ESP_OK, lr_wf_start_consumer(162550000, consume_checked, &consumed));
    /* A waterfall client cannot replace a listening owner. */
    LS_EQ_INT(ESP_ERR_INVALID_STATE, lr_wf_start(162550000));
    run_owner(); LS_CHECK(consumed > 20); LS_EQ_UINT(1, reset_seen);
    lr_wf_stats st; lr_wf_stop_stats(&st);
    LS_EQ_UINT(0, st.lost_pairs); LS_EQ_UINT(0, st.gaps);
    LS_EQ_UINT(frees_seen, ls_shim_heap_outstanding());
}
LS_CASE(lr_fm_queue_full_stops_without_blocking_and_accounts_samples)
{
    fresh(); consumed = reset_seen = 0;
    LS_EQ_INT(ESP_OK, lr_wf_start_consumer(162550000, consume_checked, &consumed));
    uint8_t data[1280] = {0};
    for (unsigned i=0; i<LR2021_IQ_QUEUE_SLOTS; ++i)
        LS_CHECK(collect(session, data, sizeof(data), i*320));
    LS_CHECK(!collect(session, data, sizeof(data), LR2021_IQ_QUEUE_SLOTS*320));
    lr_wf_stats st; lr_wf_get_stats(&st);
    LS_EQ_UINT(320, st.lost_pairs); LS_EQ_UINT(1, st.gaps);
    /* Terminating capture abandons every queued block, counted after join. */
    stop_requested = true;
    lr_wf_stop_stats(&st);
    LS_EQ_UINT((LR2021_IQ_QUEUE_SLOTS+1)*320, st.lost_pairs);
    LS_EQ_UINT(1, reset_seen);
}
LS_CASE(lr_fm_gap_never_bridges_discriminator_history)
{
    fresh(); bad_pair = true; consumed = reset_seen = 0;
    LS_EQ_INT(ESP_OK, lr_wf_start_consumer(162550000, consume_checked, &consumed));
    run_owner();
    lr_wf_stats st; lr_wf_stop_stats(&st);
    LS_EQ_UINT(0, consumed); LS_EQ_UINT(1, reset_seen);
    LS_EQ_UINT(321, st.lost_pairs); LS_EQ_UINT(1, st.gaps);
}

LS_CASE(lriq_subghz_lf_receive_above_tx_band)
{
    fresh();test_caps=LS_LORA_CAP_BAND_1G5_2G5;
    LS_EQ_INT(ESP_OK,lr_wf_start(990000000));run_owner();
    LS_EQ_UINT(990000000,confirmed_hz);lr_wf_stop();
    LS_EQ_INT(ESP_ERR_INVALID_ARG,lr_wf_start(1000000000));
    test_caps=0;
}
