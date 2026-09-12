/* LS_TEST_SOURCES: ${APP}/adsb/adsb_state.c */

#include "ls_test.h"
#include "adsb_state.h"
#include "esp_timer.h"

#include <string.h>

#define TIMEOUT_US   (30 * 1000000LL)
#define GATE_US      (10 * 1000000LL)

static int s_lost, s_announced;
static uint32_t s_last_lost_icao;

static void on_lost(adsb_aircraft_t *a) { s_lost++; s_last_lost_icao = a->icao; }
static void on_announce(adsb_aircraft_t *a) { (void)a; s_announced++; }

static void fresh(void)
{
    ls_shim_time_set(0);
    adsb_state_init();
    s_lost = 0;
    s_announced = 0;
    s_last_lost_icao = 0;
}

/* Create an aircraft and mark it heard now. */
static adsb_aircraft_t *seen(uint32_t icao)
{
    adsb_aircraft_t *a = adsb_state_find_or_create(icao);
    if (a) {
        a->last_seen_us = esp_timer_get_time();
        a->good_msg_count++;
    }
    return a;
}

/* ---------------------------------------------------------------- cases -- */

LS_CASE(the_same_aircraft_reuses_its_slot)
{
    /* Every message from one aircraft must land in one slot, or sixteen
       messages fill the table with one aeroplane. */
    fresh();
    adsb_aircraft_t *a = seen(0xABCDEF);
    LS_CHECK(a != NULL);
    LS_EQ_INT(1, adsb_state_active_count());

    for (int i = 0; i < 20; i++) LS_CHECK(seen(0xABCDEF) == a);
    LS_EQ_INT(1, adsb_state_active_count());
}

LS_CASE(a_full_table_refuses_rather_than_evicting)
{

    fresh();
    for (uint32_t i = 0; i < ADSB_MAX_TRACKED; i++)
        LS_CHECK_MSG(seen(0xA00000 + i) != NULL, "slot %u was refused", i);
    LS_EQ_INT(ADSB_MAX_TRACKED, adsb_state_active_count());

    LS_CHECK_MSG(adsb_state_find_or_create(0xBBBBBB) == NULL,
                 "a full table accepted another aircraft");
    LS_EQ_INT(ADSB_MAX_TRACKED, adsb_state_active_count());
}

LS_CASE(an_aircraft_not_heard_for_the_timeout_is_dropped)
{
    fresh();
    seen(0x111111);
    seen(0x222222);
    LS_EQ_INT(2, adsb_state_active_count());

    /* Just inside the timeout: both stay. */
    ls_shim_time_advance(TIMEOUT_US - 1);
    adsb_state_age_out(esp_timer_get_time(), TIMEOUT_US, on_lost, NULL);
    LS_EQ_INT(2, adsb_state_active_count());
    LS_EQ_INT(0, s_lost);

    /* One of them is heard again; the other is not. */
    seen(0x222222);
    ls_shim_time_advance(2);
    adsb_state_age_out(esp_timer_get_time(), TIMEOUT_US, on_lost, NULL);

    LS_EQ_INT(1, adsb_state_active_count());
    LS_EQ_INT(1, s_lost);
    LS_EQ_INT((int)0x111111, (int)s_last_lost_icao);
}

LS_CASE(a_dropped_slot_is_available_again)
{
    /* The point of ageing. If a freed slot were not reusable the table would
       fill permanently after sixteen aircraft had ever been seen. */
    fresh();
    for (uint32_t i = 0; i < ADSB_MAX_TRACKED; i++) seen(0xA00000 + i);
    LS_CHECK(adsb_state_find_or_create(0xBBBBBB) == NULL);

    ls_shim_time_advance(TIMEOUT_US + 1);
    adsb_state_age_out(esp_timer_get_time(), TIMEOUT_US, NULL, NULL);
    LS_EQ_INT(0, adsb_state_active_count());

    LS_CHECK_MSG(adsb_state_find_or_create(0xBBBBBB) != NULL,
                 "a table emptied by ageing still refused a new aircraft");
}

LS_CASE(a_selected_aircraft_that_ages_out_clears_the_selection)
{
    /* The failure this file exists for. The selection is held by ICAO, and
       the slot it named is about to be reused. Leaving it set means the next
       aircraft into that slot appears already selected. */
    fresh();
    seen(0x111111);
    seen(0x222222);
    adsb_select_set_icao(0x111111);
    LS_EQ_INT((int)0x111111, (int)adsb_select_get_icao());

    /* Keep the other one alive so the table does not simply empty. */
    ls_shim_time_advance(TIMEOUT_US - 1);
    seen(0x222222);
    ls_shim_time_advance(2);
    adsb_state_age_out(esp_timer_get_time(), TIMEOUT_US, NULL, NULL);

    LS_CHECK_MSG(adsb_select_get_icao() != 0x111111,
                 "the selection still names an aircraft that was dropped");
    LS_CHECK_MSG(adsb_select_get() == NULL ||
                 adsb_select_get()->icao != 0x111111,
                 "the selection still resolves to the dropped aircraft");
}

LS_CASE(the_late_announce_fires_once_and_only_with_a_message_behind_it)
{
    /* The gate window exists so a single noise-decoded ICAO does not announce
       an aircraft that never existed. After it, a track with real messages
       announces - once. */
    fresh();
    adsb_aircraft_t *a = adsb_state_find_or_create(0x333333);
    LS_CHECK(a != NULL);
    a->last_seen_us = esp_timer_get_time();
    /* No good messages yet. */

    ls_shim_time_advance(GATE_US + 1);
    adsb_state_age_out(esp_timer_get_time(), TIMEOUT_US, NULL, on_announce);
    LS_CHECK_MSG(s_announced == 0,
                 "an aircraft with no good messages was announced");

    a->good_msg_count = 1;
    a->last_seen_us = esp_timer_get_time();
    adsb_state_age_out(esp_timer_get_time(), TIMEOUT_US, NULL, on_announce);
    LS_EQ_INT(1, s_announced);

    /* And not again on every later sweep. */
    for (int i = 0; i < 5; i++) {
        a->last_seen_us = esp_timer_get_time();
        adsb_state_age_out(esp_timer_get_time(), TIMEOUT_US, NULL, on_announce);
    }
    LS_EQ_INT(1, s_announced);
}

LS_CASE(nothing_announces_inside_the_gate_window)
{
    fresh();
    adsb_aircraft_t *a = adsb_state_find_or_create(0x444444);
    a->good_msg_count = 5;
    a->last_seen_us = esp_timer_get_time();

    ls_shim_time_advance(GATE_US - 1);
    adsb_state_age_out(esp_timer_get_time(), TIMEOUT_US, NULL, on_announce);
    LS_CHECK_MSG(s_announced == 0, "announced before the gate window closed");
}

LS_CASE(stepping_the_selection_stays_on_aircraft_that_exist)
{
    /* The screen walks the list with the arrows. Every stop has to be a live
       aircraft, whatever gaps ageing has left in the table. */
    fresh();
    seen(0x111111);
    seen(0x222222);
    seen(0x333333);
    seen(0x444444);

    /* Drop the middle two, leaving holes between the survivors. */
    adsb_aircraft_t *b = adsb_state_find_or_create(0x222222);
    adsb_aircraft_t *c = adsb_state_find_or_create(0x333333);
    b->last_seen_us = 0;
    c->last_seen_us = 0;
    ls_shim_time_advance(TIMEOUT_US + 1);
    seen(0x111111);
    seen(0x444444);
    adsb_state_age_out(esp_timer_get_time(), TIMEOUT_US, NULL, NULL);
    LS_EQ_INT(2, adsb_state_active_count());

    adsb_select_set_icao(0x111111);
    for (int i = 0; i < 8; i++) {
        adsb_select_next();
        const adsb_aircraft_t *sel = adsb_select_get();
        LS_CHECK_MSG(sel != NULL, "the selection stepped onto nothing");
        LS_CHECK_MSG(sel->active,
                     "the selection stepped onto an inactive slot (%06lX)",
                     (unsigned long)sel->icao);
    }
    for (int i = 0; i < 8; i++) {
        adsb_select_prev();
        const adsb_aircraft_t *sel = adsb_select_get();
        LS_CHECK(sel != NULL && sel->active);
    }
}

LS_CASE(the_selection_index_counts_only_live_aircraft)
{
    /* The screen shows "n of m". Counting dropped slots would show a total
       the list cannot reach. */
    fresh();
    seen(0x111111);
    seen(0x222222);
    seen(0x333333);

    adsb_aircraft_t *b = adsb_state_find_or_create(0x222222);
    b->last_seen_us = 0;
    ls_shim_time_advance(TIMEOUT_US + 1);
    seen(0x111111);
    seen(0x333333);
    adsb_state_age_out(esp_timer_get_time(), TIMEOUT_US, NULL, NULL);

    adsb_select_set_icao(0x333333);
    int idx = -1, total = -1;
    adsb_select_index(&idx, &total);
    LS_EQ_INT(2, total);
    LS_CHECK_MSG(idx >= 0 && idx < total,
                 "index %d is outside a total of %d", idx, total);
}

LS_CASE(an_empty_table_has_no_selection_to_report)
{
    fresh();
    int idx = 99, total = 99;
    adsb_select_index(&idx, &total);
    LS_EQ_INT(0, total);
    LS_EQ_INT(-1, idx);
    LS_CHECK(adsb_select_get() == NULL);

    /* And stepping it does not wander off the table. */
    adsb_select_next();
    adsb_select_prev();
    LS_CHECK(adsb_select_get() == NULL);
}

LS_CASE(a_slot_out_of_range_is_refused)
{
    fresh();
    LS_CHECK(adsb_state_get(-1) == NULL);
    LS_CHECK(adsb_state_get(ADSB_MAX_TRACKED) == NULL);
    LS_CHECK(adsb_state_get(9999) == NULL);
}

LS_CASE(a_new_aircraft_starts_with_an_empty_altitude_history)
{

    fresh();
    adsb_aircraft_t *a = seen(0x555555);
    LS_CHECK(a != NULL);
    for (unsigned i = 0; i < sizeof(a->alt_history) / sizeof(a->alt_history[0]); i++)
        LS_CHECK_MSG(a->alt_history[i] == -1,
                     "altitude history slot %u started at %d instead of -1",
                     i, a->alt_history[i]);
}
