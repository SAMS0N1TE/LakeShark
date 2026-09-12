/* L76K GNSS receiver. */

#ifndef LS_GPS_H
#define LS_GPS_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One satellite, as GSV reports it. */

typedef struct {
    uint8_t prn;        /* satellite id, 0 for an empty slot        */
    uint8_t elevation;  /* degrees above the horizon, 0..90         */
    uint16_t azimuth;   /* degrees true, 0..359                     */
    uint8_t snr;        /* dB-Hz, 0 when seen but not tracked       */
    bool    used;       /* in the position solution, from GSA       */
} ls_gps_sat_t;

/* Enough for every constellation a consumer receiver reports at once. GPS
   alone tops out around twelve visible; with GLONASS, Galileo and BeiDou a
   clear sky reaches the high twenties. */
#define LS_GPS_MAX_SATS 32

typedef struct {
    /* Sentences are arriving and passing checksum.  True well before `fix`. */
    bool     alive;
    /* The position fields are populated and the receiver claims validity. */
    bool     fix;
    uint8_t  quality;        /* GGA field 6: 0 none, 1 GPS, 2 DGPS          */
    uint8_t  sats_used;      /* GGA field 7                                  */
    uint8_t  sats_visible;   /* satellites in view, published once a cycle   */
    /* Internal: GSV sentences accumulate here and GGA publishes the total,
       because each constellation reports its own count in its own series and
       a cycle is bounded by the GGA that opens it. Exposed only because the
       parser is pure and has nowhere else to keep it. */
    uint8_t  sats_acc;

    double   lat_deg;        /* signed, north positive                       */
    double   lon_deg;        /* signed, east positive                        */
    float    alt_m;
    float    hdop;
    float    speed_kts;
    float    course_deg;

    uint8_t  hour, minute, second;
    uint8_t  day, month;
    uint16_t year;

    /* Counters, useful for telling a wiring fault from a sky problem: bytes
       climbing with sentences flat means the baud rate is wrong, and both
       climbing with `fix` false means the antenna is the suspect. */

    ls_gps_sat_t sats[LS_GPS_MAX_SATS];
    uint8_t  sat_count;
    /* Internal: GSV fills this and the GGA that ends a cycle publishes it
       into `sats`, so a half-received sweep never reaches a drawing path. */
    ls_gps_sat_t sat_acc[LS_GPS_MAX_SATS];
    uint8_t  sat_acc_count;

    uint32_t bytes;
    uint32_t sentences;      /* passed checksum                              */
    uint32_t checksum_errors;
    int64_t  last_sentence_us;  /* esp_timer clock, 0 if never               */
} ls_gps_state_t;

/* Bring the UART up and start the reader task.  Idempotent. */
/* Is the receiver RUNNING - as distinct from talking, or knowing where it is. */

bool ls_gps_running(void);

esp_err_t ls_gps_start(void);
void      ls_gps_stop(void);

bool ls_gps_parse_line(const char *line, int len, ls_gps_state_t *st);

/* Snapshot of the current state.  Cheap; safe from any task. */
void ls_gps_get(ls_gps_state_t *out);

/* Console helper: start if needed, then describe what is arriving. */
void ls_gps_diagnostics(void);

/* Listen at each plausible rate and print what the wire carries.  For the
   case the counters cannot separate: bytes arriving that never frame. */
void ls_gps_scan_baud(void);

#ifdef __cplusplus
}
#endif

#endif /* LS_GPS_H */
