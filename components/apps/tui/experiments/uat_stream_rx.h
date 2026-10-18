/* Receive-only UAT stream workers; snapshots are safe on UI/console tasks. */
#ifndef UAT_STREAM_RX_H
#define UAT_STREAM_RX_H
#include "uat_hard.h"
#include "ls_lora.h"
typedef struct {
    uint64_t bytes;
    uint32_t reads, restarts, seams, drops, faults, bytes_sec, rate;
    uat_hard_stats_t decoded;
} uat_stream_rx_stats_t;
bool uat_stream_rx_start(uint32_t rate);
void uat_stream_rx_stop(void);
void uat_stream_rx_snapshot(uat_stream_rx_stats_t *);
/* Worker steps, also used by host tests/simulator. Never call from device UI. */
int uat_stream_rx_read(void);
bool uat_stream_rx_decode(void);
void uat_stream_rx_config(ls_fsk_cfg_t *, uint32_t rate);
bool uat_stream_rate_valid(uint32_t);
#endif
