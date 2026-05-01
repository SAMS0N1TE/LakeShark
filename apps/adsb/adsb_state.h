/*
 * adsb_state.h - canonical ADS-B aircraft state and read accessors.
 *
 * Owns the aircraft tracking table. adsb_decode.c writes; renderers
 * (tui, json) read. No internal/public struct duality - everyone uses
 * adsb_aircraft_t directly.
 */
#ifndef ADSB_STATE_H
#define ADSB_STATE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADSB_MAX_TRACKED 16

typedef struct {
    int     raw_lat;
    int     raw_lon;
    int64_t ts_us;
    bool    valid;
} adsb_cpr_frame_t;

typedef struct {
    uint32_t         icao;
    char             callsign[9];
    int              altitude;
    int              velocity;
    int              heading;
    float            lat;
    float            lon;
    bool             pos_valid;
    int              ew_velocity;
    int              ns_velocity;
    int              vert_rate;
    int              msg_count;
    int64_t          last_seen_us;
    bool             active;

    int64_t          first_seen_us;
    int              good_msg_count;
    int              crc_err_count;
    bool             announced;

    /* Pending values awaiting confirmation. When a decoded field jumps
     * far from the established value we don't commit it — we stash it
     * here, and only promote to the live field if the next decode
     * agrees. Single bad messages get filtered; sustained changes
     * (e.g., aircraft actually turning) commit on the second read.
     * Zero means "no pending change". */
    int              pending_alt;
    int              pending_vel;
    int              pending_hdg;
    int              pending_vs;

    /* Per-aircraft message-type counters. Helps the detail view answer
     * "why is this row empty?" — a contact that only sends DF11 has
     * nothing else to display. Categories chosen to map 1:1 to what
     * actually populates the table fields:
     *   df11        - ICAO-only all-call replies (no payload data)
     *   df17_pos    - airborne position (BDS05/06)         metype  9-18
     *   df17_vel    - airborne velocity (BDS09)            metype 19-22
     *   df17_id     - identification & category (BDS08)    metype  1-4
     *   surv        - DF4/DF5/DF20/DF21 surveillance replies
     *   other       - anything else with a good CRC
     */
    uint16_t         mt_df11;
    uint16_t         mt_df17_pos;
    uint16_t         mt_df17_vel;
    uint16_t         mt_df17_id;
    uint16_t         mt_surv;
    uint16_t         mt_other;

    /* 32-entry altitude ring for the detail-view sparkline. Updated on
     * every committed altitude change (one cell per altitude commit, not
     * per second), so the visible window scales with how chatty the
     * aircraft is. -1 = empty slot. */
    int16_t          alt_history[32];
    uint8_t          alt_history_head;

    adsb_cpr_frame_t cpr_even;
    adsb_cpr_frame_t cpr_odd;
} adsb_aircraft_t;

void                   adsb_state_init(void);
adsb_aircraft_t       *adsb_state_find_or_create(uint32_t icao);
const adsb_aircraft_t *adsb_state_get(int slot);
int                    adsb_state_active_count(void);
void                   adsb_state_age_out(int64_t now_us, int64_t timeout_us,
                                          void (*on_lost)(adsb_aircraft_t *),
                                          void (*on_late_announce)(adsb_aircraft_t *));

/* Push a new committed altitude into the per-aircraft history ring.
 * Called from the decoder whenever an altitude commit happens. */
void                   adsb_state_push_altitude(adsb_aircraft_t *a, int alt_ft);

/* Selection management for the TRACK detail page. Selection is keyed
 * by ICAO so it survives slot reuse; it falls back to the first active
 * aircraft if the previously-selected one ages out. The "next/prev"
 * helpers walk the active set in slot order. */
void                   adsb_select_set_icao(uint32_t icao);
uint32_t               adsb_select_get_icao(void);  /* 0 = no selection */
const adsb_aircraft_t *adsb_select_get(void);       /* NULL = no selection */
void                   adsb_select_next(void);
void                   adsb_select_prev(void);
void                   adsb_select_index(int *out_index, int *out_total);

#ifdef __cplusplus
}
#endif

#endif
