/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LS_CELL_IQ_H
#define LS_CELL_IQ_H
#include <stdbool.h>
#include <stdint.h>
#include "ls_imu.h"
#include "lte_sync.h"
#include "lte_mib.h"
#include "cell_session.h"
#include "radio_endpoint.h"

typedef enum { CELL_IQ_IDLE, CELL_IQ_RECEIVING, CELL_IQ_FILTERING,
               CELL_IQ_SYNCHRONIZING, CELL_IQ_DECODING_MIB, CELL_IQ_SAVING, CELL_IQ_WAITING } cell_iq_phase_t;
typedef struct {
    bool busy,complete,imu_valid,gps_valid;
    bool automatic, stopping, raw_saved, metadata_saved, multi;
    uint32_t sequence, attempts, decoded, wait_ms;
    uint64_t raw_bytes, started_us;
    int64_t unix_time;
    float speed_kts, hdop;
    unsigned satellites;
    bool end_imu_valid, end_gps_valid;
    ls_imu_sample_t end_imu;
    double end_latitude, end_longitude;
    bool lte_checked,lte_found;
    bool mib_checked,mib_found;
    cell_iq_phase_t phase;
    uint32_t hz,rate,bytes,elapsed_us,active_hz;
    uint32_t analysis_ms;
    uint32_t acquire_ms,resample_ms,sync_ms,raw_save_ms;
    bool config_valid,raw_stats_valid;
    ls_radio_iq_config_t config;
    float dc_i_codes,dc_q_codes,ac_power_codes2,rail_fraction;
    lte_sync_result_t lte;
    lte_mib_result_t mib;
    uint32_t mib_ms;
    uint64_t dropped;
    ls_radio_iq_health_t radio_health;
    ls_radio_err_t radio_health_result, stop_result;
    ls_imu_sample_t imu;
    float heading;
    double latitude,longitude;
    uint8_t envelope[64];
    char raw_file[80];
    char message[96];
} cell_iq_status_t;
#ifdef __cplusplus
extern "C" {
#endif
int cell_iq_command(int argc, char **argv);
bool cell_iq_hackrf_begin(uint32_t hz,uint32_t rate,uint32_t ms);
bool cell_iq_auto_begin(uint32_t hz, bool multi);
void cell_iq_stop(void);
void cell_iq_get_status(cell_iq_status_t *out);
#ifdef __cplusplus
}
#endif
#endif
