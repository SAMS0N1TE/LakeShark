#include "ls_test.h"
#include "call_recorder.h"
#include <limits.h>

static call_block_t blocks[8];
static call_recorder_t r;
static call_meta_t meta = {.time = 1791374400, .hz = 851000000, .talkgroup = 42, .rate = 8000};
static unsigned drain(call_event_t event)
{
    unsigned count = 0;
    const call_block_t *b;
    while ((b = call_recorder_peek(&r))) {
        if (b->event == event) count++;
        call_recorder_consume(&r);
    }
    return count;
}
LS_CASE(voice_boundaries_and_talkgroup_change)
{
    call_recorder_init(&r, blocks, 8);
    call_recorder_gate(&r, &meta, true, true, 100, 1500);
    call_recorder_gate(&r, &meta, true, true, 200, 1500);
    LS_CHECK(atomic_load(&r.active));
    call_meta_t other = meta; other.talkgroup = 43;
    call_recorder_gate(&r, &other, true, true, 300, 1500);
    LS_EQ_INT(drain(CALL_END), 1);
    call_recorder_end(&r, false);
    LS_EQ_INT(drain(CALL_END), 1);
    LS_CHECK(!atomic_load(&r.active));
}
LS_CASE(fm_hang_closes_once_and_reopening_keeps_the_call)
{
    call_recorder_init(&r, blocks, 8);
    call_recorder_gate(&r, &meta, true, true, 1000, 300);
    call_recorder_gate(&r, &meta, false, true, 1250, 300);
    LS_CHECK(atomic_load(&r.active));
    call_recorder_gate(&r, &meta, true, true, 1270, 300);
    LS_EQ_INT(drain(CALL_BEGIN), 1);
    call_recorder_gate(&r, &meta, false, true, 1570, 300);
    call_recorder_gate(&r, &meta, false, true, 1600, 300);
    LS_EQ_INT(drain(CALL_END), 1);
}
LS_CASE(overflow_drops_samples_and_reserves_end_marker)
{
    call_recorder_init(&r, blocks, 3);
    int16_t pcm[CALL_PCM_SAMPLES * 3] = {0};
    LS_CHECK(call_recorder_begin(&r, &meta, true));
    call_recorder_pcm(&r, pcm, CALL_PCM_SAMPLES * 3);
    LS_EQ_UINT(atomic_load(&r.drops), CALL_PCM_SAMPLES * 2);
    call_recorder_end(&r, false);
    LS_EQ_UINT(atomic_load(&r.head) - atomic_load(&r.tail), 3);
    LS_CHECK(!call_recorder_begin(&r, &meta, true));
    LS_EQ_INT(drain(CALL_END), 1);
    LS_CHECK(call_recorder_begin(&r, &meta, true));
}
LS_CASE(encrypted_never_begins_and_revokes_clear_call)
{
    call_recorder_init(&r, blocks, 8);
    LS_CHECK(!call_recorder_begin(&r, &meta, false));
    LS_EQ_INT(drain(CALL_BEGIN), 0);
    LS_CHECK(call_recorder_begin(&r, &meta, true));
    call_recorder_gate(&r, &meta, true, false, 100, 1500);
    LS_EQ_INT(drain(CALL_ABORT), 1);
    LS_CHECK(!call_recorder_begin(&r, &meta, true));
    call_recorder_end(&r, false);
    LS_CHECK(call_recorder_begin(&r, &meta, true));
}
LS_CASE(silence_gap_and_clock_wrap_preserve_boundaries)
{
    call_recorder_init(&r, blocks, 8);
    call_recorder_gate(&r, &meta, true, true, UINT32_MAX - 100, 300);
    call_recorder_gate(&r, &meta, false, true, 150, 300);
    LS_CHECK(atomic_load(&r.active));
    call_recorder_gate(&r, &meta, true, true, 300, 300);
    LS_EQ_INT(drain(CALL_END), 1);
    LS_CHECK(atomic_load(&r.active));
}
static uint32_t le32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
LS_CASE(wav_header_is_mono_pcm16_at_both_receiver_rates)
{
    for (unsigned rate = 8000; rate <= 16000; rate += 8000) {
        uint8_t h[44]; call_wav_header(h, rate, 123456);
        LS_CHECK(!memcmp(h, "RIFF", 4)); LS_CHECK(!memcmp(h + 8, "WAVEfmt ", 8));
        LS_EQ_UINT(le32(h + 4), 123492); LS_EQ_UINT(le32(h + 16), 16);
        LS_EQ_INT(h[20], 1); LS_EQ_INT(h[22], 1);
        LS_EQ_UINT(le32(h + 24), rate); LS_EQ_UINT(le32(h + 28), rate * 2);
        LS_EQ_INT(h[32], 2); LS_EQ_INT(h[34], 16);
        LS_CHECK(!memcmp(h + 36, "data", 4)); LS_EQ_UINT(le32(h + 40), 123456);
    }
}
LS_CASE(retention_keeps_today_and_newest_n_days)
{
    LS_CHECK(!call_day_expired(100, 100, 1));
    LS_CHECK(call_day_expired(99, 100, 1));
    LS_CHECK(!call_day_expired(94, 100, 7));
    LS_CHECK(call_day_expired(93, 100, 7));
    LS_CHECK(!call_day_expired(10, 100, 0));
    LS_CHECK(!call_day_expired(-1, 100, 7));
    LS_CHECK(!call_day_expired(101, 100, 7));
}
