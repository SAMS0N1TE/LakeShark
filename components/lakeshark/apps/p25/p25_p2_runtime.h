#ifndef LS_P25_P2_RUNTIME_H
#define LS_P25_P2_RUNTIME_H
#include "dsp_pipeline.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Both receivers hand over IQ: the SDR as u8 at its own rate, the LR2021
   through p25_lr_frontend as float at 240 kHz. */
bool p25_p2_enabled(void);
void p25_p2_enable(bool on);
bool p25_p2_config(uint32_t wacn, uint16_t system, uint16_t nac, unsigned slot);
void p25_p2_describe(char *text, unsigned capacity);
bool p25_p2_rx(dsp_state_t *dsp, const uint8_t *iq, int length, uint32_t hz,
               uint32_t wacn, uint16_t system, uint16_t nac);
bool p25_p2_rx_float(dsp_state_t *dsp, const float *iq, int pairs, uint32_t hz,
                     uint32_t wacn, uint16_t system, uint16_t nac);
void p25_p2_stop(void);
uint32_t p25_p2_config_generation(void);
#ifdef __cplusplus
}
#endif
#endif
