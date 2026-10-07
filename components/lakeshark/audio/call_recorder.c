#include "call_recorder.h"
#include <string.h>

void call_recorder_init(call_recorder_t *r, call_block_t *blocks, unsigned capacity)
{
    memset(r, 0, sizeof(*r));
    r->blocks = blocks; r->capacity = capacity;
    atomic_init(&r->head, 0); atomic_init(&r->tail, 0);
    atomic_init(&r->drops, 0); atomic_init(&r->active, false);
}
static call_block_t *room(call_recorder_t *r, bool closing)
{
    unsigned head = atomic_load_explicit(&r->head, memory_order_relaxed);
    unsigned tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    if (!r->blocks || r->capacity < 3 || head - tail >= r->capacity - (closing ? 0u : 1u)) return NULL;
    return &r->blocks[head % r->capacity];
}
static void publish(call_recorder_t *r)
{
    atomic_fetch_add_explicit(&r->head, 1, memory_order_release);
}
bool call_recorder_begin(call_recorder_t *r, const call_meta_t *meta, bool clear)
{
    if (!clear || r->poisoned || atomic_load(&r->active)) return false;
    call_block_t *b = room(r, false);
    if (!b) { atomic_fetch_add(&r->drops, 1); return false; }
    r->current = *meta;
    b->event = CALL_BEGIN; b->meta = *meta; b->count = 0;
    publish(r); atomic_store(&r->active, true);
    return true;
}
void call_recorder_pcm(call_recorder_t *r, const int16_t *pcm, unsigned count)
{
    if (!atomic_load(&r->active) || !pcm) return;
    while (count) {
        unsigned n = count > CALL_PCM_SAMPLES ? CALL_PCM_SAMPLES : count;
        call_block_t *b = room(r, false);
        if (!b) { atomic_fetch_add(&r->drops, count); return; }
        b->event = CALL_PCM; b->count = n;
        memcpy(b->pcm, pcm, n * sizeof(*pcm)); publish(r);
        pcm += n; count -= n;
    }
}
void call_recorder_end(call_recorder_t *r, bool discard)
{
    if (atomic_load(&r->active)) {
        call_block_t *b = room(r, true);
        if (b) { b->event = discard ? CALL_ABORT : CALL_END; b->count = 0; publish(r); }
        atomic_store(&r->active, false);
    }
    r->poisoned = discard;
}
void call_recorder_gate(call_recorder_t *r, const call_meta_t *meta,
                        bool open, bool clear, uint32_t now_ms, unsigned hang_ms)
{
    if (!clear) { call_recorder_end(r, true); return; }
    if (open && (uint32_t)(now_ms - r->last_open_ms) >= hang_ms)
        call_recorder_end(r, false);
    if ((atomic_load(&r->active) || r->poisoned) && (r->current.hz != meta->hz ||
        r->current.talkgroup != meta->talkgroup || r->current.rate != meta->rate))
        call_recorder_end(r, false);
    if (open) {
        call_recorder_begin(r, meta, true);
        r->last_open_ms = now_ms;
    } else if ((uint32_t)(now_ms - r->last_open_ms) >= hang_ms) {
        call_recorder_end(r, false);
    }
}
const call_block_t *call_recorder_peek(call_recorder_t *r)
{
    unsigned tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    if (tail == atomic_load_explicit(&r->head, memory_order_acquire)) return NULL;
    return &r->blocks[tail % r->capacity];
}
void call_recorder_consume(call_recorder_t *r)
{
    atomic_fetch_add_explicit(&r->tail, 1, memory_order_release);
}
static void le32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
void call_wav_header(uint8_t out[44], uint32_t rate, uint32_t bytes)
{
    static const uint8_t base[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E',
        'f','m','t',' ',16,0,0,0,1,0,1,0,0,0,0,0,0,0,0,0,2,0,16,0,'d','a','t','a'};
    memcpy(out, base, 44); le32(out + 4, bytes + 36); le32(out + 24, rate);
    le32(out + 28, rate * 2); le32(out + 40, bytes);
}
bool call_day_expired(int64_t day, int64_t today, unsigned keep)
{
    return keep && day >= 0 && today >= day && today - day >= keep;
}
