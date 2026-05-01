/* adsb_decode.c - magnitude, Mode-S detect, gate, CPR, dispatch to bus. */

#include "adsb_decode.h"
#include "adsb_state.h"
#include "mode-s.h"
#include "event_bus.h"
#include "perf.h"
#include "audio_events.h"
#include "plane_audio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>

static const char *TAG = "adsb_decode";

#define GATE_MIN_MSGS         3
#define GATE_WINDOW_US        (10 * 1000000LL)
#define GATE_CRC_SHAKY_NUMER  1
#define GATE_CRC_SHAKY_DENOM  2
#define LOST_TIMEOUT_US       (120 * 1000000LL)

static mode_s_t s_state;
static uint16_t *s_mag_buf     = NULL;
static int       s_mag_buf_len = 0;

static int cpr_nl(double lat)
{
    if (lat < 0) lat = -lat;
    if (lat < 10.47047130) return 59;
    if (lat < 14.82817437) return 58;
    if (lat < 18.18626357) return 57;
    if (lat < 21.02939493) return 56;
    if (lat < 23.54504487) return 55;
    if (lat < 25.82924707) return 54;
    if (lat < 27.93898710) return 53;
    if (lat < 29.91135686) return 52;
    if (lat < 31.77209708) return 51;
    if (lat < 33.53993436) return 50;
    if (lat < 35.22899598) return 49;
    if (lat < 36.85025108) return 48;
    if (lat < 38.41241892) return 47;
    if (lat < 39.92256684) return 46;
    if (lat < 41.38651832) return 45;
    if (lat < 42.80914012) return 44;
    if (lat < 44.19454951) return 43;
    if (lat < 45.54626723) return 42;
    if (lat < 46.86733252) return 41;
    if (lat < 48.16039128) return 40;
    if (lat < 49.42776439) return 39;
    if (lat < 50.67150166) return 38;
    if (lat < 51.89342469) return 37;
    if (lat < 53.09516153) return 36;
    if (lat < 54.27817472) return 35;
    if (lat < 55.44378444) return 34;
    if (lat < 56.59318756) return 33;
    if (lat < 57.72747354) return 32;
    if (lat < 58.84763776) return 31;
    if (lat < 59.95459277) return 30;
    if (lat < 61.04917774) return 29;
    if (lat < 62.13216659) return 28;
    if (lat < 63.20427479) return 27;
    if (lat < 64.26616523) return 26;
    if (lat < 65.31845310) return 25;
    if (lat < 66.36171008) return 24;
    if (lat < 67.39646774) return 23;
    if (lat < 68.42322022) return 22;
    if (lat < 69.44242631) return 21;
    if (lat < 70.45451075) return 20;
    if (lat < 71.45986473) return 19;
    if (lat < 72.45884545) return 18;
    if (lat < 73.45177442) return 17;
    if (lat < 74.43893416) return 16;
    if (lat < 75.42056257) return 15;
    if (lat < 76.39684391) return 14;
    if (lat < 77.36789461) return 13;
    if (lat < 78.33374083) return 12;
    if (lat < 79.29428225) return 11;
    if (lat < 80.24923213) return 10;
    if (lat < 81.19801349) return 9;
    if (lat < 82.13956981) return 8;
    if (lat < 83.07199445) return 7;
    if (lat < 83.99173563) return 6;
    if (lat < 84.89166191) return 5;
    if (lat < 85.75541621) return 4;
    if (lat < 86.53536998) return 3;
    if (lat < 87.00000000) return 2;
    return 1;
}

static bool cpr_decode(adsb_aircraft_t *a)
{
    if (!a->cpr_even.valid || !a->cpr_odd.valid) return false;
    int64_t dt = a->cpr_even.ts_us - a->cpr_odd.ts_us;
    if (dt < 0) dt = -dt;
    if (dt > 10000000LL) return false;

    double rlat0 = a->cpr_even.raw_lat / 131072.0;
    double rlat1 = a->cpr_odd.raw_lat  / 131072.0;
    double rlon0 = a->cpr_even.raw_lon / 131072.0;
    double rlon1 = a->cpr_odd.raw_lon  / 131072.0;

    double dlat0 = 360.0 / 60.0;
    double dlat1 = 360.0 / 59.0;
    double j     = floor(59.0 * rlat0 - 60.0 * rlat1 + 0.5);

    double lat0 = dlat0 * (fmod(j, 60.0) + rlat0);
    double lat1 = dlat1 * (fmod(j, 59.0) + rlat1);
    if (lat0 >= 270.0) lat0 -= 360.0;
    if (lat1 >= 270.0) lat1 -= 360.0;
    if (cpr_nl(lat0) != cpr_nl(lat1)) return false;

    double lat, rlon, dlon;
    int nl;
    if (a->cpr_even.ts_us >= a->cpr_odd.ts_us) {
        lat = lat0; nl = cpr_nl(lat0); rlon = rlon0;
    } else {
        lat = lat1; nl = cpr_nl(lat1); if (nl > 0) nl--; rlon = rlon1;
    }
    dlon = 360.0 / (nl > 0 ? nl : 1);

    double m   = floor(rlon0 * (cpr_nl(lat) - 1) - rlon1 * cpr_nl(lat) + 0.5);
    double lon = dlon * (fmod(m, (nl > 0 ? nl : 1)) + rlon);
    if (lon >= 180.0) lon -= 360.0;

    a->lat = (float)lat;
    a->lon = (float)lon;
    a->pos_valid = true;
    perf_mark_position(esp_timer_get_time());
    return true;
}

static void emit_contact_event(evt_kind_t kind, const adsb_aircraft_t *a, bool shaky)
{
    evt_contact_t c = {
        .icao        = a->icao,
        .altitude    = a->altitude,
        .velocity    = a->velocity,
        .heading     = a->heading,
        .lat         = a->lat,
        .lon         = a->lon,
        .pos_valid   = a->pos_valid,
        .vert_rate   = a->vert_rate,
        .crc_shaky   = shaky,
    };
    if (a->callsign[0]) {
        memcpy(c.callsign, a->callsign, sizeof(c.callsign) - 1);
        c.callsign[sizeof(c.callsign) - 1] = '\0';
    }
    event_bus_publish_contact(kind, "ADS-B", &c);
}

static void on_lost(adsb_aircraft_t *a)
{
    if (a->announced) {
        audio_events_publish(AUDIO_EVT_LOST_CONTACT, a->icao, a->callsign, false);
    }
    emit_contact_event(EVT_CONTACT_LOST, a, false);
}

static void on_late_announce(adsb_aircraft_t *a)
{
    audio_events_publish(AUDIO_EVT_NEW_CONTACT, a->icao, a->callsign, true);
    emit_contact_event(EVT_CONTACT_CONFIRMED, a, true);
}

static void on_msg(mode_s_t *self, struct mode_s_msg *mm)
{
    (void)self;

    uint32_t icao = ((uint32_t)mm->aa1 << 16) |
                    ((uint32_t)mm->aa2 <<  8) |
                     (uint32_t)mm->aa3;

    if (!mm->crcok) {
        perf_count_msg_bad();
        return;
    }
    perf_count_msg_good();
    perf_mark_good_msg(esp_timer_get_time());

    adsb_aircraft_t *a = adsb_state_find_or_create(icao);
    if (!a) return;

    int64_t now = esp_timer_get_time();
    if (a->msg_count == 0)
        emit_contact_event(EVT_CONTACT_NEW, a, false);

    a->last_seen_us = now;
    a->msg_count++;
    a->good_msg_count++;

    /* Per-aircraft message-type counters for the detail-view diagnostic.
     * The categories mirror what populates which fields, so a glance at
     * the detail page tells you why a row looks empty. */
    if (mm->msgtype == 11) {
        a->mt_df11++;
    } else if (mm->msgtype == 17) {
        if (mm->metype >= 1 && mm->metype <= 4)        a->mt_df17_id++;
        else if (mm->metype >= 9 && mm->metype <= 18)  a->mt_df17_pos++;
        else if (mm->metype >= 19 && mm->metype <= 22) a->mt_df17_vel++;
        else                                            a->mt_other++;
    } else if (mm->msgtype == 4 || mm->msgtype == 5 ||
               mm->msgtype == 20 || mm->msgtype == 21) {
        a->mt_surv++;
    } else {
        a->mt_other++;
    }

    bool had_callsign = (a->callsign[0] != '\0');
    plane_category_t prev_cat = plane_classify(a->icao,
                                               had_callsign ? a->callsign : NULL);

    /* Validate Mode-S IDENT callsigns. Garbled-but-CRC-passing flights
     * are common on weak signals — the mode-s lib's bit-flip-fix
     * occasionally rescues a message where the flipped bit was actually
     * in the callsign payload, not the CRC. Defense in two layers:
     *
     * 1. Hard structural gate (below). Real ATC callsigns always fit
     *    a tight pattern: 3-8 chars, [A-Z0-9] only (no '?', no space,
     *    no leading space), and the structure looks like a real call
     *    (has a digit, or is a known military prefix). Garbage from
     *    bit-flip-fix passes uniformly random 6-bit codes through the
     *    ais_charset table, producing '?' and weird shapes that almost
     *    never look like real callsigns.
     *
     * 2. 2-of-2 confirmation on *changes only*. Once a structurally
     *    valid callsign is committed, replacing it requires two
     *    consecutive matching decodes — otherwise a single bit-flipped
     *    message could displace a known-good callsign. First commits
     *    don't need confirmation because (a) the structural gate is
     *    tight enough that single-shot garbage almost never matches a
     *    real-callsign shape, and (b) requiring two-of-two on first
     *    commits means weak-signal aircraft never get callsigns at all
     *    if any of their early IDENT decodes have a flipped bit.
     */
    if (mm->flight[0]) {
        char cand[9];
        strncpy(cand, mm->flight, 8);
        cand[8] = '\0';
        for (int i = 7; i >= 0 && cand[i] == ' '; i--) cand[i] = '\0';

        int len = (int)strlen(cand);
        bool valid = (len >= 3 && len <= 8 && cand[0] != ' ');
        bool has_digit = false;
        bool has_letter = false;
        for (int i = 0; cand[i] && valid; i++) {
            char c = cand[i];
            if (c == '?' || c == ' ') { valid = false; break; }
            if (c >= 'A' && c <= 'Z')      has_letter = true;
            else if (c >= '0' && c <= '9') has_digit  = true;
            else                            valid     = false;
        }

        /* Shape check: a real callsign has either a digit (covers
         * commercial like UAL918, tail numbers like N53402) or is one
         * of the known all-letter military / agency prefixes (NAVY,
         * ARMY, NATO, etc. — already enumerated in plane_audio.c).
         * We don't import that list here to avoid the cross-component
         * coupling; instead we accept all-letter callsigns of length
         * 3-7 as a permissive fallback. Pure noise rarely produces a
         * meaningful all-letter string. */
        if (valid && !has_letter) valid = false;
        (void)has_digit;  /* informational; we don't reject on its absence */

        if (valid) {
            static char     pending_cand[9];
            static uint32_t pending_icao;

            if (a->callsign[0] == '\0') {
                /* First callsign for this aircraft — commit immediately
                 * once it passes the structural gate. The gate is tight
                 * enough to keep single-shot garbage out, and waiting
                 * for two-of-two costs us legitimate callsigns on weak
                 * tracks where each IDENT decode might have a different
                 * bit flipped. */
                strcpy(a->callsign, cand);
                emit_contact_event(EVT_CONTACT_IDENT, a, false);
            } else if (strcmp(a->callsign, cand) != 0) {
                /* Replacing an established callsign — require two
                 * matching decodes, since the established one was
                 * already vetted by the structural gate. */
                if (pending_icao == icao && strcmp(pending_cand, cand) == 0) {
                    strcpy(a->callsign, cand);
                    pending_icao = 0;
                    pending_cand[0] = '\0';
                    emit_contact_event(EVT_CONTACT_IDENT, a, false);
                } else {
                    strncpy(pending_cand, cand, 8);
                    pending_cand[8] = '\0';
                    pending_icao = icao;
                }
            } else {
                /* Same as committed — clear any stale pending. */
                if (pending_icao == icao) {
                    pending_icao = 0;
                    pending_cand[0] = '\0';
                }
            }
        }
    }

    /* Sanity-filter decoded fields. Mode-S CRC is 24 bits but the
     * library brute-forces single-bit-flip fixes for DF11/DF17 (and
     * two-bit fixes in aggressive mode), which means a small fraction
     * of "CRC-passing" messages have garbled non-CRC fields — the
     * flipped bit was the wrong one. Without bounds the renderer
     * latches onto values like velocity=1208130444 and vert_rate=
     * -1208163212 that flicker against the real values.
     *
     * Filter strategy:
     *   1. Hard physical bounds — anything outside is decode garbage.
     *   2. Confirmation for big jumps — a single field-update has to
     *      agree with one prior identical-direction read, so a single
     *      stray decode can't replace an established value.
     *
     * State for confirmation lives on the aircraft struct (pending_*
     * fields). pending_count is a small saturating counter — once a
     * value is confirmed (count >= 2) it commits, else it's tentative
     * and the renderer keeps the prior committed value. */

    if (mm->altitude) {
        int v = mm->altitude;
        if (v >= -1000 && v <= 60000) {
            int prev = a->altitude;
            int delta = v > prev ? v - prev : prev - v;
            if (prev == 0 || delta < 5000 || a->pending_alt == v) {
                a->altitude = v;
                a->pending_alt = 0;
                /* Record committed altitude for the detail-page
                 * sparkline. Done here rather than in the state module
                 * so it tracks committed values (post-filter) only. */
                adsb_state_push_altitude(a, v);
            } else {
                a->pending_alt = v;
            }
        }
    }
    if (mm->heading_is_valid) {
        int v = mm->heading;
        if (v >= 0 && v <= 359) {
            int prev = a->heading;
            int delta = v > prev ? v - prev : prev - v;
            if (delta > 180) delta = 360 - delta;
            if (prev == 0 || delta < 90 || a->pending_hdg == v) {
                a->heading = v;
                a->pending_hdg = 0;
            } else {
                a->pending_hdg = v;
            }
        }
    }
    if (mm->velocity) {
        int v = mm->velocity;
        if (v > 0 && v <= 1500) {
            int prev = a->velocity;
            int delta = v > prev ? v - prev : prev - v;
            if (prev == 0 || delta < 200 || a->pending_vel == v) {
                a->velocity = v;
                a->pending_vel = 0;
            } else {
                a->pending_vel = v;
            }
        }
    }
    if (mm->ew_velocity) {
        int v = mm->ew_dir ? -mm->ew_velocity : mm->ew_velocity;
        if (v >= -1500 && v <= 1500) a->ew_velocity = v;
    }
    if (mm->ns_velocity) {
        int v = mm->ns_dir ? -mm->ns_velocity : mm->ns_velocity;
        if (v >= -1500 && v <= 1500) a->ns_velocity = v;
    }
    if (mm->vert_rate) {
        int v = mm->vert_rate_sign ? -mm->vert_rate : mm->vert_rate;
        /* Spec field is 9 bits with 64 fpm resolution → ±32768 fpm
         * theoretical, but real aircraft top out near ±6000 fpm. */
        if (v >= -10000 && v <= 10000) {
            int prev = a->vert_rate;
            int delta = v > prev ? v - prev : prev - v;
            if (prev == 0 || delta < 2000 || a->pending_vs == v) {
                a->vert_rate = v;
                a->pending_vs = 0;
            } else {
                a->pending_vs = v;
            }
        }
    }

    /* Real-time gate: a contact is announced after GATE_MIN_MSGS good msgs
     * within GATE_WINDOW_US of first sight, OR after the window expires
     * with at least one message (then marked shaky). */
    if (!a->announced) {
        int64_t wall_age = now - a->first_seen_us;
        if (a->good_msg_count >= GATE_MIN_MSGS && wall_age <= GATE_WINDOW_US) {
            int total_local = perf_get_crc_good() + perf_get_crc_err();
            bool shaky = total_local > 0 &&
                (perf_get_crc_err() * GATE_CRC_SHAKY_DENOM) >
                (total_local       * GATE_CRC_SHAKY_NUMER);
            a->announced = true;
            audio_events_publish(AUDIO_EVT_NEW_CONTACT, icao, a->callsign, shaky);
            emit_contact_event(EVT_CONTACT_CONFIRMED, a, shaky);
        } else if (wall_age > GATE_WINDOW_US) {
            a->announced = true;
            audio_events_publish(AUDIO_EVT_NEW_CONTACT, icao, a->callsign, true);
        }
    } else if (mm->flight[0]) {
        /* Re-announce if a callsign just upgraded UNKNOWN to a known cat. */
        plane_category_t new_cat = plane_classify(a->icao, a->callsign);
        if (prev_cat == PLANE_UNKNOWN && new_cat != PLANE_UNKNOWN)
            audio_events_publish(AUDIO_EVT_NEW_CONTACT, icao, a->callsign, false);
    }

    if (mm->msgtype == 17 && mm->metype >= 9 && mm->metype <= 18 &&
        mm->raw_latitude != 0) {
        int64_t ts = esp_timer_get_time();
        if (mm->fflag == 0)
            a->cpr_even = (adsb_cpr_frame_t){ mm->raw_latitude, mm->raw_longitude, ts, true };
        else
            a->cpr_odd  = (adsb_cpr_frame_t){ mm->raw_latitude, mm->raw_longitude, ts, true };
        bool was_valid = a->pos_valid;
        if (cpr_decode(a) && !was_valid) {
            if (a->announced) {
                audio_events_publish(AUDIO_EVT_POSITION, icao, a->callsign, false);
                emit_contact_event(EVT_CONTACT_POSITION, a, false);
            }
        }
    }

    if (mm->msgtype == 17) {
        if (mm->metype >= 9  && mm->metype <= 18 && mm->altitude)
            emit_contact_event(EVT_CONTACT_ALTITUDE, a, false);
        else if (mm->metype >= 19 && mm->metype <= 22 && mm->velocity)
            emit_contact_event(EVT_CONTACT_VELOCITY, a, false);
    }

    perf_set_active_count(adsb_state_active_count());
}

void adsb_on_sample(uint8_t *iq, int len)
{
    if (!iq || len <= 0) return;
    int mag_len = len / 2;

    if (!s_mag_buf || s_mag_buf_len < mag_len) {
        if (s_mag_buf) { heap_caps_free(s_mag_buf); s_mag_buf = NULL; }
        s_mag_buf = heap_caps_malloc(mag_len * sizeof(uint16_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_mag_buf) s_mag_buf = malloc(mag_len * sizeof(uint16_t));
        if (!s_mag_buf) {
            ESP_LOGE(TAG, "OOM mag buffer (%d bytes)", mag_len * 2);
            return;
        }
        s_mag_buf_len = mag_len;
    }

    mode_s_compute_magnitude_vector(iq, s_mag_buf, len);

    uint32_t sum = 0, peak = 0;
    int samples = mag_len / 16;
    if (samples > 0) {
        for (int i = 0; i < mag_len; i += 16) {
            sum += s_mag_buf[i];
            if (s_mag_buf[i] > peak) peak = s_mag_buf[i];
        }
        perf_set_mag((int)(sum / samples), (int)peak);
    }

#ifdef CONFIG_ENABLE_TUI
    extern void tui_waterfall_push(const uint16_t *mag, int mag_len);
    tui_waterfall_push(s_mag_buf, mag_len);
#endif

    mode_s_detect(&s_state, s_mag_buf, mag_len, on_msg);
}

void adsb_decode_init(void)
{
    mode_s_init(&s_state);
    /* Route every preamble candidate into the perf module. The diag
     * page reads bursts/sec from there to distinguish "antenna dead"
     * (no bursts) from "bursts fail CRC" (RF noisy) from "healthy". */
    s_state.on_preamble = perf_count_burst;
    adsb_state_init();
}

void adsb_periodic_age(int64_t now_us)
{
    adsb_state_age_out(now_us, LOST_TIMEOUT_US, on_lost, on_late_announce);
    perf_set_active_count(adsb_state_active_count());
}

/* Synthetic aircraft injection — exercises the whole event pipeline
 * without needing RF or a live aircraft. Generates a plausible track
 * sample (ICAO, callsign, position, alt, vel, heading, vs) and runs
 * it through the same code paths real decodes use:
 *
 *   adsb_state_find_or_create  → table slot, render row
 *   emit_contact_event(NEW)    → first-sight event
 *   emit_contact_event(IDENT)  → callsign published
 *   emit_contact_event(CONFIRMED) → audio "new contact" + JSONL
 *   emit_contact_event(POSITION/ALTITUDE/VELOCITY) → field updates
 *
 * Each call rotates the ICAO and tweaks position/altitude/heading so
 * the consumer sees a moving track instead of a static row. After ~16
 * calls the table fills (ADSB_MAX_TRACKED) and oldest entries age out
 * normally via adsb_periodic_age. */
void adsb_inject_fake_aircraft(void)
{
    static int s_test_seq = 0;

    /* Synthetic ICAO range chosen to not collide with any real
     * registered allocation block — Mode-S codes are 24-bit and the
     * range below 0x000100 is administratively reserved / unused. */
    uint32_t icao = 0x0000A0u + (uint32_t)(s_test_seq & 0x0F);

    adsb_aircraft_t *a = adsb_state_find_or_create(icao);
    if (!a) return;

    int64_t now = esp_timer_get_time();
    bool first_msg = (a->msg_count == 0);

    if (first_msg) emit_contact_event(EVT_CONTACT_NEW, a, false);

    a->last_seen_us = now;
    a->msg_count++;
    a->good_msg_count++;

    /* Callsign: TEST<NNNN>. Modulo 10000 keeps the format inside the
     * 8-char callsign buffer (4 letters + 4 digits + NUL = 9). */
    snprintf(a->callsign, sizeof(a->callsign), "TEST%04u",
             (unsigned)(s_test_seq % 10000));

    /* Position: Laconia NH baseline, drift a bit each call so the
     * track looks alive on a map. ~0.01 deg ≈ 1 km per step. */
    a->lat = 43.5286f + 0.005f * ((float)((s_test_seq * 7) % 21) - 10.0f);
    a->lon = -71.4703f + 0.005f * ((float)((s_test_seq * 11) % 21) - 10.0f);
    a->pos_valid = true;

    a->altitude  = 5000 + ((s_test_seq * 250) % 30000);
    a->velocity  = 200 + ((s_test_seq * 13) % 200);
    a->heading   = (s_test_seq * 23) % 360;
    a->vert_rate = -1500 + ((s_test_seq * 137) % 3000);
    adsb_state_push_altitude(a, a->altitude);

    /* Push every field-update event the parser knows about so the
     * host can verify each branch of its dispatch table. */
    if (first_msg) {
        emit_contact_event(EVT_CONTACT_IDENT, a, false);
        emit_contact_event(EVT_CONTACT_CONFIRMED, a, false);
        a->announced = true;
        a->first_seen_us = now;
        audio_events_publish(AUDIO_EVT_NEW_CONTACT, icao, a->callsign, false);
    }
    emit_contact_event(EVT_CONTACT_POSITION, a, false);
    emit_contact_event(EVT_CONTACT_ALTITUDE, a, false);
    emit_contact_event(EVT_CONTACT_VELOCITY, a, false);

    /* Bump the global perf counters too — a "test" message should
     * still show up in MSGS / CRC stats so the user sees something
     * obvious flip in the TUI header line. */
    perf_count_msg_good();
    perf_set_active_count(adsb_state_active_count());

    s_test_seq++;
}
