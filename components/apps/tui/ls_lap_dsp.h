#ifndef LS_LAP_DSP_H
#define LS_LAP_DSP_H
#include "ls_lap_api.h"
int ls_lap_dsp_attach(int channel, const lap_dsp_config_t *config);
void ls_lap_dsp_submit(int channel, const int16_t *iq, unsigned pairs, const lap_iq_meta2_t *meta);
void ls_lap_dsp_end(void);
void ls_lap_dsp_channel_close(int channel);
void ls_lap_dsp_gap(int channel);
bool ls_lap_dsp_service(void);
bool ls_lap_dsp_on_worker(void);
bool ls_lap_dsp_script(const char *command);
#endif
