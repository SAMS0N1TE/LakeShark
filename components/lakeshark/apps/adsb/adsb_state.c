
#include "adsb_state.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include <string.h>

#define GATE_WINDOW_US (10 * 1000000LL)

static EXT_RAM_BSS_ATTR adsb_aircraft_t s_aircraft[ADSB_MAX_TRACKED];
static uint32_t        s_selected_icao = 0;

/* Written by the decoder, read by the UI. A point is stored before the
   count that publishes it moves, so a reader sees a whole point or none. */
typedef struct {
    adsb_trail_pt_t pt[ADSB_TRAIL_N];
    uint32_t        icao;
    volatile int    head, n;
    int             squawk, squawk_pending;
} adsb_extra_t;
EXT_RAM_BSS_ATTR static adsb_extra_t s_extra[ADSB_MAX_TRACKED];

static int slot_of(const adsb_aircraft_t *a)
{
    const int i = (int)(a - s_aircraft);
    return (i >= 0 && i < ADSB_MAX_TRACKED) ? i : -1;
}

static void extra_reset(int slot, uint32_t icao)
{
    adsb_extra_t *e = &s_extra[slot];
    e->n = 0;
    e->head = 0;
    e->icao = icao;
    e->squawk = e->squawk_pending = 0;
}

void adsb_state_init(void)
{
    memset(s_aircraft, 0, sizeof(s_aircraft));
    for (int i = 0; i < ADSB_MAX_TRACKED; i++) extra_reset(i, 0);
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
        extra_reset(slot_of(empty), icao);
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

            if (s_selected_icao == a->icao) s_selected_icao = 0;
        }
    }
}

void adsb_state_push_altitude(adsb_aircraft_t *a, int alt_ft)
{
    if (!a) return;

    if (alt_ft >  32000) alt_ft =  32000;
    if (alt_ft < -32000) alt_ft = -32000;
    a->alt_history[a->alt_history_head] = (int16_t)alt_ft;
    a->alt_history_head = (a->alt_history_head + 1) %
        (sizeof(a->alt_history) / sizeof(a->alt_history[0]));
}

static int find_selected_slot(void)
{
    if (s_selected_icao == 0) return -1;
    for (int i = 0; i < ADSB_MAX_TRACKED; i++) {
        if (s_aircraft[i].active && s_aircraft[i].icao == s_selected_icao)
            return i;
    }
    return -1;
}

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

    if (find_selected_slot() < 0) s_selected_icao = 0;
    return s_selected_icao;
}

const adsb_aircraft_t *adsb_select_get(void)
{
    int slot = find_selected_slot();
    if (slot < 0) {

        slot = find_first_active();
        if (slot < 0) return NULL;
        s_selected_icao = s_aircraft[slot].icao;
    }
    return &s_aircraft[slot];
}

static void select_step(int direction)
{
    int cur = find_selected_slot();
    if (cur < 0) {

        cur = (direction > 0) ? -1 : ADSB_MAX_TRACKED;
    }
    for (int n = 0; n < ADSB_MAX_TRACKED; n++) {
        cur = (cur + direction + ADSB_MAX_TRACKED) % ADSB_MAX_TRACKED;
        if (s_aircraft[cur].active) {
            s_selected_icao = s_aircraft[cur].icao;
            return;
        }
    }

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

void adsb_state_push_position(adsb_aircraft_t *a)
{
    const int slot = a ? slot_of(a) : -1;
    if (slot < 0 || !a->pos_valid) return;
    adsb_extra_t *e = &s_extra[slot];
    if (e->icao != a->icao) extra_reset(slot, a->icao);
    if (e->n > 0) {
        const adsb_trail_pt_t *last = &e->pt[(e->head + ADSB_TRAIL_N - 1) % ADSB_TRAIL_N];
        if (a->pos_ts_us - last->ts_us < ADSB_TRAIL_GAP_US) return;
    }
    e->pt[e->head] = (adsb_trail_pt_t){ a->lat, a->lon, a->altitude, a->pos_ts_us };
    e->head = (e->head + 1) % ADSB_TRAIL_N;
    if (e->n < ADSB_TRAIL_N) e->n++;
}

int adsb_state_trail(int slot, adsb_trail_pt_t *out, int max)
{
    if (slot < 0 || slot >= ADSB_MAX_TRACKED || !out || max <= 0) return 0;
    const adsb_extra_t *e = &s_extra[slot];
    if (!s_aircraft[slot].active || e->icao != s_aircraft[slot].icao) return 0;
    const int n = e->n < max ? e->n : max;
    const int head = e->head;
    for (int i = 0; i < n; i++)
        out[i] = e->pt[(head - n + i + 2 * ADSB_TRAIL_N) % ADSB_TRAIL_N];
    return n;
}

void adsb_state_set_squawk(adsb_aircraft_t *a, int code)
{
    const int slot = a ? slot_of(a) : -1;
    if (slot < 0 || code < 0 || code > 7777) return;
    adsb_extra_t *e = &s_extra[slot];
    if (e->icao != a->icao) extra_reset(slot, a->icao);
    if (code == e->squawk) { e->squawk_pending = 0; return; }
    if (code == e->squawk_pending) { e->squawk = code; e->squawk_pending = 0; }
    else e->squawk_pending = code;
}

int adsb_state_squawk(int slot)
{
    if (slot < 0 || slot >= ADSB_MAX_TRACKED) return 0;
    if (!s_aircraft[slot].active || s_extra[slot].icao != s_aircraft[slot].icao) return 0;
    return s_extra[slot].squawk;
}
