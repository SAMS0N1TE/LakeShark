/* The app directory. */

#ifndef LS_APP_H
#define LS_APP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "ls_tui_screen.h"

typedef enum {
    LS_APP_MAIN = 0,
    LS_APP_EXTRA,
    LS_APP_USER,
} ls_app_cat_t;

/* ------------------------------------------------- the app contract ---
 * Three questions every app answers at the point it is declared, because
 * answering them later means not answering them.
 *
 * What is this for, what does it keep, and does it care where it is.
 *
 * Before this, an app's whole self-description was one line of key hints and
 * a one-word tagline, the [?] overlay showed the same nine global keys in
 * every app, and the journal was reachable only from LORA LABS. An operator
 * could not find out what an app recorded without reading its source, and
 * nothing recorded a position unless it happened to go through ls_field.
 *
 * ls_app_register() refuses a descriptor without a doc, and
 * test_app_contract walks every registered app and fails the build if any of
 * the required answers is missing. That is what keeps this integrated at the
 * start rather than a field people forget. */

/* What an app puts in the journal. */
typedef enum {
    /* A viewer. Settings, diagnostics, the map: nothing observed, nothing
       worth keeping. Saying so is an answer, not an omission. */
    LS_APP_RECORDS_NOTHING = 0,
    /* The operator decides. A key or a button keeps the current observation. */
    LS_APP_RECORDS_MANUAL,
    /* It keeps observations as they arrive, without being asked. */
    LS_APP_RECORDS_AUTOMATIC,
} ls_app_records_t;

/* What an app does with position. Journal entries carry a fix already -
   ls_field_sample_t has gps_valid, lat, lon, alt_m and heading - so STAMPS
   costs an app nothing once it records at all. NAVIGATES is the stronger
   claim: position changes what the app does, not just what it wrote down. */
typedef enum {
    LS_APP_GPS_UNUSED = 0,
    LS_APP_GPS_STAMPS,
    LS_APP_GPS_NAVIGATES,
} ls_app_gps_t;

/* Keys are deliberately NOT here. Every screen already carries a .hint line
   that is drawn at the bottom of the glass, and the [?] overlay reads that
   same string. A second list would be a second thing to keep true. */
typedef struct {
    /* One or two sentences saying what the app is for, in the operator's
       terms and not the implementation's. Shown at the top of [?]. */
    const char *purpose;

    ls_app_records_t records;
    /* What one kept entry contains. Required unless RECORDS_NOTHING: an app
       that keeps something the operator cannot describe is keeping it for
       nobody. */
    const char *record_note;

    ls_app_gps_t gps;
    /* What position is used for. Required unless GPS_UNUSED. */
    const char *gps_note;
} ls_app_doc_t;

typedef struct {
    const char *id;       /* stable and lower case: "p25", "fm"          */
    const char *name;     /* what the tab strip and the tile show        */
    const char *sub;      /* one short line under the name, may be NULL  */
    int         icon;     /* ls_icon_t                                   */
    uint8_t     hue;      /* TUI_* colour the tile is drawn in           */
    ls_app_cat_t cat;
    const ls_tui_screen_t *screen;

    bool (*live)(void);

    /* Required. See ls_app_doc_t: registration fails without it. */
    const ls_app_doc_t *doc;
} ls_app_t;

/* Registers the app and its screen together, so the two lists cannot get out
   of step. Returns the screen index, or one of the LS_APP_REG_* codes below.
   Descriptors must outlive the program: these are static tables, and for user
   apps they are entries in the loader's own bounded storage. */
int  ls_app_register(const ls_app_t *app);

/* Why a registration was refused. All negative, so `< 0` still means
   refused. */
#define LS_APP_REG_BAD_ARGS   (-1)  /* no descriptor, or no screen          */
#define LS_APP_REG_NO_DOC     (-2)  /* doc missing or incomplete            */
#define LS_APP_REG_DIR_FULL   (-3)  /* the app directory is full            */
#define LS_APP_REG_NO_SCREEN  (-4)  /* the screen table is full             */

/* A short phrase for `rc`, to append after "name: ". Never NULL. */
const char *ls_app_register_why(int rc);

int  ls_app_count(void);
const ls_app_t *ls_app_at(int index);
const ls_app_t *ls_app_by_id(const char *id);

/* Directory order: MAIN first in registration order, then EXTRA, then USER.
   `out` receives indices into ls_app_at(). Returns how many were written. */
int  ls_app_list(ls_app_cat_t cat, const ls_app_t **out, int cap);

/* The app whose screen is showing, or NULL. */
const ls_app_t *ls_app_current(void);

/* Open an app by index into ls_app_at(), playing its opening animation. */
void ls_app_open(int index);

#ifdef __cplusplus
}
#endif

#endif /* LS_APP_H */
