#ifndef CALL_RECORDER_H
#define CALL_RECORDER_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

#define CALL_PCM_SAMPLES 512
typedef struct {
    int64_t time;
    uint32_t hz, talkgroup, source, rate, minimum_ms;
    double lat, lon;
    bool gps;
} call_meta_t;
typedef enum { CALL_BEGIN, CALL_PCM, CALL_END, CALL_ABORT } call_event_t;
typedef struct {
    call_event_t event;
    call_meta_t meta;
    unsigned count;
    int16_t pcm[CALL_PCM_SAMPLES];
} call_block_t;
/* One decoder produces, one SD task consumes. The last slot is reserved for
   a closing event, so overflow cannot join two calls into one file. */
typedef struct {
    call_block_t *blocks;
    unsigned capacity;
    atomic_uint head, tail, drops;
    atomic_bool active;
    call_meta_t current;
    uint32_t last_open_ms;
    bool poisoned;
} call_recorder_t;
void call_recorder_init(call_recorder_t *r, call_block_t *blocks, unsigned capacity);
bool call_recorder_begin(call_recorder_t *r, const call_meta_t *meta, bool clear);
void call_recorder_pcm(call_recorder_t *r, const int16_t *pcm, unsigned count);
void call_recorder_end(call_recorder_t *r, bool discard);
void call_recorder_gate(call_recorder_t *r, const call_meta_t *meta,
                        bool open, bool clear, uint32_t now_ms, unsigned hang_ms);
const call_block_t *call_recorder_peek(call_recorder_t *r);
void call_recorder_consume(call_recorder_t *r);
void call_wav_header(uint8_t out[44], uint32_t rate, uint32_t bytes);
/* Days are UTC epoch days; keep=0 disables automatic deletion. */
bool call_day_expired(int64_t day, int64_t today, unsigned keep);
#endif
