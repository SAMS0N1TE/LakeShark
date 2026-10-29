/* TIME MACHINE ring: the stage a capture path copies into, the decimating
   ring behind it, wrap, freeze, segments and the tape. */
#include "ls_test.h"
#include "ls_tmach_ring.h"

#include <stdlib.h>
#include <string.h>

static uint8_t *stage_mem(void) { return calloc(LS_TM_SLOTS, LS_TM_SLOT_BYTES); }

static ls_tm_src_t rtl(uint64_t hz, uint32_t rate)
{
    return (ls_tm_src_t){ .center_hz = hz, .rate_hz = rate, .fmt = LS_TM_FMT_CU8, .radio = LS_TM_RADIO_RTL };
}

LS_CASE(stage_publishes_full_slots_and_flushes_part_slots_on_pair_boundaries)
{
    ls_tm_stage_t st;
    uint8_t *mem = stage_mem();
    ls_tm_stage_init(&st, mem);
    ls_tm_src_t s = rtl(152600000, 256000);
    uint8_t buf[5000];
    for (unsigned i = 0; i < sizeof(buf); i++) buf[i] = (uint8_t)i;
    /* 4 x 5000 = 20000 bytes: one full slot and 3616 open. */
    for (int k = 0; k < 4; k++) LS_EQ_UINT(ls_tm_stage_put(&st, &s, buf, sizeof(buf), 1000 + k), sizeof(buf));
    const uint8_t *d;
    const ls_tm_slot_meta_t *m = ls_tm_stage_peek(&st, &d);
    LS_CHECK(m != NULL);
    LS_EQ_UINT(m->bytes, LS_TM_SLOT_BYTES);
    LS_CHECK(!m->gap_before);
    LS_EQ_UINT(d[5000], 0);              /* the second call's first byte */
    ls_tm_stage_release(&st);
    LS_CHECK(ls_tm_stage_peek(&st, NULL) == NULL);
    /* An odd byte count: the flushed slot keeps whole pairs only. */
    ls_tm_stage_put(&st, &s, buf, 3, 2000);
    ls_tm_stage_flush(&st, false, 2001);
    m = ls_tm_stage_peek(&st, &d);
    LS_EQ_UINT(m->bytes, 3616 + 2);
    ls_tm_stage_release(&st);
    /* The next byte is the dangling pair's second half and is skipped. */
    uint8_t two[3] = { 0xAA, 0x11, 0x22 };
    ls_tm_stage_put(&st, &s, two, 3, 3000);
    ls_tm_stage_flush(&st, true, 3001);
    m = ls_tm_stage_peek(&st, &d);
    LS_EQ_UINT(m->bytes, 2);
    LS_EQ_UINT(d[0], 0x11);
    ls_tm_stage_release(&st);
    free(mem);
}

LS_CASE(stage_full_queue_drops_and_marks_the_next_slot_as_a_gap)
{
    ls_tm_stage_t st;
    uint8_t *mem = stage_mem();
    ls_tm_stage_init(&st, mem);
    ls_tm_src_t s = rtl(100000000, 256000);
    uint8_t *big = calloc(1, LS_TM_SLOT_BYTES);
    for (unsigned k = 0; k < LS_TM_SLOTS; k++) ls_tm_stage_put(&st, &s, big, LS_TM_SLOT_BYTES, k);
    LS_EQ_UINT(ls_tm_stage_put(&st, &s, big, 1001, 99), 0);  /* full: dropped, odd length */
    LS_EQ_UINT(atomic_load(&st.drops), 1);
    LS_EQ_UINT(ls_tm_stage_discard(&st), LS_TM_SLOTS);
    big[1] = 7;
    ls_tm_stage_put(&st, &s, big, 64, 100);
    ls_tm_stage_flush(&st, false, 100);
    const uint8_t *d;
    const ls_tm_slot_meta_t *m = ls_tm_stage_peek(&st, &d);
    LS_CHECK(m && m->gap_before);
    LS_EQ_UINT(m->bytes, 62);             /* one byte skipped to realign, then whole pairs */
    LS_EQ_UINT(d[0], 7);
    /* A different source closes the open slot first. */
    ls_tm_stage_release(&st);
    ls_tm_src_t t = rtl(100025000, 256000);
    ls_tm_stage_put(&st, &s, big, 8, 200);
    ls_tm_stage_put(&st, &t, big, 8, 201);
    ls_tm_stage_flush(&st, false, 202);
    m = ls_tm_stage_peek(&st, NULL);
    LS_EQ_UINT(m->src.center_hz, 100000000);
    ls_tm_stage_release(&st);
    m = ls_tm_stage_peek(&st, NULL);
    LS_EQ_UINT(m->src.center_hz, 100025000);
    free(big); free(mem);
}

static void feed_bytes(ls_tm_ring_t *r, const ls_tm_src_t *s, const uint8_t *data, size_t bytes, int64_t end_us, bool gap)
{
    ls_tm_slot_meta_t m = { .src = *s, .end_us = end_us, .bytes = (uint32_t)bytes, .gap_before = gap };
    ls_tm_ring_feed(r, &m, data);
}

LS_CASE(decimation_factor_keeps_at_least_the_narrow_rate)
{
    LS_EQ_UINT(ls_tm_decim_for(256000, 60000), 4);
    LS_EQ_UINT(ls_tm_decim_for(125000, 60000), 2);
    LS_EQ_UINT(ls_tm_decim_for(2400000, 60000), 40);
    LS_EQ_UINT(ls_tm_decim_for(50000, 60000), 1);
    LS_EQ_UINT(ls_tm_decim_for(20000000, 60000), LS_TM_DECIM_MAX);
    LS_EQ_UINT(ls_tm_decim_for(256000, 0), 1);
}

LS_CASE(cic_keeps_dc_and_slow_tones_at_unity_gain)
{
    static uint8_t buf[1 << 16];
    ls_tm_ring_t r;
    ls_tm_ring_init(&r, buf, sizeof(buf), 60000);
    ls_tm_src_t s = rtl(1, 256000);
    uint8_t in[4096];
    for (unsigned k = 0; k < sizeof(in) / 2; k++) { in[2 * k] = 177; in[2 * k + 1] = 77; } /* +49, -51 */
    feed_bytes(&r, &s, in, sizeof(in), 1000000, false);
    const ls_tm_seg_t *seg = ls_tm_ring_seg_newest(&r);
    LS_EQ_UINT(seg->decim, 4);
    LS_EQ_UINT(seg->rate_hz, 64000);
    LS_EQ_UINT(seg->fmt, LS_TM_FMT_CS8);
    LS_EQ_UINT(seg->pairs, 2048 / 4);
    int8_t last[2];
    LS_CHECK(ls_tm_ring_read(&r, r.head - 2, last, 2));
    LS_EQ_INT(last[0], 49);           /* CU8 less 128 */
    LS_EQ_INT(last[1], -51);
    /* CS16 at 125k halves to 62.5k, as is. */
    ls_tm_ring_reset(&r);
    ls_tm_src_t lr = { .center_hz = 2, .rate_hz = 125000, .fmt = LS_TM_FMT_CS16, .radio = LS_TM_RADIO_LR2021 };
    uint8_t in16[4096];
    for (unsigned k = 0; k < sizeof(in16) / 4; k++) {
        int16_t i = (int16_t)(8000 * cos(6.283185307 * 1000.0 * k / 125000)), q = (int16_t)(8000 * sin(6.283185307 * 1000.0 * k / 125000));
        in16[4 * k] = (uint8_t)i; in16[4 * k + 1] = (uint8_t)((uint16_t)i >> 8);
        in16[4 * k + 2] = (uint8_t)q; in16[4 * k + 3] = (uint8_t)((uint16_t)q >> 8);
    }
    feed_bytes(&r, &lr, in16, sizeof(in16), 2000000, false);
    seg = ls_tm_ring_seg_newest(&r);
    LS_EQ_UINT(seg->decim, 2);
    LS_EQ_UINT(seg->fmt, LS_TM_FMT_CS16);
    int16_t p[2];
    LS_CHECK(ls_tm_ring_read(&r, r.head - 4, p, 4));
    LS_NEAR(sqrt((double)p[0] * p[0] + (double)p[1] * p[1]), 8000, 80);   /* a 1 kHz tone at 62.5k */
}

LS_CASE(ring_wraps_keeping_the_newest_bytes_exactly)
{
    uint8_t buf[1000];
    ls_tm_ring_t r;
    ls_tm_ring_init(&r, buf, sizeof(buf), 0);       /* keep everything: no decimation */
    LS_EQ_UINT(r.cap, 1000);
    ls_tm_src_t s = rtl(5, 48000);
    uint8_t in[600];
    uint32_t sent = 0;
    for (int b = 0; b < 7; b++) {
        for (unsigned k = 0; k < sizeof(in); k++) in[k] = (uint8_t)(128 + (int8_t)((sent + k) * 7));
        feed_bytes(&r, &s, in, sizeof(in), 1000 * (b + 1), false);
        sent += sizeof(in);
    }
    LS_EQ_UINT(r.head, 4200);
    LS_EQ_UINT(ls_tm_ring_floor(&r), 3200);
    LS_EQ_UINT(ls_tm_ring_seg_newest(&r)->pairs, 2100);
    LS_EQ_UINT(r.seg_count, 1);
    int8_t out[1000];
    LS_CHECK(ls_tm_ring_read(&r, 3200, out, 1000));
    for (unsigned k = 0; k < 1000; k++) LS_EQ_INT(out[k], (int8_t)((3200 + k) * 7));
    LS_CHECK(!ls_tm_ring_read(&r, 3199, out, 2));      /* overwritten */
    LS_CHECK(!ls_tm_ring_read(&r, 4199, out, 2));      /* not written */
    LS_NEAR(ls_tm_ring_held_s(&r), 500.0 / 48000, 1e-9);
}

LS_CASE(freeze_holds_the_window_until_it_is_copied_out)
{
    uint8_t buf[2000];
    ls_tm_ring_t r;
    ls_tm_ring_init(&r, buf, sizeof(buf), 0);
    ls_tm_src_t s = rtl(7, 10000);                     /* 10k pairs/s: 100 us a pair */
    uint8_t in[400];
    int64_t t = 0;
    uint32_t sent = 0;
    for (int b = 0; b < 5; b++) {                      /* 1000 pairs = 2000 bytes: exactly full */
        for (unsigned k = 0; k < sizeof(in); k++) in[k] = (uint8_t)(128 + (int8_t)(sent + k));
        sent += sizeof(in);
        t += 20000;
        feed_bytes(&r, &s, in, sizeof(in), t, false);
    }
    const ls_tm_seg_t *seg = ls_tm_ring_seg_newest(&r);
    LS_EQ_INT(seg->start_us, 0);
    LS_EQ_INT(seg->end_us, 100000);
    ls_tm_window_t w;
    LS_CHECK(ls_tm_window_begin(&r, &w, 20000, 0));    /* the last 80 ms */
    LS_EQ_UINT(w.from, 400);
    LS_EQ_UINT(w.to, 2000);
    LS_EQ_INT(w.start_us, 20000);
    LS_EQ_INT(w.end_us, 100000);
    /* The writer may not run over the window's first bytes. */
    feed_bytes(&r, &s, in, sizeof(in), t + 20000, false);   /* 2400 > 400 + 2000: refused */
    LS_EQ_UINT(r.held_drops, 1);
    LS_EQ_UINT(r.head, 2000);
    int8_t got[2000];
    size_t n = ls_tm_window_step(&r, &w, got, 600);
    LS_EQ_UINT(n, 600);
    for (unsigned k = 0; k < n; k++) LS_EQ_INT(got[k], (int8_t)(400 + k));
    /* Now 400 more bytes fit, and land in a new segment (a gap). */
    feed_bytes(&r, &s, in, sizeof(in), t + 40000, false);
    LS_EQ_UINT(r.head, 2400);
    LS_EQ_UINT(r.seg_count, 2);
    LS_EQ_UINT(w.seg.id, 1);
    n = 0;
    size_t k;
    while ((k = ls_tm_window_step(&r, &w, got + n, 500)) > 0) n += k;
    LS_EQ_UINT(n, 1000);
    for (unsigned j = 0; j < n; j++) LS_EQ_INT(got[j], (int8_t)(1000 + j));
    ls_tm_window_end(&r, &w);
    LS_CHECK(!r.held);
}

LS_CASE(segments_follow_retunes_and_answer_what_was_tuned_when)
{
    static uint8_t buf[1 << 15];
    ls_tm_ring_t r;
    ls_tm_ring_init(&r, buf, sizeof(buf), 60000);
    uint8_t in[2560];
    memset(in, 128, sizeof(in));
    ls_tm_src_t a = rtl(152600000, 256000), b = rtl(152840000, 256000);
    int64_t t = 1000000;
    for (int k = 0; k < 100; k++) { t += 5000; feed_bytes(&r, &a, in, sizeof(in), t, false); }   /* 0.5 s */
    for (int k = 0; k < 100; k++) { t += 5000; feed_bytes(&r, &b, in, sizeof(in), t, false); }
    LS_EQ_UINT(r.seg_count, 2);
    const ls_tm_seg_t *sa = ls_tm_ring_seg_at(&r, 1200000), *sb = ls_tm_ring_seg_at(&r, 1900000);
    LS_CHECK(sa && sb);
    LS_EQ_UINT(sa->src.center_hz, 152600000);
    LS_EQ_UINT(sb->src.center_hz, 152840000);
    /* Tape: 10 ticks a second of stored samples, the retune flagged. */
    LS_EQ_UINT(r.tick_count, 10);
    unsigned seg_flags = 0;
    for (unsigned k = 0; k < r.tick_count; k++) seg_flags += !!(r.tick[k].flags & LS_TM_TICK_SEG);
    LS_EQ_UINT(seg_flags, 2);
    /* A break starts a segment even with the same tuning. */
    ls_tm_ring_break(&r);
    feed_bytes(&r, &b, in, sizeof(in), t + 5000, false);
    LS_EQ_UINT(r.seg_count, 3);
    LS_EQ_UINT(r.gaps, 1);
}

LS_CASE(tape_level_tracks_signal_power)
{
    static uint8_t buf[1 << 16];
    ls_tm_ring_t r;
    ls_tm_ring_init(&r, buf, sizeof(buf), 60000);
    ls_tm_src_t s = rtl(1, 256000);
    static uint8_t quiet[51200], loud[51200];
    for (unsigned k = 0; k < sizeof(quiet) / 2; k++) {
        quiet[2 * k] = (uint8_t)(128 + (k & 1)); quiet[2 * k + 1] = 128;
        double ph = 6.283185307 * 3000.0 * k / 256000;
        loud[2 * k] = (uint8_t)lround(127.5 + 100 * cos(ph));
        loud[2 * k + 1] = (uint8_t)lround(127.5 + 100 * sin(ph));
    }
    feed_bytes(&r, &s, quiet, sizeof(quiet), 100000, false);   /* 0.1 s */
    feed_bytes(&r, &s, loud, sizeof(loud), 200000, false);
    LS_EQ_UINT(r.tick_count, 2);
    LS_CHECK(r.tick[1].level > r.tick[0].level + 40);      /* > 20 dB louder */
    LS_NEAR((r.tick[1].level / 2.0) - 100.0, 20 * log10(100.0 / 128.0), 1.0);
    LS_CHECK(r.tick[0].flags & LS_TM_TICK_DATA);
}

/* ------------------------------------------------ two writers, one stage -- */

#include <pthread.h>
#include <time.h>

LS_CASE(stage_publishes_an_overfull_slot_instead_of_wrapping_the_room_left)
{
    ls_tm_stage_t st;
    uint8_t *mem = stage_mem();
    ls_tm_stage_init(&st, mem);
    ls_tm_src_t s = rtl(152600000, 256000);
    uint8_t buf[64] = {0};
    ls_tm_stage_put(&st, &s, buf, 8, 1);
    /* What a second writer racing the first could leave behind. */
    st.open_bytes = LS_TM_SLOT_BYTES + 6;
    LS_EQ_UINT(ls_tm_stage_put(&st, &s, buf, sizeof(buf), 2), sizeof(buf));
    const ls_tm_slot_meta_t *m = ls_tm_stage_peek(&st, NULL);
    LS_CHECK(m != NULL);
    LS_EQ_UINT(m->bytes, LS_TM_SLOT_BYTES);
    ls_tm_stage_release(&st);
    LS_EQ_UINT(st.open_bytes, sizeof(buf));     /* the new bytes went to the next slot */
    free(mem);
}

static int64_t mono_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static struct {
    ls_tm_stage_t st;
    ls_tm_claim_t claim;
    atomic_int inside, worst_inside;
    atomic_int done;
    atomic_uint written[3];
} two;

static void *two_writer(void *arg)
{
    const unsigned radio = (unsigned)(uintptr_t)arg;
    const uint8_t fill = radio == LS_TM_RADIO_RTL ? 0xA5 : 0x5A;
    uint8_t buf[5000];
    memset(buf, fill, sizeof(buf));
    const ls_tm_src_t s = { .center_hz = 100000000u + radio, .rate_hz = 256000,
                            .fmt = LS_TM_FMT_CU8, .radio = (uint8_t)radio };
    for (int k = 0; k < 4000; k++) {
        /* A short stale time so ownership changes hands many times. */
        if (ls_tm_claim_enter(&two.claim, radio, mono_us(), 50)) {
            const int in = atomic_fetch_add(&two.inside, 1) + 1;
            if (in > atomic_load(&two.worst_inside)) atomic_store(&two.worst_inside, in);
            ls_tm_stage_put(&two.st, &s, buf, sizeof(buf), mono_us());
            atomic_fetch_add(&two.written[radio], 1);
            atomic_fetch_sub(&two.inside, 1);
            ls_tm_claim_leave(&two.claim);
        }
        if (k % 64 == 0) { struct timespec t = {0, 100000}; nanosleep(&t, NULL); }
    }
    atomic_fetch_add(&two.done, 1);
    return NULL;
}

LS_CASE(two_writers_through_the_claim_never_mix_or_overrun_a_slot)
{
    uint8_t *mem = stage_mem();
    memset(&two, 0, sizeof(two));
    ls_tm_stage_init(&two.st, mem);
    pthread_t a, b;
    pthread_create(&a, NULL, two_writer, (void *)(uintptr_t)LS_TM_RADIO_RTL);
    pthread_create(&b, NULL, two_writer, (void *)(uintptr_t)LS_TM_RADIO_HACKRF);
    unsigned slots = 0, bad = 0;

    for (;;) {
        const bool finished = atomic_load(&two.done) == 2;   /* read before the peek */
        const uint8_t *d;
        const ls_tm_slot_meta_t *m = ls_tm_stage_peek(&two.st, &d);
        if (!m) {
            if (finished) break;
            struct timespec t = {0, 50000}; nanosleep(&t, NULL); continue;
        }
        const uint8_t fill = m->src.radio == LS_TM_RADIO_RTL ? 0xA5 : 0x5A;
        if (m->bytes > LS_TM_SLOT_BYTES || m->src.center_hz != 100000000u + m->src.radio) bad++;
        else for (uint32_t i = 0; i < m->bytes; i++) if (d[i] != fill) { bad++; break; }
        slots++;
        ls_tm_stage_release(&two.st);
    }
    pthread_join(a, NULL); pthread_join(b, NULL);
    LS_EQ_INT(atomic_load(&two.worst_inside), 1);
    LS_EQ_UINT(bad, 0);
    LS_CHECK(slots > 0);
    LS_CHECK(atomic_load(&two.written[LS_TM_RADIO_RTL]) + atomic_load(&two.written[LS_TM_RADIO_HACKRF]) > 0);
    free(mem);
}

LS_CASE(claim_keeps_the_stage_for_the_dongle_it_has_until_it_goes_quiet)
{
    ls_tm_claim_t c;
    memset(&c, 0, sizeof(c));
    LS_CHECK(ls_tm_claim_enter(&c, LS_TM_RADIO_RTL, 1000, 1500000));
    LS_CHECK(!ls_tm_claim_enter(&c, LS_TM_RADIO_RTL, 1001, 1500000));      /* busy: refused */
    ls_tm_claim_leave(&c);
    LS_CHECK(!ls_tm_claim_enter(&c, LS_TM_RADIO_HACKRF, 500000, 1500000)); /* the RTL is fresh */
    LS_CHECK(ls_tm_claim_enter(&c, LS_TM_RADIO_RTL, 600000, 1500000));
    ls_tm_claim_leave(&c);
    LS_CHECK(ls_tm_claim_enter(&c, LS_TM_RADIO_HACKRF, 2200000, 1500000)); /* quiet 1.6 s: taken */
    ls_tm_claim_leave(&c);
    LS_CHECK(!ls_tm_claim_enter(&c, LS_TM_RADIO_RTL, 2300000, 1500000));
    LS_EQ_UINT(atomic_load(&c.collisions), 1);
}
