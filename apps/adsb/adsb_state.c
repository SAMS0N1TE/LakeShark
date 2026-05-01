/* adsb_state.c - aircraft table, find/create, aging, selection. */

#include "adsb_state.h"
#include "esp_timer.h"
#include <string.h>

#define GATE_WINDOW_US (10 * 1000000LL)

static adsb_aircraft_t s_aircraft[ADSB_MAX_TRACKED];
static uint32_t        s_selected_icao = 0;

void adsb_state_init(void)
{
    memset(s_aircraft, 0, sizeof(s_aircraft));
    s_selected_icao = 0;
}

adsb_aircraft_t *adsb_state_find_or_create(uint32_t icao)
{
    adsb_aircraft_t *empty = NULL;
    for (int i = 0; i < ADSB_MAX_TRACKED; i++) {
        if (s_aircraft[i].active && s_aircraft[i].icao == icao)
            return &s_aircraft[i];
        if (!s_aircraft[i].active && !empty)
            empty = &s_aircraft[i];
    }
    if (empty) {
        memset(empty, 0, sizeof(*empty));
        empty->icao          = icao;
        empty->active        = true;
        empty->first_seen_us = esp_timer_get_time();
        for (size_t i = 0; i < sizeof(empty->alt_history) / sizeof(empty->alt_history[0]); i++)
            empty->alt_history[i] = -1;
    }
    return empty;
}

const adsb_aircraft_t *adsb_state_get(int slot)
{
    if (slot < 0 || slot >= ADSB_MAX_TRACKED) return NULL;
    return &s_aircraft[slot];
}

int adsb_state_active_count(void)
{
    int n = 0;
    for (int i = 0; i < ADSB_MAX_TRACKED; i++)
        if (s_aircraft[i].active) n++;
    return n;
}

void adsb_state_age_out(int64_t now_us, int64_t timeout_us,
                        void (*on_lost)(adsb_aircraft_t *),
                        void (*on_late_announce)(adsb_aircraft_t *))
{
    for (int i = 0; i < ADSB_MAX_TRACKED; i++) {
        adsb_aircraft_t *a = &s_aircraft[i];
        if (!a->active) continue;

        if (!a->announced && a->good_msg_count >= 1 &&
            (now_us - a->first_seen_us) > GATE_WINDOW_US) {
            a->announced = true;
            if (on_late_announce) on_late_announce(a);
        }

        if (now_us - a->last_seen_us > timeout_us) {
            if (on_lost) on_lost(a);
            a->active = false;
            /* If the user had this aircraft selected for the detail
             * view, drop the selection so the next render falls back
             * to first-active or shows the empty state. */
            if (s_selected_icao == a->icao) s_selected_icao = 0;
        }
    }
}

void adsb_state_push_altitude(adsb_aircraft_t *a, int alt_ft)
{
    if (!a) return;
    /* Clamp to int16 range so the ring fits. Real altitudes never
     * exceed ~60000 ft so this never actually clips a real value. */
    if (alt_ft >  32000) alt_ft =  32000;
    if (alt_ft < -32000) alt_ft = -32000;
    a->alt_history[a->alt_history_head] = (int16_t)alt_ft;
    a->alt_history_head = (a->alt_history_head + 1) %
        (sizeof(a->alt_history) / sizeof(a->alt_history[0]));
}

/* Helper: find the slot containing the currently-selected ICAO, or
 * -1 if no selection or the selected aircraft has aged out. */
static int find_selected_slot(void)
{
    if (s_selected_icao == 0) return -1;
    for (int i = 0; i < ADSB_MAX_TRACKED; i++) {
        if (s_aircraft[i].active && s_aircraft[i].icao == s_selected_icao)
            return i;
    }
    return -1;
}

/* Helper: find the first active slot, or -1 if none. */
static int find_first_active(void)
{
    for (int i = 0; i < ADSB_MAX_TRACKED; i++)
        if (s_aircraft[i].active) return i;
    return -1;
}

void adsb_select_set_icao(uint32_t icao)
{
    s_selected_icao = icao;
}

uint32_t adsb_select_get_icao(void)
{
    /* Validate that the cached ICAO is still in the active set; if not,
     * return 0 so callers don't hold stale references. */
    if (find_selected_slot() < 0) s_selected_icao = 0;
    return s_selected_icao;
}

const adsb_aircraft_t *adsb_select_get(void)
{
    int slot = find_selected_slot();
    if (slot < 0) {
        /* Auto-select first active when there's no valid selection.
         * This makes the detail page show *something* the moment the
         * user navigates to it, without requiring an explicit pick. */
        slot = find_first_active();
        if (slot < 0) return NULL;
        s_selected_icao = s_aircraft[slot].icao;
    }
    return &s_aircraft[slot];
}

/* Walk active slots in numeric slot order. Direction = +1 for next,
 * -1 for prev. Wraps at the ends. */
static void select_step(int direction)
{
    int cur = find_selected_slot();
    if (cur < 0) {
        /* No valid current selection — pick whichever end the user is
         * stepping toward. */
        cur = (direction > 0) ? -1 : ADSB_MAX_TRACKED;
    }
    for (int n = 0; n < ADSB_MAX_TRACKED; n++) {
        cur = (cur + direction + ADSB_MAX_TRACKED) % ADSB_MAX_TRACKED;
        if (s_aircraft[cur].active) {
            s_selected_icao = s_aircraft[cur].icao;
            return;
        }
    }
    /* No active aircraft at all — clear selection. */
    s_selected_icao = 0;
}

void adsb_select_next(void) { select_step(+1); }
void adsb_select_prev(void) { select_step(-1); }

void adsb_select_index(int *out_index, int *out_total)
{
    int total = 0, idx = -1;
    int sel = find_selected_slot();
    for (int i = 0; i < ADSB_MAX_TRACKED; i++) {
        if (!s_aircraft[i].active) continue;
        if (i == sel) idx = total;
        total++;
    }
    if (out_index) *out_index = idx;
    if (out_total) *out_total = total;
}
