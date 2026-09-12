/* User apps: a screen described in a file on the card. */

#ifndef LS_USERAPP_H
#define LS_USERAPP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "ls_tui.h"

/* These are implementation caps, not engineering budgets. */

#define LS_UA_STR          32   /* labels and paths, including the NUL     */
#define LS_UA_MAX_ELEMS    48
#define LS_UA_MAX_APPS      8   /* how many the directory will carry       */
#define LS_UA_MAX_BYTES  8192   /* a bigger file is refused, not truncated */

typedef enum {
    LS_UA_PANEL = 0,   /* starts a panel; label is its title               */
    LS_UA_VALUE,       /* label + a value path                             */
    LS_UA_BAR,         /* label + a 0..1 path, drawn as a meter            */
    LS_UA_TEXT,        /* label only, a fixed line                         */
    LS_UA_WATERFALL,   /* the shared waterfall, in whatever room is left   */
} ls_ua_kind_t;

typedef struct {
    uint8_t kind;
    char    label[LS_UA_STR];
    char    path[LS_UA_STR];
} ls_ua_elem_t;

typedef struct {
    char    name[LS_UA_STR];
    char    sub[LS_UA_STR];
    uint8_t icon;              /* ls_icon_t                                */
    uint8_t hue;               /* TUI_*                                    */
    uint8_t n;
    ls_ua_elem_t elem[LS_UA_MAX_ELEMS];
} ls_ua_model_t;

typedef struct {
    int  line;                 /* 1-based; 0 when the whole file is at fault */
    char reason[48];
} ls_ua_err_t;

/* Parse `text` into `out`. On any error `out` is left untouched and `err`
   says which line and why - the same contract the P25 profile parser has, for
   the same reason: a half-applied layout is worse than a refused one. */
bool ls_userapp_parse(const char *text, size_t len, ls_ua_model_t *out,
                      ls_ua_err_t *err);

/* Draw a parsed model. Panels are laid out by the same ls_tui_split rule the
   built-in screens use, so a user app is portrait- and landscape-correct
   without saying anything about orientation. */
void ls_userapp_draw(const ls_ua_model_t *model, tui_surface *sf,
                     tui_rect area);

int ls_userapp_load_dir(const char *dir);

/* The most recent load failure, for the directory to show. NULL when none. */
const char *ls_userapp_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* LS_USERAPP_H */
