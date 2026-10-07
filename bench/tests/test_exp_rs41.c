#include "ls_test.h"
#include "ls_experiments.h"
#include "ls_lora.h"
#include "experiments/rs41_store.h"
extern const ls_experiment_t exp_rs41;
void ls_shim_time_set(int64_t us);
void ls_shim_time_advance(int64_t us);
static ls_fsk_cfg_t cfg;
static bool active, capable = true;
static int begins, ends;
const char *ls_exp_hw_radio_busy(void) { return NULL; }
bool ls_exp_hw_radio_take(void) { return true; }
void ls_exp_hw_radio_give(void) { }
bool ls_exp_hw_wake(void) { return false; }
uint32_t ls_lora_caps(void) { return capable ? LS_LORA_CAP_FSK_STREAM : LS_LORA_CAP_FSK; }
uint32_t ls_lora_fsk_bw_snap(uint32_t hz) { return hz; }
esp_err_t ls_lora_fsk_begin(const ls_fsk_cfg_t *c) { cfg = *c; active = true; ++begins; return ESP_OK; }
esp_err_t ls_lora_fsk_end(void) { active = false; ++ends; return ESP_OK; }
int ls_lora_fsk_stream_read(uint8_t *buf, size_t n, bool *restarted)
{
    LS_CHECK(active); LS_CHECK(n >= 256); (void)buf; *restarted = false; return 0;
}
LS_CASE(receive_only_session_and_polarity_search)
{
    ls_exp_forget(); ls_shim_time_set(1000000); capable = true; begins = ends = 0;
    ls_exp_register(&exp_rs41); ls_exp_start(&exp_rs41); ls_exp_service();
    LS_CHECK(ls_exp_running() == &exp_rs41); LS_CHECK(active && cfg.stream);
    LS_EQ_INT(cfg.bitrate,4800); LS_EQ_INT(cfg.deviation_hz,2400); LS_EQ_INT(cfg.pulse_shape,5);
    LS_EQ_UINT(cfg.sync_word,0x4469481f); LS_EQ_INT(cfg.sync_bits,32);
    ls_shim_time_advance(3000001); ls_exp_service(); LS_EQ_UINT(cfg.sync_word,~0x4469481fu);
    LS_EQ_INT(begins,2); LS_EQ_INT(ends,1);
    ls_exp_stop(); ls_exp_service(); LS_CHECK(!active); LS_EQ_INT(ends,2);
    capable = false; ls_exp_start(&exp_rs41); ls_exp_service(); LS_CHECK(ls_exp_running() == NULL);
}
LS_CASE(layered_options_frequency_steps_and_bounds)
{
    const ls_opt_ctx_t *radio = exp_rs41.opts[0].sub, *tracks = exp_rs41.opts[1].sub;
    LS_EQ_INT(exp_rs41.opts[0].kind,LS_OPT_MENU); LS_CHECK(radio && tracks);
    const ls_opt_t *freq = &radio->opt[0]; LS_EQ_INT(freq->kind,LS_OPT_LEVEL); LS_NEAR(freq->step,.01,1e-8);
    freq->set_num(freq,403.01); LS_NEAR(freq->num(freq),403.01,1e-8);
    freq->set_num(freq,400); LS_NEAR(freq->num(freq),400,1e-8);
    freq->set_num(freq,406); LS_NEAR(freq->num(freq),406,1e-8);
    char why[80], *bad[] = {"406.01"}; LS_CHECK(!exp_rs41.configure(1,bad,why,sizeof(why)));
    char *good[] = {"403.007"}; LS_CHECK(exp_rs41.configure(1,good,why,sizeof(why))); LS_NEAR(freq->num(freq),403.01,1e-8);
    tracks->opt[0].set_num(&tracks->opt[0],3); LS_EQ_INT(rs41_keep_hours(),3);
    tracks->opt[1].set(&tracks->opt[1],0); LS_CHECK(!rs41_show_map());
    tracks->opt[1].set(&tracks->opt[1],1); LS_CHECK(rs41_show_map());
}
