#ifndef LORAFAM_MODULE_FAKE_H
#define LORAFAM_MODULE_FAKE_H
#include "ls_lora_lr20xx.h"
extern lr20xx_engine_t lf_engine;
extern unsigned lf_preset, lf_channel, lf_fec, lf_restarts;
extern int lf_result;
extern uint8_t lf_data[255];
extern lr20xx_engine_packet_t lf_packet;
extern esp_err_t lf_begin_error;
#endif
