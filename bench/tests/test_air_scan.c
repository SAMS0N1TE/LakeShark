/* The channelised airband / CB / weather scanner against a simulated band:
   carriers that come and go, each with the mirror image a dongle's I/Q
   imbalance puts on the far side of the LO, over a noise floor with a
   little texture. The scanner sees only what one 512-bin FFT a block would
   show it at the LO it asked for. */
#include "ls_test.h"
#include "air_scan.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define N     512
#define RATE  256000u
#define BLOCK_US 32000

typedef struct { uint32_t hz; float db; int64_t on_us, off_us; } carrier_t;

static carrier_t s_world[8];
static int s_nworld;
static float s_spec[N];
static ls_rng_t s_rng;

static void world_clear(void) { s_nworld = 0; ls_rng_seed(&s_rng, 42); }
static void world_add(uint32_t hz, float db, int64_t on_us, int64_t off_us)
{
    s_world[s_nworld++] = (carrier_t){ hz, db, on_us, off_us };
}

static void put(int bin, float db)
{
    if (bin < 0 || bin >= N) return;
    if (db > s_spec[bin]) s_spec[bin] = db;
}

static const float *world(uint32_t lo, int64_t now)
{
    for (int i = 0; i < N; ++i)
        s_spec[i] = -80.0f + (float)(ls_rng_u32(&s_rng) % 400) / 100.0f;
    /* The LO's own spike. */
    s_spec[N / 2] = -45.0f;
    for (int c = 0; c < s_nworld; ++c) {
        if (now < s_world[c].on_us || now >= s_world[c].off_us) continue;
        const double off = (double)s_world[c].hz - (double)lo;
        if (fabs(off) > RATE / 2) continue;
        const int b = N / 2 + (int)lround(off * N / RATE);
        const float pk = -80.0f + s_world[c].db;
        put(b, pk); put(b - 1, pk - 8); put(b + 1, pk - 8);
        put(N - b, pk - 35.0f);                   /* the image */
    }
    return s_spec;
}

static air_scan_t s_air;
static uint32_t s_lo;
static int64_t s_now;

static air_action_t s_last;
static void apply(air_action_t a) { if (a.tune_hz) s_lo = a.tune_hz; s_last = a; }

static void run(int64_t until_us)
{
    while (s_now < until_us) {
        apply(air_scan_block(&s_air, world(s_lo, s_now), N, RATE, s_now));
        s_now += BLOCK_US;
    }
}

static void begin(int preset)
{
    memset(&s_air, 0, sizeof(s_air));
    s_now = 1000000;
    apply(air_scan_start(&s_air, preset, s_now));
}

/* Runs until the scanner is listening (or `limit` passes); true if it is. */
static bool run_until_listen(int64_t limit_us)
{
    const int64_t end = s_now + limit_us;
    while (s_now < end) {
        apply(air_scan_block(&s_air, world(s_lo, s_now), N, RATE, s_now));
        s_now += BLOCK_US;
        if (s_last.listen && s_air.phase == AIR_PH_LISTEN) return true;
    }
    return false;
}

LS_CASE(air_channel_plans_match_the_bands)
{
    const air_preset_t *a = air_preset(AIR_PRESET_AIR25);
    LS_EQ_INT(air_channel_count(a), 760);
    LS_EQ_UINT(air_channel_hz(a, 0), 118000000);
    LS_EQ_UINT(air_channel_hz(a, 759), 136975000);
    const air_preset_t *e = air_preset(AIR_PRESET_AIR833);
    LS_EQ_INT(air_channel_count(e), 2280);
    LS_EQ_UINT(air_channel_hz(e, 420), 121500000);
    LS_EQ_UINT(air_channel_hz(e, 1), 118008333);
    LS_EQ_UINT(air_channel_hz(e, 2), 118016667);
    LS_EQ_INT(air_channel_find(e, 118016600), 2);
    const air_preset_t *cb = air_preset(AIR_PRESET_CB_AM);
    LS_EQ_UINT(air_channel_hz(cb, 22), 27255000);        /* CB 23 */
    LS_EQ_UINT(air_channel_hz(cb, 23), 27235000);        /* CB 24 */
    LS_EQ_INT(air_channel_number(cb, 39), 40);
    LS_EQ_INT(air_preset(AIR_PRESET_CB_USB)->demod, AIR_DEMOD_USB);
    const air_preset_t *wx = air_preset(AIR_PRESET_WX);
    LS_EQ_UINT(air_channel_hz(wx, 0), 162550000);        /* WX1 */
    LS_EQ_INT(air_channel_find(wx, 162475000), 2);       /* WX3 */
}

LS_CASE(air_windows_cover_every_channel_once_clear_of_the_lo)
{
    for (int p = AIR_PRESET_AIR25; p < AIR_PRESET_COUNT; ++p) {
        begin(p);
        int seen = 0;
        for (int w = 0; w < s_air.nwin; ++w) {
            for (int k = s_air.win_first[w]; k < s_air.win_first[w] + s_air.win_n[w]; ++k) {
                const int ch = s_air.p->list ? s_air.order[k] : k;
                const int64_t off = (int64_t)air_channel_hz(s_air.p, ch) - s_air.win_hz[w];
                LS_CHECK_MSG(llabs(off) <= 96000 && llabs(off) >= 3000,
                             "preset %d window %d channel %d off %lld", p, w, ch, (long long)off);
                seen++;
            }
        }
        LS_EQ_INT(seen, air_channel_count(s_air.p));
    }
    begin(AIR_PRESET_AIR25);
    ls_note("airband 25k: %d windows a sweep", s_air.nwin);
    LS_CHECK(s_air.nwin <= 120);
    begin(AIR_PRESET_WX);
    LS_EQ_INT(s_air.nwin, 1);                            /* all seven at once */
    begin(AIR_PRESET_CB_AM);
    LS_CHECK(s_air.nwin <= 3);
}

LS_CASE(air_scan_finds_a_tower_listens_then_resumes_after_the_hang)
{
    world_clear();
    world_add(124350000, 30.0f, 0, 6000000);
    begin(AIR_PRESET_AIR25);
    LS_CHECK(run_until_listen(12000000));
    LS_EQ_UINT(s_last.listen_hz, 124350000);
    LS_EQ_UINT(s_lo, 124350000 + AIR_LISTEN_OFS);
    run(s_now + 4 * BLOCK_US);
    LS_CHECK(s_last.audio);
    LS_EQ_INT(s_air.nhits, 1);
    LS_EQ_UINT(s_air.hits[0].hz, 124350000);
    /* The carrier drops at 6 s; after the hang the scan moves on. */
    run(6000000 + 2500000 + 300000);
    LS_CHECK(s_air.phase != AIR_PH_LISTEN);
    LS_CHECK(s_air.hits[0].talk_ms > 1000);
}

LS_CASE(air_guard_is_sampled_between_hops_while_scanning)
{
    world_clear();
    world_add(121500000, 25.0f, 0, 60000000);
    begin(AIR_PRESET_AIR25);
    /* Start far from 121.5: the guard has to be found by its own samples,
       well before the sweep would get there. */
    s_air.win = s_air.nwin / 2;
    LS_CHECK(run_until_listen(2000000));
    LS_EQ_UINT(s_last.listen_hz, 121500000);
    LS_CHECK(s_air.hits[0].guard);
}

LS_CASE(air_guard_interrupts_a_long_conversation)
{
    world_clear();
    world_add(127000000, 30.0f, 0, 60000000);             /* never stops */
    world_add(121500000, 28.0f, 5000000, 60000000);       /* mayday at 5 s */
    begin(AIR_PRESET_AIR25);
    LS_CHECK(run_until_listen(12000000));
    LS_EQ_UINT(s_last.listen_hz, 127000000);
    run(s_now + 1000000);
    LS_EQ_UINT(s_air.lo_hz, 127000000 + AIR_LISTEN_OFS);
    run(5000000 + 3000000 + 500000);
    LS_EQ_UINT(s_last.listen_hz, 121500000);
}

LS_CASE(air_lockout_keeps_a_busy_channel_from_stopping_the_scan)
{
    world_clear();
    world_add(125000000, 30.0f, 0, 600000000);
    world_add(133000000, 22.0f, 0, 600000000);
    begin(AIR_PRESET_AIR25);
    s_air.guard_on = false;
    LS_CHECK(run_until_listen(12000000));
    LS_EQ_UINT(s_last.listen_hz, 125000000);
    apply(air_scan_lockout(&s_air, s_now));
    LS_CHECK(air_scan_locked(&s_air, air_channel_find(s_air.p, 125000000)));
    LS_CHECK(run_until_listen(12000000));
    LS_EQ_UINT(s_last.listen_hz, 133000000);
    apply(air_scan_skip(&s_air, s_now));
    /* A whole sweep later it is 133.000 again, never 125.000. */
    LS_CHECK(run_until_listen(12000000));
    LS_EQ_UINT(s_last.listen_hz, 133000000);
    LS_EQ_INT(air_scan_lockout_count(&s_air), 1);
}

LS_CASE(air_mirror_image_of_a_strong_carrier_is_not_a_hit)
{
    world_clear();
    begin(AIR_PRESET_AIR25);
    s_air.guard_on = false;
    /* A 70 dB carrier 62.5 kHz above the first window's LO (118.0875)
       puts its image 35 dB down on 118.025: 35 dB over the floor, which
       would pass the threshold if it were taken for a carrier. */
    const uint32_t lo = s_air.win_hz[0];
    LS_EQ_UINT(lo, 118087500);
    world_add(lo + 62500, 70.0f, 0, 600000000);
    const uint32_t image = lo - 62500;
    run(s_now + 3 * BLOCK_US);
    LS_CHECK(s_air.phase == AIR_PH_LISTEN);
    LS_EQ_UINT(s_last.listen_hz, lo + 62500);
    LS_CHECK(s_air.images_rejected > 0);
    LS_EQ_INT(s_air.level[air_channel_find(s_air.p, image)], 0);
}

LS_CASE(air_833_plan_lands_on_the_right_channel)
{
    world_clear();
    world_add(132008333, 24.0f, 0, 600000000);
    begin(AIR_PRESET_AIR833);
    s_air.guard_on = false;
    LS_CHECK(run_until_listen(15000000));
    LS_EQ_UINT(s_last.listen_hz, 132008333);
}

LS_CASE(cb_scan_stops_on_channel_19)
{
    world_clear();
    world_add(27185000, 26.0f, 0, 600000000);
    begin(AIR_PRESET_CB_AM);
    LS_CHECK(run_until_listen(2000000));
    LS_EQ_UINT(s_last.listen_hz, 27185000);
    LS_EQ_INT(air_channel_number(s_air.p, s_air.listen_ch), 19);
}

LS_CASE(weather_picks_the_strongest_of_the_seven)
{
    world_clear();
    world_add(162400000, 14.0f, 0, 600000000);
    world_add(162475000, 31.0f, 0, 600000000);
    begin(AIR_PRESET_WX);
    LS_CHECK(run_until_listen(500000));
    LS_EQ_UINT(s_last.listen_hz, 162475000);
    LS_EQ_INT(air_channel_number(s_air.p, s_air.listen_ch), 3);
}
