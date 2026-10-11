#ifndef P25_SPLIT_H
#define P25_SPLIT_H

#include "scan_ctrl.h"

/* Owner-task only. The LR stream has its own framing and identifier tables;
 * RTL voice acquisition never resets these. Put this object in PSRAM. */
#define P25_SPLIT_LOSS_US 5000000LL
#define P25_SPLIT_SOLID_US 2000000LL
typedef struct {
    dsd_state cc;
    uint64_t sync;
    uint8_t frame[33 + P25_TSDU_DIBIT_COUNT];
    unsigned frame_n;
    bool collecting, inverted, enabled, available, active;
    int64_t last_good_us, last_quality_us, solid_since_us, window_us;
    unsigned good, bad, crc_percent, fallbacks, recoveries, skipped;
    p25_scan_reason_t reason;
    uint64_t control_hz;
} p25_split_t;

void p25_split_init(p25_split_t *s, uint64_t control_hz, int64_t now_us);
/* Returns true on a routing transition. Loss immediately retires the call
 * through the usual follower callback and tunes RTL back to control. */
bool p25_split_service(p25_split_t *s, p25_grant_follower_t *f,
                       bool enabled, bool available, int64_t now_us);
bool p25_split_tsdu(p25_split_t *s, p25_scan_ctrl_t *sc,
                    p25_grant_follower_t *f, uint16_t nac,
                    const uint8_t *wire, size_t n, int64_t now_us);
bool p25_split_feed(p25_split_t *s, p25_scan_ctrl_t *sc,
                    p25_grant_follower_t *f, uint8_t dibit, int64_t now_us);
const char *p25_split_reason(p25_scan_reason_t reason);
#endif
