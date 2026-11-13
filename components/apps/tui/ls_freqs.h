/* FREQS: the frequency library every app reads from and saves into.

   A bank is a CSV file on the card, /sdcard/freqs/<name>.csv, one frequency a
   line, written to be edited by hand:

       freq_mhz,mode,bw_khz,name,group,tags,lat,lon,radius_km,tone,notes

   A blank lat/lon means everywhere. Lines starting with '#' are comments, and
   a line that cannot be read is skipped and counted, never the whole file.
   mine.csv is where SAVE puts things, from FREQS or from any other app.

   This file is the model and the card: parsing, the importers (.lscan, CHIRP,
   SDR++, gqrx), the filters, and which app a mode is tuned in. None of it
   touches a radio, so all of it runs on the bench. ls_freqs_run.c is the part
   that does: TUNE, the freq.save action and the console. */
#ifndef LS_FREQS_H
#define LS_FREQS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ls_action.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS_FREQ_NAME   25   /* 24 characters, the dataset's own limit */
#define LS_FREQ_GROUP  20   /* "PMR/LICENSE-FREE" and room to spare */
#define LS_FREQ_TAGS   32
#define LS_FREQ_TONE    8
/* Notes are kept to what a screen line shows. A longer note stays whole in
   its file: an edit rewrites only the fields that changed. */
#define LS_FREQ_NOTES  56
/* What one view holds, in PSRAM (164 bytes an entry). A bank past it is
   loaded to here and says so. */
#define LS_FREQ_MAX  2000
#define LS_FREQ_BANKS  16
#define LS_FREQ_NOWHERE INT32_MIN

/* The modes a line may name. LTE is CELL WATCH's, which saves too. */
typedef enum {
    LS_FM_NFM, LS_FM_WFM, LS_FM_AM, LS_FM_USB, LS_FM_LSB, LS_FM_CW,
    LS_FM_P25, LS_FM_DMR, LS_FM_NXDN, LS_FM_TETRA, LS_FM_DSTAR, LS_FM_YSF,
    LS_FM_FSK, LS_FM_OOK, LS_FM_LORA, LS_FM_ADSB, LS_FM_AIS, LS_FM_APRS,
    LS_FM_POCSAG, LS_FM_ACARS, LS_FM_VDL2, LS_FM_APT, LS_FM_LRPT, LS_FM_GPS,
    LS_FM_IRIDIUM, LS_FM_LTE, LS_FM__COUNT
} ls_fmode_t;

typedef struct {
    uint32_t hz;
    int32_t  lat_e5, lon_e5;     /* LS_FREQ_NOWHERE: everywhere          */
    uint16_t bw;                 /* hundreds of Hz, 0 when not given     */
    uint16_t radius_km;          /* 0 unless placed                      */
    uint16_t line;               /* its line in the bank file, 0: none   */
    uint8_t  mode;               /* ls_fmode_t                           */
    uint8_t  bank;               /* index into the bank list             */
    char name[LS_FREQ_NAME];
    char group[LS_FREQ_GROUP];
    char tags[LS_FREQ_TAGS];
    char tone[LS_FREQ_TONE];
    char notes[LS_FREQ_NOTES];
} ls_freq_t;

const char *ls_freq_mode_name(int mode);
/* Names and the spellings other tools use for them ("FM", "C4FM", "ADS-B").
   -1 for none. */
int  ls_freq_mode_parse(const char *text);
/* "121.500", "446.00625": the fewest decimals that say it, at least three. */
void ls_freq_mhz_text(uint32_t hz, char *out, size_t n);

/* ---------------------------------------------------------------- lines -- */

/* Reading one bank. A header line names the columns (any order, extras
   ignored); without one they are taken in the order above. */
typedef struct { int8_t col[11]; bool seen; } ls_freq_csv_t;
void ls_freq_csv_begin(ls_freq_csv_t *c);
/* 1: *out is an entry. 0: nothing there (blank, comment, header).
   -1: a line that says something and is not a frequency - counted, skipped.
   `line` is cut up in place. */
int  ls_freq_csv_line(ls_freq_csv_t *c, char *line, ls_freq_t *out);
/* The entry as one line of the format, quoted where it has to be. Answers
   the length, or 0 when it did not fit. */
int  ls_freq_csv_format(const ls_freq_t *e, char *out, size_t n);
/* The same in the columns a file's own header put them in, so a line added
   to a bank written by hand lands under the right names. */
int  ls_freq_csv_format_as(const ls_freq_csv_t *c, const ls_freq_t *e, char *out, size_t n);
extern const char LS_FREQ_CSV_HEADER[];

/* What a file loaded as: lines taken, lines refused, the first refused. */
typedef struct { int loaded, skipped, first_bad; bool truncated; } ls_freq_tally_t;
typedef bool (*ls_freq_sink_t)(const ls_freq_t *e, void *ctx);

/* The formats an import understands, told apart by what is in them. */
typedef enum { LS_FI_CSV, LS_FI_LSCAN, LS_FI_CHIRP, LS_FI_GQRX, LS_FI_SDRPP } ls_freq_kind_t;
ls_freq_kind_t ls_freq_kind_of(const char *first_line);
const char *ls_freq_kind_name(ls_freq_kind_t k);
/* Every entry in `f`, in whatever format it turns out to be, handed to
   `sink` (which answers false to stop). `group` fills the group of formats
   that have none. False only when the file could not be read at all. */
bool ls_freq_read(FILE *f, const char *group, ls_freq_sink_t sink, void *ctx,
                  ls_freq_tally_t *t, ls_freq_kind_t *kind);
/* The scanner's line for an NFM or P25 entry, checked by the scanner's own
   parser; false for an entry the scanner cannot take. */
bool ls_freq_lscan_format(const ls_freq_t *e, char *out, size_t n);

/* --------------------------------------------------------------- views -- */

enum { LS_FB_ALL, LS_FB_HF, LS_FB_VHF, LS_FB_UHF, LS_FB_900, LS_FB_L, LS_FB__COUNT };
enum { LS_FS_LISTED, LS_FS_FREQ, LS_FS_NAME, LS_FS_NEAR, LS_FS__COUNT };
const char *ls_freq_band_name(int band);
const char *ls_freq_sort_name(int sort);
bool ls_freq_in_band(int band, uint32_t hz);

typedef struct {
    uint8_t band, sort;
    bool near;                    /* placed entries within reach of the fix */
    char group[LS_FREQ_GROUP];    /* "" for every group */
    char find[32];                /* name, tags, notes, group or the MHz */
} ls_freq_filter_t;

/* Great circle, and the eight-point bearing from (lat0, lon0) to the entry. */
double ls_freq_km(double lat0, double lon0, double lat1, double lon1);
const char *ls_freq_bearing(double lat0, double lon0, double lat1, double lon1);
/* Distance to a placed entry, or a negative number for an unplaced one. */
float ls_freq_distance(const ls_freq_t *e, double lat, double lon);
bool ls_freq_matches(const ls_freq_t *e, const ls_freq_filter_t *f, bool fix,
                     double lat, double lon);

/* A group's three-letter tag and colour on the list. */
const char *ls_freq_group_tag(const char *group);
uint8_t ls_freq_group_hue(const char *group);

/* Where TUNE takes an entry of a mode. `arg` is FM's submode or the HF
   demodulator; `note` says what that app will and will not do with it. */
typedef enum {
    LS_FR_FM, LS_FR_HF, LS_FR_P25, LS_FR_SUBGHZ, LS_FR_LABS, LS_FR_OPEN, LS_FR_NONE
} ls_freq_target_t;
typedef struct {
    uint8_t target;
    const char *app, *arg, *note;
} ls_freq_route_t;
ls_freq_route_t ls_freq_route(int mode, uint32_t hz);

/* ---------------------------------------------------------------- store -- */

/* The scan, the load and its pump, and the writes may be called from any
   task: the console and freq.save run beside the UI. What is loaded
   (ls_freqs_at, the view) is the UI task's to read. */

/* The set every unit has with no card: CSV text in the bank format. */
const char *ls_freqs_builtin_text(void);

/* Where the banks live; /sdcard/freqs unless the bench says otherwise. */
void ls_freqs_set_root(const char *dir);
const char *ls_freqs_root(void);
/* The store's lock; ls_freqs_register makes it at boot. */
void ls_freqs_lock_init(void);
/* The bank files on the card, sorted; false without a card. */
bool ls_freqs_scan(void);
int  ls_freqs_banks(void);
const char *ls_freqs_bank(int i);        /* "argentina", without .csv */
int  ls_freqs_bank_find(const char *name);

/* What is loaded: one bank, every bank, or the set built in. Loading is a
   walk over the files a few dozen lines at a time, so the screen keeps
   drawing (and the header's needle sweeps) while a big bank comes in. */
#define LS_FREQS_ALL     (-1)
#define LS_FREQS_BUILTIN (-2)
#define LS_FREQ_BANK_BUILTIN 0xFE
void ls_freqs_load(int which);
/* Read up to `lines` more; true while there is more to read. */
bool ls_freqs_pump(int lines);
bool ls_freqs_loading(void);
int  ls_freqs_which(void);
/* Bumps every time the loaded set changes, for views to rebuild. */
uint32_t ls_freqs_generation(void);
int  ls_freqs_count(void);
const ls_freq_t *ls_freqs_at(int i);
const ls_freq_tally_t *ls_freqs_tally(void);
/* Ask for a reload the next time anyone pumps: a save from another app. */
void ls_freqs_dirty(void);
bool ls_freqs_is_dirty(void);

/* The indices of the entries a filter keeps, sorted; answers how many. */
int  ls_freqs_view(const ls_freq_filter_t *f, bool fix, double lat, double lon,
                   uint16_t *out, int cap);

/* Writing. `why` gets a sentence either way. */
bool ls_freqs_append(const char *bank, const ls_freq_t *e, char *why, size_t n);
/* Replace (or with NULL, delete) loaded entry `i` in its file, every other
   line - comments included - left exactly as it was. */
bool ls_freqs_rewrite(int i, const ls_freq_t *with, char *why, size_t n);
/* Writing a list out, entry by entry: CSV, or the scanner's .lscan, which
   takes NFM and P25 only and refuses the same channel twice. */
typedef struct { FILE *f; bool lscan; int wrote, left, seen; } ls_freq_out_t;
bool ls_freq_out_open(ls_freq_out_t *o, const char *path, bool lscan);
void ls_freq_out_put(ls_freq_out_t *o, const ls_freq_t *e);
bool ls_freq_out_close(ls_freq_out_t *o, const char *path, char *why, size_t n);
/* `idx` entries of what is loaded to `path`, as CSV or as a scanner list. */
bool ls_freqs_export(const uint16_t *idx, int n, const char *path, bool lscan,
                     char *why, size_t wn);
/* A file in any format into a new bank beside the others. */
bool ls_freqs_import(const char *path, char *why, size_t n);
/* The bank the last import made, "" when it made none. */
const char *ls_freqs_imported(void);

/* ------------------------------------------------- saving from any app -- */

/* SAVE from anywhere goes through the action table, so an app needs no link
   to FREQS and a build without it answers "no such action" instead of not
   building. Mode is a name ("NFM", "OOK"); `name` may be NULL. The sentence
   to show comes back in `msg`. */
static inline bool ls_freqs_offer(uint32_t hz, const char *mode, const char *name,
                                  const char *app, char *msg, size_t n)
{
    char mhz[16];
    snprintf(mhz, sizeof(mhz), "%lu.%06lu", (unsigned long)(hz / 1000000u),
             (unsigned long)(hz % 1000000u));
    ls_args_t a = { .n = 4 };
    const char *v[4] = { mhz, mode ? mode : "NFM", name ? name : "", app ? app : "" };
    for (int i = 0; i < 4; i++) { a.v[i].kind = LS_VAL_TEXT; a.v[i].s = v[i]; }
    ls_val_t out = { LS_VAL_NONE };   /* left as it is when the call goes no further */
    const ls_act_status_t st = ls_action_call("freq.save", &a, &out, LS_CAP_STORE);
    if (msg && n) snprintf(msg, n, "%s", out.kind == LS_VAL_TEXT && out.s ? out.s
                           : st == LS_ACT_UNKNOWN ? "FREQS is not in this build" : ls_act_status_str(st));
    return st == LS_ACT_OK;
}

/* ls_freqs_run.c: the actions, the console and TUNE. */
void ls_freqs_register(void);
void ls_freqs_console_register(void);
int  ls_freqs_command(int argc, char **argv);
/* Tune entry `e` in the app its mode belongs to; the sentence in `msg`. */
bool ls_freqs_tune(const ls_freq_t *e, char *msg, size_t n);
/* Keep `e` in mine.csv and say so in a banner. */
bool ls_freqs_keep(const ls_freq_t *e, char *msg, size_t n);
/* Once a frame on the UI task: console requests. */
void ls_freqs_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* LS_FREQS_H */
