/* LS_TEST_SOURCES: ${APP}/dmr/dmr_sync.c ${APP}/dmr/dmr_slot.c
 *                  ${APP}/dmr/dmr_bptc.c ${APP}/dmr/dmr_lc.c
 *                  ${APP}/dmr/dmr_burst.c ${APP}/dmr/dmr_framer.c
 *                  ${APP}/dmr/dmr_watch.c
 *                  fixtures/dmr_gen.c
 *
 * The whole DMR path from the demodulator's side: 4FSK symbols in, a
 * talkgroup and a radio ID out. Everything below this was already tested in
 * pieces; nothing fed those pieces from a symbol stream. */

#include "ls_test.h"

#include "dmr.h"
#include "dmr_watch.h"
#include "dmr_gen.h"

#include <string.h>

static const uint8_t BS_VOICE[6] = { 0x75, 0x5F, 0xD7, 0xDF, 0x75, 0xF7 };
static const uint8_t BS_DATA [6] = { 0xDF, 0xF5, 0x7D, 0x75, 0xDF, 0x5D };

/* Thresholds the P25 slicer would have measured, and four symbol levels
   sitting cleanly inside the regions they select. */
#define CENTER 0
#define UMID   2400
#define LMID   (-2400)

static const int SYM_P3 =  3600;
static const int SYM_P1 =  1200;
static const int SYM_M1 = -1200;
static const int SYM_M3 = -3600;

static int rd_bit(const uint8_t *buf, unsigned idx)
{
    return (buf[idx >> 3] >> (7u - (idx & 7u))) & 1u;
}

/* The inverse of the watcher's own mapping: 01 is +3, 00 is +1, 10 is -1,
   11 is -3. A test that reused the watcher's table would agree with itself
   no matter which way round it had them. */
static int symbol_for(int hi, int lo)
{
    if (!hi && lo) return SYM_P3;
    if (!hi)       return SYM_P1;
    if (!lo)       return SYM_M1;
    return SYM_M3;
}

static void feed_bits(const uint8_t *bits, unsigned count, bool invert)
{
    for (unsigned i = 0; i + 1 < count; i += 2) {
        int hi = rd_bit(bits, i), lo = rd_bit(bits, i + 1);
        if (invert) { hi = !hi; lo = !lo; }
        dmr_watch_symbol(symbol_for(hi, lo), CENTER, UMID, LMID);
    }
}

/* Idle either side, so the burst is not the first thing the framer sees and
   the window has to slide onto it. */
static void feed_silence(unsigned symbols)
{
    for (unsigned i = 0; i < symbols; i++)
        dmr_watch_symbol(SYM_P1, CENTER, UMID, LMID);
}

static dmr_lc_t reference_lc(void)
{
    dmr_lc_t lc;
    memset(&lc, 0, sizeof(lc));
    lc.flco            = 0;          /* group voice */
    lc.fid             = 0;
    lc.service_options = 0x00;
    lc.destination     = 2051;       /* talkgroup */
    lc.source          = 1234567;    /* radio ID  */
    return lc;
}

LS_CASE(a_voice_header_in_symbols_comes_out_as_a_talkgroup_and_a_radio_id)
{
    const dmr_lc_t lc = reference_lc();
    uint8_t burst[33];
    dmr_gen_burst(burst, BS_VOICE, 7, 1, &lc);

    dmr_watch_reset();
    feed_silence(40);
    feed_bits(burst, DMR_BURST_BITS, false);
    feed_silence(200);

    dmr_watch_t w;
    LS_CHECK_MSG(dmr_watch_get(&w), "the watcher reported nothing");
    LS_CHECK_MSG(w.bursts >= 1, "no burst was framed out of the symbols");
    LS_EQ_INT((int)w.last_class, (int)DMR_SYNC_BS_VOICE);
    LS_EQ_UINT(w.colour_code, 7);
    LS_CHECK_MSG(!w.inverted, "a normal-polarity burst was reported inverted");
    LS_CHECK_MSG(w.have_lc, "the link control never passed parity");
    if (!w.have_lc) return;
    LS_EQ_UINT(w.lc.destination, lc.destination);
    LS_EQ_UINT(w.lc.source, lc.source);
    LS_EQ_UINT(w.lc.flco, lc.flco);
}

LS_CASE(the_same_burst_decodes_with_the_polarity_the_other_way_round)
{
    /* Which way round the discriminator sits is not known in advance, and a
       receiver that only framed one polarity would be silent on half the
       radios it met. */
    const dmr_lc_t lc = reference_lc();
    uint8_t burst[33];
    dmr_gen_burst(burst, BS_VOICE, 3, 1, &lc);

    dmr_watch_reset();
    feed_silence(40);
    feed_bits(burst, DMR_BURST_BITS, true);
    feed_silence(200);

    dmr_watch_t w;
    LS_CHECK(dmr_watch_get(&w));
    LS_CHECK_MSG(w.bursts >= 1, "an inverted burst was not framed");
    LS_CHECK_MSG(w.inverted, "an inverted burst was reported as normal");
    LS_EQ_UINT(w.colour_code, 3);
    LS_CHECK_MSG(w.have_lc, "inverted link control never passed parity");
    if (!w.have_lc) return;
    LS_EQ_UINT(w.lc.destination, lc.destination);
    LS_EQ_UINT(w.lc.source, lc.source);
}

LS_CASE(noise_alone_never_reports_a_burst)
{
    /* The counters are what a user reads to decide the receiver is working,
       so they must not move on a channel carrying nothing. */
    dmr_watch_reset();
    ls_rng_t rng;
    ls_rng_seed(&rng, 20260920);
    for (int i = 0; i < 20000; i++) {
        const int pick = (int)(ls_rng_u32(&rng) & 3u);
        const int sym = pick == 0 ? SYM_P3 : pick == 1 ? SYM_P1
                      : pick == 2 ? SYM_M1 : SYM_M3;
        dmr_watch_symbol(sym, CENTER, UMID, LMID);
    }

    dmr_watch_t w;
    LS_CHECK(dmr_watch_get(&w));
    LS_EQ_UINT(w.bursts, 0);
    LS_EQ_UINT(w.lc_ok, 0);
    LS_CHECK_MSG(!w.have_lc, "noise produced a link control");
}

LS_CASE(a_data_burst_is_counted_but_carries_no_link_control)
{
    /* Data type 3 is a CSBK, not one of the two that carry an LC. The burst
       still counts - it is evidence the channel is DMR - but nothing is
       published as identity. */
    const dmr_lc_t lc = reference_lc();
    uint8_t burst[33];
    dmr_gen_burst(burst, BS_DATA, 11, 3, &lc);

    dmr_watch_reset();
    feed_silence(40);
    feed_bits(burst, DMR_BURST_BITS, false);
    feed_silence(200);

    dmr_watch_t w;
    LS_CHECK(dmr_watch_get(&w));
    LS_CHECK_MSG(w.bursts >= 1, "a data burst was not framed");
    LS_EQ_INT((int)w.last_class, (int)DMR_SYNC_BS_DATA);
    LS_EQ_UINT(w.colour_code, 11);
    LS_CHECK_MSG(!w.have_lc, "a CSBK was published as link control");
}

LS_CASE(the_watcher_counts_every_symbol_it_is_given)
{
    dmr_watch_reset();
    feed_silence(500);
    dmr_watch_t w;
    LS_CHECK(dmr_watch_get(&w));
    LS_EQ_UINT(w.symbols, 500);
}
