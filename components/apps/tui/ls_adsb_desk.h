/* ADS-B on the desk: the aircraft table's rows, in any order. The screen
   fills a row from the tracker for each aircraft it holds; this sorts them
   by a column, says what a cell reads, matches a row against typed text and
   writes one as a CSV line. No drawing and no tracker in here, so the bench
   checks it with rows of its own. */
#ifndef LS_ADSB_DESK_H
#define LS_ADSB_DESK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LS_AC_ALARM, LS_AC_CALL, LS_AC_ICAO, LS_AC_ALT, LS_AC_VS, LS_AC_SPD, LS_AC_HDG, LS_AC_DIST, LS_AC_BRG,
    LS_AC_SQUAWK, LS_AC_SRC, LS_AC_AGE, LS_AC_MSGS, LS_AC__COUNT
};

/* A field that is known, and one that has not been heard for a while. */
enum { LS_AF_ALT = 1, LS_AF_SPD = 2, LS_AF_HDG = 4, LS_AF_VS = 8, LS_AF_POS = 16 };

typedef struct {
    uint32_t key;               /* the tracker's own name for it (address and qualifier) */
    uint32_t icao;
    int8_t slot;
    uint8_t have, stale;        /* LS_AF_* */
    char call[9];
    char src[6];                /* "1090", "UAT", "BOTH", a * when stale */
    int32_t alt, vs;            /* ft, ft/min */
    int16_t spd, hdg;           /* kt, degrees */
    float nm, brg;              /* from here; negative when it or we have no position */
    float lat, lon;
    int16_t squawk;             /* 0: none heard */
    const char *alarm;          /* "EMERGENCY", "MEDICAL"...; NULL for none */
    int32_t age_s;              /* since anything was heard from it */
    uint32_t msgs;
} ls_adsbd_row_t;

/* order[0..n) becomes the rows' indices sorted by `col`, ascending unless
   `desc`. A row with nothing in that column goes last either way. With
   `alarms_first` an aircraft declaring an emergency is above the rest
   whatever the column. Sorting by LS_AC_ALARM puts emergencies first and
   leaves the rest as the tracker holds them. */
void ls_adsbd_sort(uint8_t *order, const ls_adsbd_row_t *rows, int n, int col, bool desc, bool alarms_first);
/* What a cell reads; a * after a value not heard for a while, "" when unknown. */
void ls_adsbd_cell(const ls_adsbd_row_t *r, int col, char *out, size_t n);
/* Case-blind: the callsign, the address in hex, the squawk or the alarm
   word holds `needle`. An empty needle matches everything. */
bool ls_adsbd_match(const ls_adsbd_row_t *r, const char *needle);
/* The CSV's first line, and a row as one line of it (no newline). */
extern const char LS_ADSBD_CSV_HEAD[];
int ls_adsbd_csv(const ls_adsbd_row_t *r, char *out, size_t n);
/* A climb, level or a descent, from ft/min: '^', '=', 'v'; ' ' unknown. */
char ls_adsbd_trend(const ls_adsbd_row_t *r);

#ifdef __cplusplus
}
#endif

#endif /* LS_ADSB_DESK_H */
