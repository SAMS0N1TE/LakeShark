/* Named actions - the write half of the seam ls_value.h opens for reading. */

#ifndef LS_ACTION_H
#define LS_ACTION_H

#include <stdbool.h>
#include <stdint.h>
#include "ls_value.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS_ACT_MAX_ARGS 4

typedef enum {
    LS_ACT_OK = 0,
    LS_ACT_UNKNOWN,      /* no action by that name */
    LS_ACT_DENIED,       /* the caller was not granted what the action needs */
    LS_ACT_BADARG,       /* wrong count, wrong kind, or out of range */
    LS_ACT_UNAVAILABLE,  /* the hardware is absent or not brought up */
    LS_ACT_BUSY,         /* correct request, wrong moment - retry is sane */
    LS_ACT_FAILED,       /* it was tried and it did not work */
} ls_act_status_t;

/* What an action needs to touch. A grant is a bitmask; dispatch requires the
   action's needs to be a subset of it. LS_CAP_TX is deliberately never part
   of the user-app grant - see ls_action_grant_user(). */
typedef enum {
    LS_CAP_NONE  = 0,
    LS_CAP_READ  = 1u << 0,   /* sample or query; changes nothing */
    LS_CAP_TUNE  = 1u << 1,   /* change receive parameters */
    LS_CAP_UI    = 1u << 2,   /* move the interface about */
    LS_CAP_STORE = 1u << 3,   /* write NVS or the SD card */
    LS_CAP_POWER = 1u << 4,   /* rails, backlight, sleep */
    LS_CAP_TX    = 1u << 5,   /* emit RF. The broker still gates the emission */
} ls_cap_t;

typedef struct {
    int      n;
    ls_val_t v[LS_ACT_MAX_ARGS];
} ls_args_t;

/* out may be left LS_VAL_NONE by an action with nothing to report. It is
   zeroed before the handler runs, so a handler need not touch it. */
typedef ls_act_status_t (*ls_act_fn)(const ls_args_t *in, ls_val_t *out);

/* `path`, `sig` and `help` must outlive the program. False when the table is
   full or the signature is malformed - both build-time mistakes. */
bool ls_action_register(const char *path, const char *sig, ls_cap_t needs,
                        ls_act_fn fn, const char *help);

/* The single dispatch point. Checks existence, then capability, then arity
   and argument kinds, then calls. `granted` is what the CALLER may do. */
ls_act_status_t ls_action_call(const char *path, const ls_args_t *in,
                               ls_val_t *out, ls_cap_t granted);

int          ls_action_count(void);
const char  *ls_action_name(int index);
const char  *ls_action_sig(int index);
const char  *ls_action_help(int index);
ls_cap_t     ls_action_needs(int index);

const char  *ls_act_status_str(ls_act_status_t s);
const char  *ls_cap_str(ls_cap_t caps);   /* static buffer, not reentrant */

ls_cap_t ls_action_grant_user(void);

/* Parse "1234", "-3.5", "true", "hello" into the kind the signature at
   `index` expects for argument `argn`. Shared by the console and the .lsapp
   loader so the two cannot disagree about what an argument means. */
bool ls_action_parse_arg(int index, int argn, const char *text, ls_val_t *out);

/* Registers the built-in actions. Safe to call more than once. */
void ls_action_register_builtin(void);

#ifdef __cplusplus
}
#endif

#endif /* LS_ACTION_H */
