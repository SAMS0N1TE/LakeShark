#include "rec_watch.h"
#include <limits.h>
#include <string.h>

static uint32_t magnitude(int32_t v) { return v < 0 ? (uint32_t)(-(int64_t)v) : (uint32_t)v; }
int rec_watch_receiver_want(int visible_mode, int rec_mode, bool enabled)
{ return visible_mode < 0 && enabled ? rec_mode : visible_mode; }
static bool matches(const rec_watch_record_t *r, uint32_t hz, const int32_t *p, int n)
{
    if (!r->event.id || r->event.frequency != hz || r->event.edges != n) return false;
    for (int i = 0; i < n; i++) {
        if ((p[i] < 0) != (r->pulse[i] < 0)) return false;
        uint32_t a = magnitude(p[i]), b = magnitude(r->pulse[i]);
        uint32_t tolerance = b / 8;
        if (tolerance < 30) tolerance = 30;
        if ((a > b ? a-b : b-a) > tolerance) return false;
    }
    return true;
}
int rec_watch_observe(rec_watch_catalog_t *c, uint32_t hz,
    const int32_t *p, int n, uint32_t boot, uint64_t ms,
    int peak, int reason, bool *novel)
{
    if (novel) *novel = false;
    if (!c || !p || n < 6 || n > REC_WATCH_EDGES || !hz ||
        c->sequence == UINT64_MAX) return -1;
    uint64_t span = 0;
    for (int i = 0; i < n; i++) {
        if (!p[i] || p[i] == INT32_MIN) return -1;
        span += magnitude(p[i]);
    }
    if (span > UINT32_MAX) return -1;
    if (!c->archive_id) c->archive_id=((uint64_t)boot<<32)|hz;
    int slot = -1;
    for (int i = 0; i < REC_WATCH_SLOTS; i++) {
        if (matches(&c->record[i], hz, p, n)) {
            rec_watch_event_t *e = &c->record[i].event;
            if (e->count < UINT32_MAX) e->count++;
            e->last_ms = ms; e->last_boot = boot; e->order = ++c->sequence;
            if (peak > e->peak) e->peak = peak;
            return i;
        }
        if (!c->record[i].event.pinned && (slot < 0 ||
            c->record[i].event.order < c->record[slot].event.order)) slot = i;
    }
    if (slot < 0 || c->next_id == UINT32_MAX) {
        if (c->rejected < UINT32_MAX) c->rejected++;
        return -1;
    }
    rec_watch_record_t *r = &c->record[slot];
    memset(r, 0, sizeof(*r));
    r->event = (rec_watch_event_t){.id=++c->next_id, .frequency=hz, .count=1,
        .first_boot=boot, .last_boot=boot, .first_ms=ms, .last_ms=ms,
        .order=++c->sequence, .span_us=(uint32_t)span, .edges=(uint16_t)n,
        .peak=peak, .end_reason=(uint8_t)reason};
    memcpy(r->pulse, p, (size_t)n * sizeof(*p));
    if (novel) *novel = true;
    return slot;
}
bool rec_watch_pin(rec_watch_catalog_t *c, uint32_t id, bool pin)
{
    if (!c || !id || c->sequence == UINT64_MAX) return false;
    for (int i=0; i<REC_WATCH_SLOTS; i++) if (c->record[i].event.id == id) {
        c->record[i].event.pinned = pin;
        ++c->sequence;
        return true;
    }
    return false;
}
uint32_t rec_watch_crc(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t crc = UINT32_MAX;
    while (len--) {
        crc ^= *p++;
        for (int b=0; b<8; b++) crc = (crc >> 1) ^ (0xedb88320u & (0u-(crc&1u)));
    }
    return ~crc;
}
