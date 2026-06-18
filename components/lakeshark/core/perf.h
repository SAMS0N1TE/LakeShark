
#ifndef PERF_H
#define PERF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void perf_init(void);
void perf_start_heartbeat_task(const char *app_name, uint32_t period_ms);

void perf_count_bytes(uint32_t n);
void perf_count_msg_good(void);
void perf_count_msg_bad(void);

void perf_count_burst(void);

void perf_mark_good_msg(int64_t now_us);
void perf_mark_position(int64_t now_us);

void perf_set_mag(int avg, int peak);
void perf_set_active_count(int n);

uint32_t perf_get_bytes_per_sec(void);
int      perf_get_msgs_per_sec(void);
int      perf_get_msgs_total(void);
int      perf_get_crc_good(void);
int      perf_get_crc_err(void);
int      perf_get_burst_total(void);
int      perf_get_bursts_per_sec(void);
int      perf_get_mag_avg(void);
int      perf_get_mag_peak(void);
int      perf_get_active_count(void);

int64_t  perf_get_last_good_us(void);
int64_t  perf_get_last_burst_us(void);
int64_t  perf_get_last_position_us(void);

#define PERF_HISTORY_LEN 60
const uint16_t *perf_history_bursts(void);
const uint16_t *perf_history_good(void);
const uint8_t  *perf_history_mag_avg(void);

#ifdef __cplusplus
}
#endif

#endif
