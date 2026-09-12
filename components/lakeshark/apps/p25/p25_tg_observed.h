#ifndef P25_TG_OBSERVED_H
#define P25_TG_OBSERVED_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define P25_TG_OBSERVED_MAX 32
enum { P25_TG_SEEN_LCW = 1, P25_TG_SEEN_HDU = 2, P25_TG_SEEN_GRANT = 4 };
typedef struct {
    uint64_t channel_hz;
    uint32_t last_seen_ms;
    uint32_t hits;
    uint16_t nac;
    uint16_t talkgroup;
    uint8_t sources;
} p25_tg_observed_row_t;
typedef struct {
    uint32_t generation;
    uint8_t count;
    p25_tg_observed_row_t rows[P25_TG_OBSERVED_MAX];
} p25_tg_observed_snapshot_t;

bool p25_tg_observed_record(uint64_t channel_hz, uint16_t nac,
    uint16_t talkgroup, uint8_t source, uint32_t now_ms);
bool p25_tg_observed_read(p25_tg_observed_snapshot_t *out);
bool p25_tg_observed_clear(void);
#ifdef __cplusplus
}
#endif
#endif
