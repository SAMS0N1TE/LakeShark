#include "rec_watch.h"
#include <limits.h>
#include <string.h>

static uint32_t magnitude(int32_t v) { return v < 0 ? (uint32_t)(-(int64_t)v) : (uint32_t)v; }
int rec_watch_receiver_want(int visible_mode, int rec_mode, bool enabled)
{ return visible_mode < 0 && enabled ? rec_mode : visible_mode; }
int rec_watch_receiver_want_source(int visible_mode, int rec_mode, bool enabled, rec_source_t source)
{
    if(source==REC_SOURCE_CC1101)return visible_mode==rec_mode?-1:visible_mode;
    return rec_watch_receiver_want(visible_mode,rec_mode,enabled);
}
static bool matches(const rec_watch_record_t *r, uint32_t hz, const int32_t *p, int n)
{
    if (!r->event.id || r->event.frequency != hz) return false;
    rec_ook24_t previous, incoming;
    if (rec_decode_ook24(p,n,&incoming) && rec_decode_ook24(r->pulse,r->event.edges,&previous))
        return incoming.value == previous.value;
    if (r->event.edges != n) return false;
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
    return rec_watch_observe_from(c, REC_SOURCE_RTL, hz, p, n, boot, ms, peak, reason, novel);
}
int rec_watch_observe_from(rec_watch_catalog_t *c, rec_source_t source, uint32_t hz,
    const int32_t *p, int n, uint32_t boot, uint64_t ms,
    int peak, int reason, bool *novel)
{
    if (novel) *novel = false;
    if ((source != REC_SOURCE_RTL && source != REC_SOURCE_CC1101) || !c || !p || n < 6 || n > REC_WATCH_EDGES || !hz ||
        c->sequence == UINT64_MAX) return -1;
    uint64_t span = 0;
    for (int i = 0; i < n; i++) {
        if (!p[i] || p[i] == INT32_MIN || (i && (p[i]>0)==(p[i-1]>0))) return -1;
        span += magnitude(p[i]);
    }
    if (span > UINT32_MAX) return -1;
    if (!c->archive_id) c->archive_id=((uint64_t)boot<<32)|hz;
    rec_ook24_t incoming;
    int incoming_rank=rec_decode_ook24(p,n,&incoming)?3:1;
    int slot = -1, slot_rank=99;
    for (int i = 0; i < REC_WATCH_SLOTS; i++) {
        if (c->record[i].event.source == source && matches(&c->record[i], hz, p, n)) {
            rec_watch_event_t *e = &c->record[i].event;
            if (e->count < UINT32_MAX) e->count++;
            e->last_ms = ms; e->last_boot = boot; e->order = ++c->sequence;
            if (peak > e->peak) e->peak = peak;
            return i;
        }
        const rec_watch_record_t *candidate=&c->record[i];
        rec_ook24_t decoded;
        int rank=!candidate->event.id?0:
            rec_decode_ook24(candidate->pulse,candidate->event.edges,&decoded)?3:
            candidate->event.count>1?2:1;
        /* One-off noise cannot evict a repeated or decoded observation. */
        if (!candidate->event.pinned && rank<=incoming_rank &&
            (slot<0 || rank<slot_rank || (rank==slot_rank &&
             candidate->event.order<c->record[slot].event.order))) {slot=i;slot_rank=rank;}
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
        .peak=peak, .end_reason=(uint8_t)reason, .source=(uint8_t)source};
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

void rec_watch_filter_status(rec_watch_status_t *out,rec_source_t source)
{
    if(!out)return;
    int count=0;
    for(int i=0;i<out->count && i<REC_WATCH_SLOTS;i++) if(out->event[i].source==source) {
        out->event[count]=out->event[i];out->decoded[count]=out->decoded[i];
        if(count!=i)memcpy(out->preview[count],out->preview[i],sizeof(out->preview[count]));
        count++;
    }
    out->count=count;
}

int rec_watch_filter_pulses(int32_t *pulse,int edges,uint32_t min_us)
{
    if(!pulse || edges<0 || edges>REC_WATCH_EDGES)return 0;
    int out=0;
    for(int i=0;i<edges;i++) {
        if(!pulse[i] || pulse[i]==INT32_MIN)return 0;
        uint32_t width=magnitude(pulse[i]);
        if(width<min_us) {
            if(out) {
                int64_t joined=(int64_t)pulse[out-1]+(pulse[out-1]>0?(int64_t)width:-(int64_t)width);
                if(joined>INT32_MAX || joined< -INT32_MAX)return 0;
                pulse[out-1]=(int32_t)joined;
            }
        } else if(out && (pulse[out-1]>0)==(pulse[i]>0)) {
            int64_t joined=(int64_t)pulse[out-1]+pulse[i];
            if(joined>INT32_MAX || joined< -INT32_MAX)return 0;
            pulse[out-1]=(int32_t)joined;
        } else pulse[out++]=pulse[i];
    }
    if(out && pulse[0]<0){memmove(pulse,pulse+1,(--out)*sizeof(*pulse));}
    return out;
}
