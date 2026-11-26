#include "ls_test.h"
#include "ls_gauge_soc.h"

/* Feed the filter n samples dt seconds apart; the last percent. */
static int run(ls_batt_filter_t *f, int n, float dt, int mv, int ma)
{
    int p = -1;
    for (int i = 0; i < n; i++) p = ls_batt_filter_step(f, mv, ma, dt);
    return p;
}

LS_CASE(a_full_cell_under_the_board_load_reads_full)
{
    /* 4.15 V at -350 mA rests near 4.185 V: a charged cell under load. */
    LS_EQ_INT(ls_gauge_rest_mv(4150, -350), 4185);
    LS_EQ_INT(ls_gauge_soc_from_rest_mv(ls_gauge_rest_mv(4150, -350)), 100);
}

LS_CASE(the_curve_is_monotonic_and_bounded)
{
    int last = -1;
    for (int mv = 3000; mv <= 4400; mv += 5) {
        const int p = ls_gauge_soc_from_rest_mv(mv);
        LS_CHECK(p >= 0 && p <= 100);
        LS_CHECK(p >= last);
        last = p;
    }
    LS_EQ_INT(ls_gauge_soc_from_rest_mv(3400), 0);
    LS_EQ_INT(ls_gauge_soc_from_rest_mv(3760), 50);
    LS_EQ_INT(ls_gauge_soc_from_rest_mv(4170), 100);
}

LS_CASE(the_curve_matches_an_nmc_cell_through_the_middle)
{
    /* Published NMC rest voltages, within the curve's deliberate pessimism. */
    LS_CHECK(ls_gauge_soc_from_rest_mv(3740) >= 42 && ls_gauge_soc_from_rest_mv(3740) <= 50);  /* ~50 % */
    LS_CHECK(ls_gauge_soc_from_rest_mv(3980) >= 75 && ls_gauge_soc_from_rest_mv(3980) <= 80);  /* ~80 % */
    LS_CHECK(ls_gauge_soc_from_rest_mv(3570) >= 10 && ls_gauge_soc_from_rest_mv(3570) <= 20);  /* ~20 % */
}

LS_CASE(charging_current_is_taken_off_not_added)
{
    LS_EQ_INT(ls_gauge_rest_mv(4100, 500), 4050);
    LS_EQ_INT(ls_gauge_rest_mv(3700, 0), 3700);
    /* Charging takes more off: the cell's polarisation rides on top. */
    LS_CHECK(ls_batt_instant(4000, 600) < ls_gauge_soc_from_rest_mv(4000 - 60));
}

LS_CASE(what_the_current_says_the_board_is_doing)
{
    LS_EQ_INT(ls_batt_classify(3900, -350), LS_BATT_DISCHARGING);
    LS_EQ_INT(ls_batt_classify(3900, 450), LS_BATT_CHARGING);
    /* The bench board on USB, charger finished: 4123 mV, 0 mA. */
    LS_EQ_INT(ls_batt_classify(4123, 0), LS_BATT_FULL);
    LS_EQ_INT(ls_batt_classify(3800, 3), LS_BATT_PLUGGED);
}

LS_CASE(the_bench_board_on_usb_reads_full_not_92)
{
    /* Measured on COM19: USB in, 4123 mV, 0 mA. The old code showed 92 %
       with no sign of USB. */
    ls_batt_filter_t f;
    ls_batt_filter_init(&f);
    LS_EQ_INT(ls_batt_filter_step(&f, 4123, 0, 0), 100);
    LS_EQ_INT(f.state, LS_BATT_FULL);
    LS_EQ_INT(run(&f, 20, 5, 4123, 0), 100);
}

LS_CASE(a_load_spike_moves_it_a_fraction_not_a_jump)
{
    ls_batt_filter_t f;
    ls_batt_filter_init(&f);
    const int start = run(&f, 30, 5, 3800, -350);         /* settled, past warm-up */
    /* A radio burst: 1.2 A for one sample drops the pack 120 mV more. */
    const int during = ls_batt_filter_step(&f, 3680, -1200, 5);
    LS_CHECK(start - during <= 1);
    /* and back: it never climbs again on battery */
    const int after = run(&f, 10, 5, 3800, -350);
    LS_CHECK(after <= during);
    LS_CHECK(start - after <= 1);
}

LS_CASE(discharge_only_goes_down_and_no_faster_than_a_cell_can)
{
    ls_batt_filter_t f;
    ls_batt_filter_init(&f);
    int last = run(&f, 20, 5, 3950, -400);
    /* the voltage falls a whole 30 % at once (a heavy, long load) */
    for (int i = 0; i < 60; i++) {
        const int p = ls_batt_filter_step(&f, 3700, -400, 5);
        LS_CHECK(p <= last);
        LS_CHECK(last - p <= 1);           /* 5 s: a quarter point at most, so steps of one */
        last = p;
    }
    /* the load goes away and the voltage recovers: still no step up */
    for (int i = 0; i < 60; i++) {
        const int p = ls_batt_filter_step(&f, 3900, -150, 5);
        LS_CHECK(p <= last);
        last = p;
    }
}

LS_CASE(the_first_minute_can_correct_a_boot_reading_upward)
{
    ls_batt_filter_t f;
    ls_batt_filter_init(&f);
    /* the first sample is taken while the radios come up */
    const int boot = ls_batt_filter_step(&f, 3650, -1100, 0);
    const int later = run(&f, 11, 5, 3820, -300);
    LS_CHECK(later > boot);
}

LS_CASE(charging_never_reads_full_and_never_goes_down)
{
    ls_batt_filter_t f;
    ls_batt_filter_init(&f);
    run(&f, 20, 5, 3750, -300);
    const int before = ls_batt_filter_percent(&f);
    /* plugged in: the charger lifts the pack to 4.2 V in its CV phase */
    int last = before;
    for (int i = 0; i < 720; i++) {                   /* an hour */
        const int p = ls_batt_filter_step(&f, i < 40 ? 4000 : 4200, i < 40 ? 600 : 300, 5);
        LS_CHECK(p >= last);
        LS_CHECK(p <= 99);
        last = p;
    }
    LS_EQ_INT(f.state, LS_BATT_CHARGING);
    /* the charger stops: the cell rests at 4.15 V, current 0 */
    LS_EQ_INT(run(&f, 2, 5, 4150, 0), 100);
    LS_EQ_INT(f.state, LS_BATT_FULL);
}

LS_CASE(charging_rises_no_faster_than_a_charger_can_fill)
{
    ls_batt_filter_t f;
    ls_batt_filter_init(&f);
    run(&f, 20, 5, 3700, -300);
    const int before = ls_batt_filter_percent(&f);
    /* five minutes of charging at a voltage that reads 95 % */
    const int after = run(&f, 60, 5, 4180, 500);
    LS_CHECK(after - before <= 300 / 30 + 1);
}

LS_CASE(one_odd_sample_does_not_flip_the_state)
{
    ls_batt_filter_t f;
    ls_batt_filter_init(&f);
    run(&f, 20, 5, 3900, -350);
    ls_batt_filter_step(&f, 3950, 40, 5);              /* one positive blip */
    LS_EQ_INT(f.state, LS_BATT_DISCHARGING);
    ls_batt_filter_step(&f, 3950, 400, 5);             /* and a second: it is charging */
    LS_EQ_INT(f.state, LS_BATT_CHARGING);
}

LS_CASE(unplugging_a_full_board_counts_down_from_100)
{
    ls_batt_filter_t f;
    ls_batt_filter_init(&f);
    run(&f, 10, 5, 4150, 0);
    LS_EQ_INT(ls_batt_filter_percent(&f), 100);
    int last = 100;
    for (int i = 0; i < 40; i++) {
        const int p = ls_batt_filter_step(&f, 4080, -400, 5);
        LS_CHECK(p <= last);
        last = p;
    }
    LS_CHECK(last >= 90);                              /* 4120 mV rest: about 94 % */
    LS_EQ_INT(f.state, LS_BATT_DISCHARGING);
}
