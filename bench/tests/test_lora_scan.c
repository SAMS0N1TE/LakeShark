/* LS_TEST_SOURCES: ${FW}/components/lakeshark/board/ls_lora.c */

#include "ls_test.h"
#include "ls_lora.h"

LS_CASE(the_ends_of_a_sweep_land_on_the_numbers_printed_under_it)
{
    /* The waterfall prints min and max under the plot. A bin grid that put
       the first bin half a step inside min would make those two labels
       wrong by half a bin, every time, on every band. */
    LS_EQ_UINT(902000000u, ls_lora_scan_bin_hz(902000000u, 928000000u, 52, 0));
    LS_EQ_UINT(928000000u, ls_lora_scan_bin_hz(902000000u, 928000000u, 52, 51));
}

LS_CASE(bins_are_ordered_and_evenly_spread)
{
    const uint32_t lo = 902000000u, hi = 928000000u;
    const int n = 52;
    uint32_t prev = ls_lora_scan_bin_hz(lo, hi, n, 0);
    uint32_t first_step = 0;
    for (int i = 1; i < n; i++) {
        const uint32_t f = ls_lora_scan_bin_hz(lo, hi, n, i);
        LS_CHECK(f > prev);
        const uint32_t step = f - prev;
        if (!first_step) first_step = step;
        /* Within a hertz of each other: the divide is done on the span so
           the error cannot accumulate, which a repeated addition would. */
        LS_CHECK(step >= first_step - 1 && step <= first_step + 1);
        prev = f;
    }
    LS_EQ_UINT(hi, prev);
}

LS_CASE(a_wider_band_does_not_overflow_the_span_arithmetic)
{
    /* The part's whole range. 810 MHz times a bin index does not fit in 32
       bits, and the intermediate is where that shows up. */
    const uint32_t lo = 150000000u, hi = 960000000u;
    LS_EQ_UINT(lo, ls_lora_scan_bin_hz(lo, hi, 256, 0));
    LS_EQ_UINT(hi, ls_lora_scan_bin_hz(lo, hi, 256, 255));
    const uint32_t mid = ls_lora_scan_bin_hz(lo, hi, 256, 128);
    LS_CHECK(mid > 550000000u && mid < 560000000u);
}

LS_CASE(degenerate_bin_counts_return_a_frequency_in_band)
{
    /* Never out of range and never a divide by zero: a single-bin sweep is a
       legitimate thing to ask for and a zero-bin one is a caller bug that
       must not take the radio somewhere it should not be. */
    LS_EQ_UINT(902000000u, ls_lora_scan_bin_hz(902000000u, 928000000u, 1, 0));
    LS_EQ_UINT(902000000u, ls_lora_scan_bin_hz(902000000u, 928000000u, 0, 0));
    LS_EQ_UINT(902000000u, ls_lora_scan_bin_hz(902000000u, 928000000u, 52, -5));
    LS_EQ_UINT(928000000u, ls_lora_scan_bin_hz(902000000u, 928000000u, 52, 999));
    /* An inverted band is refused by scan_begin; here it must at least not
       compute a frequency below the bottom of it. */
    LS_EQ_UINT(928000000u, ls_lora_scan_bin_hz(928000000u, 902000000u, 52, 7));
}

LS_CASE(the_frequency_register_matches_the_datasheets_own_worked_example)
{
    /* Steps of Fxtal / 2^25 with Fxtal at 32 MHz, so one step is 0.95367 Hz
       and the register is freq * 2^25 / 32e6. Checked against the values the
       vendor driver writes for the two bands this board uses. */
    LS_EQ_UINT(0x39300000u, ls_lora_freq_steps(915000000u));
    LS_EQ_UINT(0x36400000u, ls_lora_freq_steps(868000000u));
    LS_EQ_UINT(0x1B200000u, ls_lora_freq_steps(434000000u));
}

LS_CASE(the_register_value_is_monotonic_across_the_whole_tuning_range)
{
    /* The shift is done in 64 bits. In 32 it wraps somewhere above 128 MHz,
       and the symptom is a sweep that tunes backwards through part of the
       band while reporting the frequencies it meant to use. */
    uint32_t prev = 0;
    for (uint32_t mhz = 150; mhz <= 960; mhz += 5) {
        const uint32_t v = ls_lora_freq_steps(mhz * 1000000u);
        LS_CHECK(v > prev);
        prev = v;
    }
}

LS_CASE(every_bin_of_a_real_sweep_converts_to_a_register_value_in_band)
{

    const uint32_t lo = 902000000u, hi = 928000000u;
    const uint32_t lo_steps = ls_lora_freq_steps(lo);
    const uint32_t hi_steps = ls_lora_freq_steps(hi);
    for (int i = 0; i < 52; i++) {
        const uint32_t v = ls_lora_freq_steps(
            ls_lora_scan_bin_hz(lo, hi, 52, i));
        LS_CHECK(v >= lo_steps && v <= hi_steps);
    }
}

static const uint32_t RUNG[] = {
    7810, 10420, 15630, 20830, 31250, 41670, 62500, 125000, 250000, 500000,
};
#define RUNG_N ((int)(sizeof(RUNG) / sizeof(RUNG[0])))

static const struct { const char *name; uint32_t lo, hi; } PRESET[] = {
    { "mesh watch",  909500000u, 911500000u },
    { "US915 ISM",   902000000u, 928000000u },
    { "EU868",       863000000u, 870000000u },
    { "433 ISM",     433050000u, 434790000u },
    { "315 remotes", 314000000u, 316000000u },
    { "full range",  150000000u, 960000000u },
};
#define PRESET_N ((int)(sizeof(PRESET) / sizeof(PRESET[0])))

static int rung_of(uint32_t hz)
{
    for (int r = 0; r < RUNG_N; r++) if (RUNG[r] == hz) return r;
    return -1;
}

LS_CASE(the_narrow_presets_are_no_longer_read_through_a_500khz_filter)
{
    /* The four the fix is for, by name and by rung. Against the old driver
       every one of these was 500000. */
    ls_lora_scan_plan_t p;
    ls_lora_scan_plan(909500000u, 911500000u, LS_LORA_SCAN_BINS, &p);
    LS_EQ_UINT(41670u, p.bw_hz);
    LS_EQ_INT(1, p.looks);
    ls_lora_scan_plan(433050000u, 434790000u, LS_LORA_SCAN_BINS, &p);
    LS_EQ_UINT(31250u, p.bw_hz);
    ls_lora_scan_plan(863000000u, 870000000u, LS_LORA_SCAN_BINS, &p);
    LS_EQ_UINT(125000u, p.bw_hz);
    ls_lora_scan_plan(314000000u, 316000000u, LS_LORA_SCAN_BINS, &p);
    LS_EQ_UINT(41670u, p.bw_hz);
}

LS_CASE(every_filter_covers_the_gap_to_its_neighbour_and_no_narrower_rung_would)
{
    for (int s = 0; s < PRESET_N; s++) {
        ls_lora_scan_plan_t p;
        ls_lora_scan_plan(PRESET[s].lo, PRESET[s].hi, LS_LORA_SCAN_BINS, &p);
        const uint64_t span = PRESET[s].hi - PRESET[s].lo;
        const uint64_t gaps = LS_LORA_SCAN_BINS - 1;
        const int r = rung_of(p.bw_hz);
        LS_CHECK_MSG(r >= 0, "%s: %lu Hz is not a rung of the ladder",
                     PRESET[s].name, (unsigned long)p.bw_hz);
        if (r < 0) continue;
        /* Covered: the filters, or the looks inside each bin, reach from one
           bin centre to the next with nothing between them unmeasured. */
        LS_CHECK_MSG((uint64_t)p.bw_hz * (uint64_t)p.looks * gaps >= span,
                     "%s: %d look(s) of %lu Hz leave gaps between bins",
                     PRESET[s].name, p.looks, (unsigned long)p.bw_hz);
        /* Narrowest: the rung below would not have covered the spacing. */
        if (r > 0)
            LS_CHECK_MSG((uint64_t)RUNG[r - 1] * gaps < span,
                         "%s: %lu Hz would have covered it too - the plan "
                         "took a wider filter than it needed",
                         PRESET[s].name, (unsigned long)RUNG[r - 1]);
        /* One look while the filter covers a bin; looks only once it cannot. */
        if (p.bw_hz < 500000u) LS_EQ_INT(1, p.looks);
    }
}

LS_CASE(the_case_that_was_measured_is_the_case_it_was_measured_in)
{
    /* US915 at 64 bins is 413 kHz apart, so still 500 kHz, one look and the
       300 us measured on the board. None of it may move. */
    ls_lora_scan_plan_t p;
    ls_lora_scan_plan(902000000u, 928000000u, LS_LORA_SCAN_BINS, &p);
    LS_EQ_UINT(500000u, p.bw_hz);
    LS_EQ_INT(1, p.looks);
    LS_EQ_UINT(300u, p.settle_us);

    /* Nothing to plan from - one bin, or a band with no width - is what
       every sweep was before. */
    ls_lora_scan_plan(902000000u, 928000000u, 1, &p);
    LS_EQ_UINT(500000u, p.bw_hz);
    LS_EQ_INT(1, p.looks);
    LS_EQ_UINT(300u, p.settle_us);
    ls_lora_scan_plan(915000000u, 915000000u, 64, &p);
    LS_EQ_UINT(500000u, p.bw_hz);
    LS_EQ_INT(1, p.looks);
}

LS_CASE(a_narrower_filter_waits_longer_by_its_own_time_constant)
{

    uint32_t wider = 0;
    for (int r = RUNG_N - 1; r >= 0; r--) {
        ls_lora_scan_plan_t p;
        const uint32_t lo = 150000000u;
        ls_lora_scan_plan(lo, lo + RUNG[r] * 63u, 64, &p);
        LS_EQ_UINT(RUNG[r], p.bw_hz);
        const uint32_t taus = (8u * 1000000u + RUNG[r] - 1) / RUNG[r];
        LS_EQ_UINT(300u + taus - 16u, p.settle_us);
        if (wider) LS_CHECK_MSG(p.settle_us > wider,
                                "%lu Hz waits %lu us, no longer than the "
                                "wider rung's %lu",
                                (unsigned long)RUNG[r],
                                (unsigned long)p.settle_us,
                                (unsigned long)wider);
        wider = p.settle_us;
    }
    ls_lora_scan_plan_t p;
    ls_lora_scan_plan(909500000u, 911500000u, 64, &p);
    LS_EQ_UINT(476u, p.settle_us);
    ls_lora_scan_plan(433050000u, 434790000u, 64, &p);
    LS_EQ_UINT(540u, p.settle_us);
}

LS_CASE(one_look_puts_every_bin_exactly_where_it_always_was)
{
    /* The waterfall prints min and max under the plot (see the first case in
       this file); a bin read through one look must still sit on its centre. */
    static const int NS[] = { 2, 32, 52, 64 };
    for (unsigned t = 0; t < sizeof(NS) / sizeof(NS[0]); t++)
        for (int i = 0; i < NS[t]; i++) {
            LS_EQ_UINT(ls_lora_scan_bin_hz(902000000u, 928000000u, NS[t], i),
                       ls_lora_scan_look_hz(902000000u, 928000000u, NS[t], i, 1, 0));
            LS_EQ_UINT(ls_lora_scan_bin_hz(150000000u, 960000000u, NS[t], i),
                       ls_lora_scan_look_hz(150000000u, 960000000u, NS[t], i, 1, 0));
        }
}

LS_CASE(full_range_is_measured_end_to_end_with_no_gap_wider_than_the_filter)
{
    /* The other half of the report. 26 looks a bin, walked in the order the
       rows are built: never outside the band, never backwards, and never
       further apart than one filter width - so every frequency from 150 to
       960 MHz is inside some look's filter. */
    const uint32_t lo = 150000000u, hi = 960000000u;
    ls_lora_scan_plan_t p;
    ls_lora_scan_plan(lo, hi, LS_LORA_SCAN_BINS, &p);
    LS_EQ_UINT(500000u, p.bw_hz);
    LS_EQ_INT(26, p.looks);

    uint32_t prev = 0;
    uint32_t worst = 0;
    for (int i = 0; i < LS_LORA_SCAN_BINS; i++)
        for (int k = 0; k < p.looks; k++) {
            const uint32_t f = ls_lora_scan_look_hz(lo, hi, LS_LORA_SCAN_BINS,
                                                    i, p.looks, k);
            LS_CHECK(f >= lo && f <= hi);
            if (i == 0 && k == 0) {
                LS_CHECK(f <= lo + p.bw_hz / 2);
            } else {
                LS_CHECK_MSG(f >= prev, "look %d of bin %d went backwards", k, i);
                if (f - prev > worst) worst = f - prev;
            }
            prev = f;
        }
    LS_CHECK(prev >= hi - p.bw_hz / 2);
    LS_CHECK_MSG(worst <= p.bw_hz,
                 "two looks %lu Hz apart through a %lu Hz filter leave a gap",
                 (unsigned long)worst, (unsigned long)p.bw_hz);
}

LS_CASE(no_bin_is_the_peak_of_more_looks_than_the_cap)
{
    /* Two bins over the whole range would want 1620 looks each. A row that
       took that many passes would be minutes, so the cap holds and the bin
       samples part of its slice instead. */
    ls_lora_scan_plan_t p;
    ls_lora_scan_plan(150000000u, 960000000u, 2, &p);
    LS_EQ_INT(LS_LORA_SCAN_LOOKS_MAX, p.looks);
}

LS_CASE(no_preset_costs_much_more_a_pass_than_us915_did)
{
    /* 780 us a bin was measured at 500 kHz (), 300 of it the settle.
       The rest is SPI and BUSY and does not change with the filter, so a
       pass costs 64 x (480 + settle). The sweep runs on the draw path; a
       narrower filter may cost a third again and no more. */
    const uint64_t us915 = 64ull * 780u;
    for (int s = 0; s < PRESET_N; s++) {
        ls_lora_scan_plan_t p;
        ls_lora_scan_plan(PRESET[s].lo, PRESET[s].hi, LS_LORA_SCAN_BINS, &p);
        const uint64_t pass = 64ull * (480u + p.settle_us);
        LS_CHECK_MSG(pass * 3 <= us915 * 4,
                     "%s: a pass costs %llu us against US915's %llu",
                     PRESET[s].name, (unsigned long long)pass,
                     (unsigned long long)us915);
    }
}
