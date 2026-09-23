#ifndef LS_P25_P2_RUNTIME_H
#define LS_P25_P2_RUNTIME_H
#include "dsp_pipeline.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
bool p25_p2_enabled(void);
void p25_p2_enable(bool on);
bool p25_p2_config(uint32_t wacn, uint16_t system, uint16_t nac, unsigned slot);
void p25_p2_describe(char *text, unsigned capacity);
bool p25_p2_rx(dsp_state_t *dsp, const uint8_t *iq, int length, uint32_t hz,
               uint32_t wacn, uint16_t system, uint16_t nac);
void p25_p2_stop(void);
/* The configuration p25_p2_config last set. p25_p2_status_for() in
   p25_p2_runtime_status.h reads the decoder once it has applied it. */
uint32_t p25_p2_config_generation(void);
#ifdef __cplusplus
}
#endif
#endif
