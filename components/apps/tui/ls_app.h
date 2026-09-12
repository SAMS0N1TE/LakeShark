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

typedef struct {
    const char *id;       /* stable and lower case: "p25", "fm"          */
    const char *name;     /* what the tab strip and the tile show        */
    const char *sub;      /* one short line under the name, may be NULL  */
    int         icon;     /* ls_icon_t                                   */
    uint8_t     hue;      /* TUI_* colour the tile is drawn in           */
    ls_app_cat_t cat;
    const ls_tui_screen_t *screen;

    bool (*live)(void);
} ls_app_t;

/* Registers the app and its screen together, so the two lists cannot get out
   of step. Returns the screen index, or -1. Descriptors must outlive the
   program: these are static tables, and for user apps they are entries in the
   loader's own bounded storage. */
int  ls_app_register(const ls_app_t *app);

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
