#ifndef P25_TG_OBSERVED_H
#define P25_TG_OBSERVED_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define P25_TG_OBSERVED_MAX 32
/* Minutes of per-row activity kept for sparklines: a ring of hit counts, one
   bucket per minute of the record clock, each saturating at 255. */
#define P25_TG_OBSERVED_MINUTES 16
enum { P25_TG_SEEN_LCW = 1, P25_TG_SEEN_HDU = 2, P25_TG_SEEN_GRANT = 4 };
typedef struct {
    uint64_t channel_hz;
    uint32_t last_seen_ms;
    uint32_t hits;
    uint16_t nac;
    uint16_t talkgroup;
    uint8_t sources;
    /* minute[m % MINUTES] is minute m's hits, for the MINUTES minutes up to
       and including newest_minute (now_ms / 60000 of the last record). */
    uint32_t newest_minute;
    uint8_t minute[P25_TG_OBSERVED_MINUTES];
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
/* Hits in the minute `ago` minutes before now_ms (0 is the current minute);
   0 where the row holds no history. Reads a snapshot row, takes no lock. */
unsigned p25_tg_observed_minute_hits(const p25_tg_observed_row_t *row,
                                     uint32_t now_ms, unsigned ago);
#ifdef __cplusplus
}
#endif
#endif
