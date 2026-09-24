/* When a frame whose NID passed BCH counts as signal. Pure policy; the real
 * decode loop calls it once per valid frame in app_p25.c. */
#include "ls_test.h"

#include "p25_sync_confirm.h"

LS_CASE(a_lone_frame_on_a_fresh_tune_is_not_signal)
{
    p25_sync_confirm_t c;
    p25_sync_confirm_reset(&c);
    LS_CHECK(!p25_sync_confirm_frame(&c, 0x527, 1000, 1));
    LS_EQ_UINT(c.unconfirmed, 1);
    LS_EQ_UINT(c.unconfirmed_nac, 0x527);
    LS_EQ_UINT(c.confirmed, 0);
}

LS_CASE(back_to_back_frames_with_one_nac_are_signal_from_the_second)
{
    p25_sync_confirm_t c;
    p25_sync_confirm_reset(&c);
    /* HDU, then LDU1 82 ms later, then LDU2 180 ms after that. */
    LS_CHECK(!p25_sync_confirm_frame(&c, 0x527, 1000, 1));
    LS_CHECK(p25_sync_confirm_frame(&c, 0x527, 1082, 1));
    LS_CHECK(p25_sync_confirm_frame(&c, 0x527, 1262, 1));
}

LS_CASE(one_lost_frame_between_two_good_ones_still_pairs)
{
    p25_sync_confirm_t c;
    p25_sync_confirm_reset(&c);
    LS_CHECK(!p25_sync_confirm_frame(&c, 0x527, 1000, 1));
    LS_CHECK(p25_sync_confirm_frame(&c, 0x527, 1360, 1));
}

LS_CASE(a_new_call_on_a_channel_already_heard_counts_at_once)
{
    p25_sync_confirm_t c;
    p25_sync_confirm_reset(&c);
    (void)p25_sync_confirm_frame(&c, 0x527, 1000, 1);
    LS_CHECK(p25_sync_confirm_frame(&c, 0x527, 1180, 1));
    /* Four minutes of silence, then the next call's first frame. */
    LS_CHECK(p25_sync_confirm_frame(&c, 0x527, 241000, 1));
}

LS_CASE(noise_with_a_random_nac_on_a_known_channel_is_not_signal)
{
    p25_sync_confirm_t c;
    p25_sync_confirm_reset(&c);
    (void)p25_sync_confirm_frame(&c, 0x527, 1000, 1);
    LS_CHECK(p25_sync_confirm_frame(&c, 0x527, 1180, 1));
    LS_CHECK(!p25_sync_confirm_frame(&c, 0x9C3, 60000, 1));
    /* ...and it does not displace the channel's NAC. */
    LS_CHECK(p25_sync_confirm_frame(&c, 0x527, 90000, 1));
}

LS_CASE(two_quick_frames_with_different_nacs_do_not_pair)
{
    p25_sync_confirm_t c;
    p25_sync_confirm_reset(&c);
    LS_CHECK(!p25_sync_confirm_frame(&c, 0x111, 1000, 1));
    LS_CHECK(!p25_sync_confirm_frame(&c, 0x222, 1100, 1));
}

LS_CASE(frames_too_far_apart_do_not_pair)
{
    p25_sync_confirm_t c;
    p25_sync_confirm_reset(&c);
    LS_CHECK(!p25_sync_confirm_frame(&c, 0x527, 1000, 1));
    LS_CHECK(!p25_sync_confirm_frame(&c, 0x527, 1000 + P25_SYNC_PAIR_MS + 1, 1));
}

LS_CASE(a_retune_forgets_the_channel_nac)
{
    p25_sync_confirm_t c;
    p25_sync_confirm_reset(&c);
    (void)p25_sync_confirm_frame(&c, 0x527, 1000, 1);
    LS_CHECK(p25_sync_confirm_frame(&c, 0x527, 1180, 1));
    /* Same NAC on another frequency proves nothing about that frequency. */
    LS_CHECK(!p25_sync_confirm_frame(&c, 0x527, 5000, 2));
    LS_CHECK(p25_sync_confirm_frame(&c, 0x527, 5180, 2));
    /* Counters survive the retune. */
    LS_EQ_UINT(c.confirmed, 2);
    LS_EQ_UINT(c.unconfirmed, 2);
}

LS_CASE(a_pair_across_a_retune_does_not_count)
{
    p25_sync_confirm_t c;
    p25_sync_confirm_reset(&c);
    LS_CHECK(!p25_sync_confirm_frame(&c, 0x527, 1000, 1));
    LS_CHECK(!p25_sync_confirm_frame(&c, 0x527, 1100, 2));
}

LS_CASE(the_millisecond_clock_wrapping_does_not_break_a_pair)
{
    p25_sync_confirm_t c;
    p25_sync_confirm_reset(&c);
    LS_CHECK(!p25_sync_confirm_frame(&c, 0x527, 0xFFFFFF00u, 1));
    LS_CHECK(p25_sync_confirm_frame(&c, 0x527, 0x00000040u, 1));
}

/* The rate the hunt produces in noise: about 0.8 raw syncs a second, one in
 * roughly 1,100 passing BCH with a random NAC - a valid noise frame every
 * 20-25 minutes. A year of that on an empty channel, with random gaps of up
 * to 45 minutes. Without the check every one of them was signal. */
LS_CASE(a_year_of_noise_never_becomes_signal)
{
    p25_sync_confirm_t c;
    p25_sync_confirm_reset(&c);
    uint32_t seed = 12345u;
    uint64_t t = 0;
    unsigned frames = 0, signal = 0;
    while (t < 365ull * 24 * 3600 * 1000) {
        seed = seed * 1664525u + 1013904223u;
        t += (seed >> 8) % (45u * 60u * 1000u);
        seed = seed * 1664525u + 1013904223u;
        frames++;
        if (p25_sync_confirm_frame(&c, (uint16_t)((seed >> 12) & 0xFFFu),
                                   (uint32_t)t, 1))
            signal++;
    }
    LS_CHECK(frames > 20000);
    LS_EQ_UINT(signal, 0);
}

/* The same noise on a channel whose NAC is known: only a noise frame that
 * happens to carry that NAC gets through, one in 4,096. */
LS_CASE(noise_on_a_known_channel_passes_only_on_its_own_nac)
{
    p25_sync_confirm_t c;
    p25_sync_confirm_reset(&c);
    (void)p25_sync_confirm_frame(&c, 0x527, 0, 1);
    LS_CHECK(p25_sync_confirm_frame(&c, 0x527, 180, 1));
    uint32_t seed = 777u, t = 1000;
    unsigned other = 0;
    for (int i = 0; i < 20000; ++i) {
        seed = seed * 1664525u + 1013904223u;
        t += 60000u;
        const uint16_t nac = (uint16_t)((seed >> 12) & 0xFFFu);
        if (p25_sync_confirm_frame(&c, nac, t, 1) && nac != 0x527) other++;
    }
    LS_EQ_UINT(other, 0);
}
