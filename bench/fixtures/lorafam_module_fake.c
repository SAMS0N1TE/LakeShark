/* Scripted session seam for the experiment UI/readout tests. */
#include "lorafam_module_fake.h"
#include "ls_experiments.h"
#include <string.h>
lr20xx_engine_t lf_engine;
unsigned lf_preset, lf_channel, lf_fec, lf_restarts;
int lf_result;
uint8_t lf_data[255];
lr20xx_engine_packet_t lf_packet;
esp_err_t lf_begin_error;
esp_err_t lr20xx_exp_begin(lr20xx_engine_t e,unsigned p,unsigned c,unsigned f)
{ lf_engine=e; lf_preset=p; lf_channel=c; lf_fec=f; return lf_begin_error; }
int lr20xx_exp_poll(uint8_t *buf,size_t n,lr20xx_engine_packet_t *out)
{ *out=lf_packet; if(lf_result>0 && (size_t)lf_result<=n) memcpy(buf,lf_data,lf_result); int r=lf_result; lf_result=0; return r; }
esp_err_t lr20xx_exp_end(void) { return ESP_OK; }
const ls_experiment_t *ls_exp_running(void) { return NULL; }
void ls_exp_start(const ls_experiment_t *e) { (void)e; lf_restarts++; }
