/* L76K GNSS receiver. */

#ifndef LS_GPS_H
#define LS_GPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The constellation a satellite belongs to. The values are NMEA 4.1's GSA
   system IDs, so a GSA that carries one is stored as it stands. */
typedef enum {
    LS_GPS_SYS_UNKNOWN = 0,   /* a GN talker, or one not listed here */
    LS_GPS_SYS_GPS     = 1,
    LS_GPS_SYS_GLONASS = 2,
    LS_GPS_SYS_GALILEO = 3,
    LS_GPS_SYS_BEIDOU  = 4,
    LS_GPS_SYS_QZSS    = 5,
    LS_GPS_SYS_COUNT
} ls_gps_sys_t;

/* What the receiver's TXT sentences say about its antenna feed. */
typedef enum {
    LS_GPS_ANT_UNKNOWN = 0,   /* not reported */
    LS_GPS_ANT_OK,
    LS_GPS_ANT_OPEN,
    LS_GPS_ANT_SHORT,
} ls_gps_antenna_t;

/* One satellite, as GSV reports it. */

typedef struct {
    uint8_t prn;        /* satellite id, 0 for an empty slot        */
    uint8_t elevation;  /* degrees above the horizon, 0..90         */
    uint16_t azimuth;   /* degrees true, 0..359                     */
    uint8_t snr;        /* dB-Hz, 0 when seen but not tracked       */
    /* One byte for the pair: every ls_gps_get snapshot carries a table of
       these, most of them on a task's stack, and the parser's context two. */
    bool    used : 1;   /* in the position solution, from GSA       */
    uint8_t sys  : 7;   /* ls_gps_sys_t, from the GSV talker        */
} ls_gps_sat_t;

/* Enough for every constellation a consumer receiver reports at once. GPS
   alone tops out around twelve visible; GPS, GLONASS and BeiDou together
   pass thirty in a clear sky, and Galileo adds more. */
#define LS_GPS_MAX_SATS 48

typedef struct {
    /* Sentences are arriving and passing checksum.  True well before `fix`.
       Cleared when the reader starts. */
    bool     alive;
    /* The position fields are populated and the receiver claims validity. */
    bool     fix;
    uint8_t  quality;        /* GGA field 6: 0 none, 1 GPS, 2 DGPS          */
    uint8_t  sats_used;      /* GGA field 7                                  */
    uint8_t  sats_visible;   /* satellites in view, published once a cycle   */
    /* In view with a C/N0, which is what the receiver is actually tracking:
       the in-view count also holds satellites the almanac only predicts.
       Published with the table. */
    uint8_t  sats_tracked;
    uint8_t  tracked_by_sys[LS_GPS_SYS_COUNT];
    uint8_t  cn0_best;       /* dB-Hz, the strongest tracked satellite       */
    uint8_t  cn0_top4;       /* dB-Hz, mean of the four strongest            */
    uint8_t  antenna;        /* ls_gps_antenna_t, the latest TXT report      */

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

    uint32_t bytes;
    uint32_t sentences;      /* passed checksum                              */
    uint32_t checksum_errors;
    int64_t  last_sentence_us;  /* esp_timer clock, 0 if never               */
    int64_t  last_fix_us;
    int64_t  start_us;          /* when the reader last started              */
    int64_t  first_fix_us;      /* first position since then, 0 until one    */
    uint32_t position_updates;
} ls_gps_state_t;

/* The parser's context: the published state and the working tables behind it.
   ls_gps_get copies only `st`, so a snapshot on a task's stack does not carry
   the tables, which only the parser reads. */
typedef struct {
    ls_gps_state_t st;   /* what ls_gps_get publishes */

    /* Internal: GSV sentences accumulate here and GGA publishes the total,
       because each constellation reports its own count in its own series and
       a cycle is bounded by the GGA that opens it. Exposed only because the
       parser is pure and has nowhere else to keep it. */
    uint8_t  sats_acc;

    /* Internal: GSV fills this and the GGA that ends a cycle publishes it
       into `st.sats`, so a half-received sweep never reaches a drawing path. */
    ls_gps_sat_t sat_acc[LS_GPS_MAX_SATS];
    uint8_t  sat_acc_count;

    /* Internal: satellites a GSA has marked used this cycle, as constellation
       and PRN, kept independently of sat_acc because a GSA can name a
       satellite before the GSV that first creates its entry. Bounded and
       deduplicated; cleared when the GGA that ends the cycle publishes
       sat_acc into st.sats. */
    uint8_t  gsa_used_prns[LS_GPS_MAX_SATS];
    uint8_t  gsa_used_sys[LS_GPS_MAX_SATS];
    uint8_t  gsa_used_count;
} ls_gps_parser_t;

/* Bring the UART up and start the reader task.  Idempotent. */
/* Is the receiver RUNNING - as distinct from talking, or knowing where it is. */

bool ls_gps_running(void);

esp_err_t ls_gps_start(void);
void      ls_gps_stop(void);

bool ls_gps_parse_line(const char *line, int len, ls_gps_parser_t *p);

/* A framed line as the reader hands it over at now_us: parsed, and counted
   into the sentence, checksum and fix clocks. False when it fails its
   checksum. */
bool ls_gps_feed_line(const char *line, int len, ls_gps_parser_t *p,
                      int64_t now_us);

/* "GPS", "GLONASS", "Galileo", "BeiDou", "QZSS", or "?". */
const char *ls_gps_sys_name(uint8_t sys);
/* "OK", "OPEN", "SHORT", or "not reported". */
const char *ls_gps_antenna_name(uint8_t antenna);

/* "tracked 9 (GPS 5, BeiDou 3, GLONASS 1), C/N0 best 31, top-4 mean 27
   dB-Hz, antenna OPEN, first fix after 41 s". Returns the length snprintf
   would have written. */
int ls_gps_describe_sky(char *out, size_t cap, const ls_gps_state_t *g,
                        int64_t now_us);

/* ---- bringing the module up ---------------------------------------------

   The L76K keeps its baud rate in RAM its backup cell holds up, so a board
   whose cell has drained comes back at the datasheet's 9600 and the reader,
   at 115200, hears nothing. Its constellation set is not kept across a power
   cycle at all. The reader walks these states once per start; each step is
   decided by ls_gps_link_step from whether a sentence has framed since the
   current state began and how long ago that was. */

typedef enum {
    LS_GPS_LINK_PROBE = 0,  /* at 115200, waiting for a first sentence      */
    LS_GPS_LINK_TRY_9600,   /* nothing framed there; listening at 9600       */
    LS_GPS_LINK_CONFIRM,    /* told the module to go to 115200; listening    */
    LS_GPS_LINK_UP,         /* sentences framing at 115200                   */
    LS_GPS_LINK_AT_9600,    /* the module stayed at 9600; reading it there   */
    LS_GPS_LINK_SILENT,     /* nothing at either rate; listening at 115200   */
} ls_gps_link_t;

typedef enum {
    LS_GPS_DO_NOTHING = 0,
    LS_GPS_DO_LISTEN_9600,  /* UART to 9600                                  */
    LS_GPS_DO_RAISE,        /* $PCAS01,5 twice, then the UART to 115200      */
    LS_GPS_DO_SAVE,         /* $PCAS00, so 115200 outlives a drained cell    */
    LS_GPS_DO_CONFIGURE,    /* $PCAS04,7 then $PCAS02,1000                   */
    LS_GPS_DO_STAY_9600,    /* UART back to 9600: the module did not move    */
    LS_GPS_DO_GIVE_UP,      /* UART back to 115200, and keep listening there */
} ls_gps_do_t;

#define LS_GPS_PROBE_US      2000000   /* at 115200 before trying 9600     */
#define LS_GPS_TRY_9600_US   1500000   /* at 9600 before giving up         */
#define LS_GPS_CONFIRM_US    2000000   /* at 115200 after the switch       */

/* `configured` is whether the constellations and rate were already sent this
   boot; they are sent once, from the first state that frames at 115200. */
ls_gps_do_t ls_gps_link_step(ls_gps_link_t *link, bool framed,
                             int64_t waited_us, bool configured);

/* The CASIC commands the reader sends. */
#define LS_GPS_CMD_GNSS      "$PCAS04,7*1E\r\n"     /* GPS+BeiDou+GLONASS */
#define LS_GPS_CMD_RATE_1HZ  "$PCAS02,1000*2E\r\n"  /* one fix a second   */
#define LS_GPS_CMD_115200    "$PCAS01,5*19\r\n"
#define LS_GPS_CMD_SAVE      "$PCAS00*01\r\n"

/* Snapshot of the current state.  Cheap; safe from any task. */
void ls_gps_get(ls_gps_state_t *out);

/* Console helper: start if needed, then describe what is arriving. */
void ls_gps_diagnostics(void);

/* Console helper: each satellite in the last published table. */
void ls_gps_print_sky(void);

/* Console helper: the last lines framed off the wire, as received. */
void ls_gps_print_raw(void);

/* Listen at each plausible rate and print what the wire carries.  For the
   case the counters cannot separate: bytes arriving that never frame. */
void ls_gps_scan_baud(void);

#ifdef __cplusplus
}
#endif

#endif /* LS_GPS_H */
