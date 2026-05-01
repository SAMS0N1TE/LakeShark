/*
 * perf.h - shared performance counters and heartbeat publisher.
 *
 * The decode loop calls perf_*() to update counters; a low-priority task
 * publishes EVT_HEARTBEAT at a fixed cadence so all consumers (TUI status
 * line, human log, event stream) see the same numbers.
 */
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
/* Bumped each time the demod's preamble pattern-match fires, regardless
 * of whether the message later passes CRC. The ratio crc_good/burst is
 * the most direct SNR indicator: high = clean reception, low = noise
 * tripping the preamble matcher. */
void perf_count_burst(void);
/* Mark the timestamp (esp_timer microseconds) of the most recent CRC-
 * good message and CRC-good position fix. Lets the diag page show "last
 * good msg N seconds ago" instead of just a counter. */
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

/* Last-event timestamps. Returns 0 if the event has never occurred. */
int64_t  perf_get_last_good_us(void);
int64_t  perf_get_last_burst_us(void);
int64_t  perf_get_last_position_us(void);

/* Rolling 1Hz history rings. The diag page sparklines read these.
 * Each ring holds PERF_HISTORY_LEN seconds of one-second-bucketed data,
 * with index 0 being the oldest sample and index PERF_HISTORY_LEN-1 the
 * most recent completed second. The current in-progress second isn't
 * exposed here. */
#define PERF_HISTORY_LEN 60
const uint16_t *perf_history_bursts(void);   /* count per second */
const uint16_t *perf_history_good(void);     /* CRC-good per second */
const uint8_t  *perf_history_mag_avg(void);  /* magnitude avg per second 0-255 */

#ifdef __cplusplus
}
#endif

#endif
