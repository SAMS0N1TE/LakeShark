/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LS_CELL_IQ_H
#define LS_CELL_IQ_H
#include <stdbool.h>
#include <stdint.h>
#include "ls_imu.h"
#include "lte_sync.h"

typedef enum { CELL_IQ_IDLE, CELL_IQ_RECEIVING, CELL_IQ_FILTERING,
               CELL_IQ_SYNCHRONIZING, CELL_IQ_SAVING } cell_iq_phase_t;
typedef struct {
    bool busy,complete,imu_valid,gps_valid;
    bool lte_checked,lte_found;
    cell_iq_phase_t phase;
    uint32_t hz,rate,bytes,elapsed_us;
    uint32_t analysis_ms;
    lte_sync_result_t lte;
    uint64_t dropped;
    ls_imu_sample_t imu;
    float heading;
    double latitude,longitude;
    uint8_t envelope[64];
    char message[96];
} cell_iq_status_t;
#ifdef __cplusplus
extern "C" {
#endif
int cell_iq_command(int argc, char **argv);
bool cell_iq_hackrf_begin(uint32_t hz,uint32_t rate,uint32_t ms);
void cell_iq_get_status(cell_iq_status_t *out);
#ifdef __cplusplus
}
#endif
#endif
