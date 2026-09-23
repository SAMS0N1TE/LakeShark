/* Voice decoded before the call proves clear: held, then released or
 * discarded. Pure policy; the real decode path is p25_voice_replay. */
#include "ls_test.h"
#include "p25_voice_hold.h"
#include <string.h>

#define CLEAR 0x80
#define AES   0x84
#define LDU   (9 * 160)

static p25_voice_hold_t h;
static int16_t out[P25_HOLD_MAX_SAMPLES + 2000];

/* An LDU's worth of voice whose samples say which LDU they came from. */
static const int16_t *ldu(int16_t tag)
{
    static int16_t pcm[LDU];
    for (int i = 0; i < LDU; i++) pcm[i] = tag;
    return pcm;
}

static int frame(int16_t tag, int unproven, int ess_valid, int algid, int tg, int now)
{
    return p25_voice_hold_frame(&h, ldu(tag), LDU, unproven, ess_valid,
                                (uint8_t)algid, (uint32_t)tg, (uint32_t)now,
                                out, (int)(sizeof(out) / sizeof(out[0])));
}

LS_CASE(a_call_joined_without_its_hdu_plays_its_first_ldu1_once_ldu2_proves_it_clear)
{
    p25_voice_hold_reset(&h);
    /* LDU1: ESS unknown, decoded and held. */
    LS_EQ_INT(frame(1, 1, 0, 0, 42, 0), 0);
    LS_EQ_INT(h.n, LDU);
    /* LDU2: its own frames decoded unknown, its ESS proves clear at the end. */
    LS_EQ_INT(frame(2, 1, 1, CLEAR, 42, 180), 2 * LDU);
    LS_EQ_INT(out[0], 1);
    LS_EQ_INT(out[LDU - 1], 1);
    LS_EQ_INT(out[LDU], 2);
    LS_EQ_INT(out[2 * LDU - 1], 2);
    LS_EQ_INT(h.n, 0);
    LS_EQ_UINT(h.held_frames, 9);
    LS_EQ_UINT(h.released_frames, 9);
    /* Proven: the next LDU1 is decoded clear and plays straight through. */
    LS_EQ_INT(frame(3, 0, 1, CLEAR, 42, 360), LDU);
    LS_EQ_INT(out[0], 3);
}

LS_CASE(an_encrypted_call_never_reaches_the_speaker)
{
    p25_voice_hold_reset(&h);
    LS_EQ_INT(frame(1, 1, 0, 0, 7, 0), 0);
    /* LDU2 proves it encrypted: the held LDU1 and LDU2's own frames go. */
    LS_EQ_INT(frame(2, 1, 1, AES, 7, 180), 0);
    LS_EQ_INT(h.n, 0);
    LS_EQ_UINT(h.released_frames, 0);
    LS_EQ_UINT(h.discarded_frames, 18);
}

LS_CASE(an_ldu2_whose_ess_fails_keeps_waiting_for_the_next_one)
{
    p25_voice_hold_reset(&h);
    LS_EQ_INT(frame(1, 1, 0, 0, 9, 0), 0);    /* LDU1 unknown */
    LS_EQ_INT(frame(2, 1, 0, 0, 9, 180), 0);  /* LDU2, ESS failed RS: still unknown */
    LS_EQ_INT(h.n, 2 * LDU);
    LS_EQ_INT(frame(3, 1, 0, 0, 9, 360), 0);  /* LDU1: the oldest LDU is dropped */
    LS_EQ_INT(h.n, P25_HOLD_MAX_SAMPLES);
    LS_EQ_UINT(h.discarded_frames, 9);
    LS_EQ_INT(frame(4, 1, 1, CLEAR, 9, 540), P25_HOLD_MAX_SAMPLES + LDU);
    /* The newest 360 ms held, then the proving frame, in order. */
    LS_EQ_INT(out[0], 2);
    LS_EQ_INT(out[LDU], 3);
    LS_EQ_INT(out[2 * LDU], 4);
}

LS_CASE(held_voice_is_not_released_into_a_different_talkgroup)
{
    p25_voice_hold_reset(&h);
    LS_EQ_INT(frame(1, 1, 0, 0, 100, 0), 0);
    LS_EQ_INT(frame(2, 1, 1, CLEAR, 200, 180), LDU);
    LS_EQ_INT(out[0], 2);
    LS_EQ_UINT(h.discarded_frames, 9);
}

LS_CASE(held_voice_expires_when_the_call_goes_quiet)
{
    p25_voice_hold_reset(&h);
    LS_EQ_INT(frame(1, 1, 0, 0, 5, 1000), 0);
    p25_voice_hold_tick(&h, 1000 + P25_HOLD_MAX_AGE_MS);
    LS_EQ_INT(h.n, LDU);
    p25_voice_hold_tick(&h, 1001 + P25_HOLD_MAX_AGE_MS);
    LS_EQ_INT(h.n, 0);
    /* A later call's proof releases only its own voice. */
    LS_EQ_INT(frame(2, 1, 1, CLEAR, 5, 5000), LDU);
    LS_EQ_INT(out[0], 2);
}

LS_CASE(the_end_of_a_call_discards_what_it_never_proved)
{
    p25_voice_hold_reset(&h);
    LS_EQ_INT(frame(1, 1, 0, 0, 5, 0), 0);
    p25_voice_hold_end_call(&h);
    LS_EQ_INT(h.n, 0);
    LS_EQ_UINT(h.discarded_frames, 9);
}

LS_CASE(voice_the_operator_unmuted_is_never_held)
{
    /* unmute_encrypted: frames are not flagged unproven and play at once,
       known-encrypted included. */
    p25_voice_hold_reset(&h);
    LS_EQ_INT(frame(1, 0, 0, 0, 5, 0), LDU);
    LS_EQ_INT(frame(2, 0, 1, AES, 5, 180), LDU);
    LS_EQ_UINT(h.held_frames, 0);
}

LS_CASE(a_proven_clear_call_with_nothing_held_plays_through)
{
    p25_voice_hold_reset(&h);
    LS_EQ_INT(frame(1, 0, 1, CLEAR, 5, 0), LDU);
    LS_EQ_INT(p25_voice_hold_frame(&h, NULL, 0, 0, 1, CLEAR, 5, 1, out, 10), 0);
}
