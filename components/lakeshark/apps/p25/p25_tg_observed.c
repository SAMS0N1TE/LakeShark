#include "p25_tg_observed.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define OBS_RAM EXT_RAM_BSS_ATTR
#else
#define OBS_RAM
#endif
/* One bounded PSRAM store; no per-frame allocations/persistence and no large
 * RX stack buffer. An occupied lock drops this observation/read, never waits
 * for a preempted task on the same core. Payload copies occur under the lock,
 * unlike an unsynchronised C seqlock read. */
static OBS_RAM p25_tg_observed_snapshot_t s_seen;
static unsigned char s_lock;
static bool take(void) { return !__atomic_test_and_set(&s_lock, __ATOMIC_ACQUIRE); }
static void give(void) { __atomic_clear(&s_lock, __ATOMIC_RELEASE); }
bool p25_tg_observed_clear(void)
{
    if (!take()) return false;
    uint32_t next = s_seen.generation + 1;
    memset(&s_seen, 0, sizeof(s_seen));
    s_seen.generation = next;
    give();
    return true;
}
bool p25_tg_observed_read(p25_tg_observed_snapshot_t *out)
{
    if (!out || !take()) return false;
    *out = s_seen;
    give();
    return true;
}
bool p25_tg_observed_record(uint64_t hz, uint16_t nac, uint16_t tg,
                           uint8_t source, uint32_t now)
{
    if (!hz || hz > 6000000000ULL || nac > 0xfff || !tg || tg == 0xffff ||
        !(source & 7) || (source & ~7)) return false;
    if (!take()) return false;
    unsigned index = s_seen.count;
    for (unsigned i = 0; i < s_seen.count; ++i) {
        p25_tg_observed_row_t *r = &s_seen.rows[i];
        if (r->channel_hz == hz && r->nac == nac && r->talkgroup == tg) {
            index = i; break;
        }
    }
    if (index == s_seen.count) {
        if (s_seen.count < P25_TG_OBSERVED_MAX) ++s_seen.count;
        else {
            index = 0;
            for (unsigned i = 1; i < P25_TG_OBSERVED_MAX; ++i)
                if ((uint32_t)(now - s_seen.rows[i].last_seen_ms) >
                    (uint32_t)(now - s_seen.rows[index].last_seen_ms)) index = i;
        }
        memset(&s_seen.rows[index], 0, sizeof(s_seen.rows[index]));
        s_seen.rows[index].channel_hz = hz;
        s_seen.rows[index].nac = nac;
        s_seen.rows[index].talkgroup = tg;
    }
    p25_tg_observed_row_t *row = &s_seen.rows[index];
    /* Advance the minute ring, clearing the minutes nothing was heard in. A
       row new to its slot was zeroed above, so its newest_minute of 0 only
       clears buckets that are already clear. */
    const uint32_t minute = now / 60000u;
    if (minute != row->newest_minute) {
        uint32_t gap = minute - row->newest_minute;
        if (gap > P25_TG_OBSERVED_MINUTES) gap = P25_TG_OBSERVED_MINUTES;
        for (uint32_t k = 0; k < gap; ++k)
            row->minute[(minute - k) % P25_TG_OBSERVED_MINUTES] = 0;
        row->newest_minute = minute;
    }
    if (row->minute[minute % P25_TG_OBSERVED_MINUTES] != UINT8_MAX)
        ++row->minute[minute % P25_TG_OBSERVED_MINUTES];
    row->last_seen_ms = now;
    if (row->hits != UINT32_MAX) ++row->hits;
    row->sources |= source;
    ++s_seen.generation;
    give();
    return true;
}

unsigned p25_tg_observed_minute_hits(const p25_tg_observed_row_t *row,
                                     uint32_t now_ms, unsigned ago)
{
    if (!row || ago >= P25_TG_OBSERVED_MINUTES) return 0;
    const uint32_t now_minute = now_ms / 60000u;
    if (ago > now_minute) return 0;
    const uint32_t m = now_minute - ago;
    /* Only minutes inside the ring the row last advanced, never one after
       it: a record clock that went backwards reads as no history. */
    if (m > row->newest_minute ||
        row->newest_minute - m >= P25_TG_OBSERVED_MINUTES) return 0;
    return row->minute[m % P25_TG_OBSERVED_MINUTES];
}
