#include "ls_test.h"
#include "ls_experiments.h"
#include "uat_stream_rx.h"
#include "uat_settings.h"
#include "esp_timer.h"
#include <string.h>
extern const ls_experiment_t exp_uat;
static ls_fsk_cfg_t config;
static unsigned incoming;
static bool fresh;
static int fault;
uint32_t ls_lora_caps(void) { return LS_LORA_CAP_FSK | LS_LORA_CAP_FSK_STREAM | LS_LORA_CAP_RX_WIDE; }
uint32_t ls_lora_fsk_bw_snap(uint32_t hz) { return hz <= 2222222 ? 2222222 : 2666667; }
esp_err_t ls_lora_fsk_begin(const ls_fsk_cfg_t *c) { config=*c; return ESP_OK; }
esp_err_t ls_lora_fsk_end(void) { return ESP_OK; }
int ls_lora_fsk_poll(uint8_t *b,size_t n,float *r) { (void)b;(void)n;(void)r;return 0; }
esp_err_t ls_lora_rssi_inst(float *r) { *r=-90; return ESP_OK; }
int ls_lora_fsk_stream_read(uint8_t *b,size_t n,bool *restart)
{
    LS_CHECK(n>=256); *restart=fresh; fresh=false;
    if (fault) { int f=fault; fault=0; return f; }
    unsigned got = incoming < n ? incoming : n;
    memset(b,0x35,got); incoming-=got; return got;
}
const char *ls_exp_hw_radio_busy(void) { return NULL; }
bool ls_exp_hw_radio_take(void) { return true; }
void ls_exp_hw_radio_give(void) {}
bool ls_exp_hw_wake(void) { return false; }
LS_CASE(schema_validation_is_atomic_and_order_independent)
{
    const char *good="{\"sample_rate\":1500000, \"mode\":\"STREAM\",\"version\":1}";
    uat_settings_t out={false,42};
    LS_CHECK(uat_settings_parse(good,strlen(good),&out));
    LS_CHECK(out.stream); LS_EQ_UINT(out.rate,1500000);
    const char *bad[]={
        "{\"version\":2,\"mode\":\"STREAM\",\"sample_rate\":2000000}",
        "{\"version\":1,\"mode\":\"STREAM\",\"sample_rate\":1000000}",
        "{\"version\":1,\"mode\":\"TX\",\"sample_rate\":2000000}",
        "{\"version\":1,\"version\":1,\"sample_rate\":2000000}",
        "{\"version\":1,\"mode\":\"PACKET\",\"sample_rate\":02000000}",
        "{\"version\":1,\"mode\":\"PACKET\",\"sample_rate\":2000000}x",
        "{\"version\":1,\"mode\":\"PACKET\",\"sample_rate\":2000000,\"tx\":1}"
    };
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        LS_CHECK(!uat_settings_parse(bad[i],strlen(bad[i]),&out));
        LS_EQ_UINT(out.rate,1500000); LS_CHECK(out.stream);
    }
}
LS_CASE(reader_counts_byte_rate_restarts_faults_and_ring_backpressure)
{
    ls_shim_time_set(0); LS_CHECK(uat_stream_rx_start(2000000));
    incoming=256;fresh=true;LS_EQ_INT(uat_stream_rx_read(),256);
    LS_CHECK(uat_stream_rx_decode()); LS_CHECK(!uat_stream_rx_decode());
    ls_shim_time_advance(1000000); incoming=256; uat_stream_rx_read();
    uat_stream_rx_stats_t v; uat_stream_rx_snapshot(&v);
    LS_EQ_UINT(v.bytes_sec,512);LS_EQ_UINT(v.bytes,512);LS_EQ_UINT(v.reads,2);
    LS_EQ_UINT(v.restarts,1);LS_EQ_UINT(v.seams,1);LS_EQ_UINT(v.decoded.seams,1);
    while(uat_stream_rx_decode()) {}
    for(unsigned i=0;i<UAT_HARD_SLOTS+1;++i) { incoming=256;uat_stream_rx_read(); }
    uat_stream_rx_snapshot(&v);LS_EQ_UINT(v.drops,1);LS_EQ_UINT(v.seams,2);
    while(uat_stream_rx_decode()) {}
    incoming=13;uat_stream_rx_read();uat_stream_rx_decode();
    uat_stream_rx_snapshot(&v);LS_EQ_UINT(v.decoded.seams,2);
    fault=-1;uat_stream_rx_read(); incoming=1;uat_stream_rx_read();uat_stream_rx_decode();
    uat_stream_rx_snapshot(&v);LS_EQ_UINT(v.faults,1);LS_EQ_UINT(v.decoded.seams,3);
    ls_shim_time_advance(1000000);uat_stream_rx_read();
    ls_shim_time_advance(1000000);uat_stream_rx_read();
    uat_stream_rx_snapshot(&v);LS_EQ_UINT(v.bytes_sec,0);
    uat_stream_rx_stop();
}
LS_CASE(options_and_console_share_modes_and_validated_rates)
{
    ls_exp_forget(); ls_exp_register(&exp_uat);
    char *cmd[]={"exp","uat","stream","1500"};
    LS_EQ_INT(ls_exp_console(4,cmd),0);
    LS_CHECK(config.stream);LS_EQ_UINT(config.bitrate,1500000);
    LS_EQ_UINT(config.freq_hz,978000000);LS_EQ_UINT(config.deviation_hz,312500);
    LS_EQ_UINT(config.sync_bits,8);LS_EQ_UINT(config.sync_word >> 24,0xCC);LS_EQ_UINT(config.sync_word & 0xFFFFFFu,0); /* the driver reads the top sync_bits */
    LS_EQ_UINT(config.bandwidth_hz,2222222);LS_CHECK(!config.stream_sync_prefix);
    LS_EQ_UINT(config.rx_boost_step,0);LS_EQ_UINT(config.rx_gain_step,0);
    LS_EQ_INT(exp_uat.n_opts,2);
    exp_uat.opts[1].set(&exp_uat.opts[1],0);ls_exp_service();
    LS_EQ_UINT(config.bitrate,2000000);LS_EQ_UINT(config.bandwidth_hz,2666667);
    char why[80], *bug[]={"stream","1041.667"}, *invalid[]={"stream","1000"};
    LS_CHECK(exp_uat.configure(2,bug,why,sizeof(why)));ls_exp_service();
    LS_EQ_UINT(config.bitrate,1041667);
    LS_CHECK(!exp_uat.configure(2,invalid,why,sizeof(why)));
    exp_uat.opts[0].set(&exp_uat.opts[0],0);ls_exp_service();
    LS_CHECK(!config.stream);LS_EQ_UINT(config.sync_bits,32);
    char *packet[]={"exp","uat","packet"};LS_EQ_INT(ls_exp_console(3,packet),0);
    ls_exp_stop();ls_exp_service();
}
