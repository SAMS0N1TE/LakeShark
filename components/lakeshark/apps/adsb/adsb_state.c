
#include "adsb_state.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>

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
    return adsb_state_find_address(icao, 0);
}

const char *adsb_state_source_label(const adsb_aircraft_t *a, int64_t now)
{
    if (!a) return "--";
    bool modes = (a->sources & 1) && now - a->source_seen_us[0] <= ADSB_FIELD_STALE_US;
    bool uat = (a->sources & 2) && now - a->source_seen_us[1] <= ADSB_FIELD_STALE_US;
    if (modes && uat) return "BOTH";
    if (uat) return "UAT";
    if (modes) return "1090";
    return a->sources == 3 ? "BOTH*" : a->sources == 2 ? "UAT*" : "1090*";
}

adsb_aircraft_t *adsb_state_find_address(uint32_t icao, uint8_t qualifier)
{
    if (icao > 0xFFFFFF || qualifier > 5) return NULL;
    /* ADS-B and TIS-B ICAO addresses share an identity. Other address
       namespaces must never alias a 1090 ICAO target with the same bits. */
    if (qualifier == 2) qualifier = 0;
    adsb_aircraft_t *empty = NULL;
    for (int i = 0; i < ADSB_MAX_TRACKED; i++) {
        if (s_aircraft[i].active && s_aircraft[i].icao == icao &&
            s_aircraft[i].address_qualifier == qualifier)
            return &s_aircraft[i];
        if (!s_aircraft[i].active && !empty)
            empty = &s_aircraft[i];
    }
    if (empty) {
        memset(empty, 0, sizeof(*empty));
        empty->icao          = icao;
        empty->address_qualifier = qualifier;
        empty->active        = true;
        empty->first_seen_us = esp_timer_get_time();
        for (size_t i = 0; i < sizeof(empty->alt_history) / sizeof(empty->alt_history[0]); i++)
            empty->alt_history[i] = ADSB_ALT_UNKNOWN;
        extra_reset(slot_of(empty), icao);
    }
    return empty;
}

adsb_aircraft_t *adsb_state_update_uat(const uat_fix_t *fix, int64_t now)
{
    if (!fix) return NULL;
    adsb_aircraft_t *a = adsb_state_find_address(fix->address, fix->qualifier);
    if (!a) return NULL;
    if (fix->callsign[0]) memcpy(a->callsign, fix->callsign, sizeof(a->callsign));
    a->uat_qualifier = fix->qualifier;
    a->sources |= 2;
    a->source_seen_us[1] = now;
    a->last_seen_us = now;
    a->msg_count++;
    a->good_msg_count++;
    a->rebroadcast = fix->qualifier == 2 || fix->qualifier == 3;
    if (fix->altitude_valid) {
        int delta = abs(fix->altitude - a->altitude);
        if (!a->altitude_valid || delta < 5000 || a->pending_alt == fix->altitude) {
            a->altitude = fix->altitude;
            a->altitude_valid = true;
            a->altitude_ts_us = now;
            a->pending_alt = 0;
            adsb_state_push_altitude(a, a->altitude);
        } else a->pending_alt = fix->altitude;
    }
    if (fix->velocity_valid) {
        a->ns_velocity = fix->ns_velocity;
        a->ew_velocity = fix->ew_velocity;
        int speed = (int)lround(hypot(fix->ns_velocity, fix->ew_velocity));
        if (!a->velocity_valid || abs(speed - a->velocity) < 200 || a->pending_vel == speed) {
            a->velocity = speed;
            a->velocity_valid = true;
            a->velocity_ts_us = now;
            a->pending_vel = 0;
        } else a->pending_vel = speed;
        if (speed) {
            int heading = (int)lround(atan2(fix->ew_velocity, fix->ns_velocity) * (180.0 / 3.141592653589793));
            if (heading < 0) heading += 360;
            if (heading >= 360) heading -= 360;
            int delta = abs(heading - a->heading);
            if (delta > 180) delta = 360 - delta;
            if (!a->heading_valid || delta < 90 || a->pending_hdg == heading) {
                a->heading = heading;
                a->heading_valid = a->heading_is_track = true;
                a->heading_ts_us = now;
                a->pending_hdg = 0;
            } else a->pending_hdg = heading;
        }
    }
    if (fix->vert_rate_valid) {
        if (!a->vert_rate_valid || abs(fix->vert_rate - a->vert_rate) < 2000 || a->pending_vs == fix->vert_rate) {
            a->vert_rate = fix->vert_rate;
            a->vert_rate_valid = true;
            a->vert_rate_ts_us = now;
            a->pending_vs = 0;
        } else a->pending_vs = fix->vert_rate;
    }
    if (fix->position && isfinite(fix->lat) && isfinite(fix->lon) &&
        fix->lat >= -90 && fix->lat <= 90 && fix->lon >= -180 && fix->lon <= 180) {
        a->lat = fix->lat;
        a->lon = fix->lon;
        a->pos_valid = true;
        a->pos_ts_us = now;
        adsb_state_push_position(a);
    }
    return a;
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

            if (s_selected_icao == adsb_state_key(a)) s_selected_icao = 0;
        }
    }
}

void adsb_state_push_altitude(adsb_aircraft_t *a, int alt_ft)
{
    if (!a) return;

    a->alt_history[a->alt_history_head] = alt_ft;
    a->alt_history_head = (a->alt_history_head + 1) %
        (sizeof(a->alt_history) / sizeof(a->alt_history[0]));
}

static int find_selected_slot(void)
{
    if (s_selected_icao == 0) return -1;
    for (int i = 0; i < ADSB_MAX_TRACKED; i++) {
        if (s_aircraft[i].active && adsb_state_key(&s_aircraft[i]) == s_selected_icao)
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
        s_selected_icao = adsb_state_key(&s_aircraft[slot]);
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
            s_selected_icao = adsb_state_key(&s_aircraft[cur]);
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
