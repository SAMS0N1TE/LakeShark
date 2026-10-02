/* OPTIONS: the settings for what you are doing now, reached the same way in
   every app.

   An app that has settings carries one OPTIONS button on 'o', beside RADIO,
   and it opens one list titled for what is running - POCSAG OPTIONS / LR2021,
   NFM OPTIONS / RTL-SDR - with one row per setting and its value beside it.
   A row that is a choice moves to its next choice in place; one that is a
   number or words opens the keypad or the keyboard; either way the list comes
   back on the same row showing the new value, so several settings can be
   changed in a row, and BACK or ESC leaves.

   What is in the list is the screen's to say. It hands over the context it is
   in - the mode, the job and so the radio - with a const table of the
   settings that context has, kept beside the code that owns them. Rows for
   another radio than the one in use are left out, and a context left with
   nothing to set has no button at all: POCSAG shows POCSAG's settings, and a
   sweep that has none shows no OPTIONS. */

#ifndef LS_OPTIONS_H
#define LS_OPTIONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "radio_choice.h"
#include "ls_tui_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The one letter, on every screen that has the button. */
#define LS_OPT_KEY 'o'

typedef enum {
    LS_OPT_CYCLE,    /* steps through `names`, wrapping after the last      */
    LS_OPT_TOGGLE,   /* off and on: `names` holds the two words, or NULL    */
    LS_OPT_NUMBER,   /* typed on the keypad, from lo to hi                  */
    LS_OPT_TEXT,     /* typed on the keyboard, up to `max` characters       */
    LS_OPT_ACTION,   /* does something, and may open a list of its own      */
} ls_opt_kind_t;

/* Which radios a row belongs to, as bits of ls_rsel_radio_t. 0 is all. */
#define LS_OPT_RADIO(r) ((uint16_t)(1u << (r)))
#define LS_OPT_SDR      ((uint16_t)(LS_OPT_RADIO(LS_RSEL_SDR_RTL) | LS_OPT_RADIO(LS_RSEL_SDR_HACKRF)))
#define LS_OPT_LORA     LS_OPT_RADIO(LS_RSEL_LORA)

typedef struct ls_opt_s ls_opt_t;

/* One setting. Every callback is given the row, so one function can serve a
   table through `arg`. Only the fields the kind uses need filling. */
struct ls_opt_s {
    const char *label;
    ls_opt_kind_t kind;
    uint16_t radios;                 /* LS_OPT_RADIO bits; 0 for every radio  */
    int arg;                         /* the owner's own number for the row    */

    /* CYCLE and TOGGLE. get answers the index of the choice in force, or
       0/1; set is handed the next one. */
    const char *const *names;
    int n;                           /* CYCLE: how many names                 */
    int (*get)(const ls_opt_t *o);
    void (*set)(const ls_opt_t *o, int v);

    /* NUMBER. A value outside lo..hi is refused with the range said; lo == hi
       leaves the checking to set_num. `unit` is the line under the keypad's
       title. */
    double (*num)(const ls_opt_t *o);
    void (*set_num)(const ls_opt_t *o, double v);
    double lo, hi;
    const char *unit;

    /* TEXT. */
    const char *(*text)(const ls_opt_t *o);
    void (*set_text)(const ls_opt_t *o, const char *s);
    int max;

    /* ACTION. */
    void (*act)(const ls_opt_t *o);
    /* The row moves the screen somewhere else, so the list stays shut. */
    bool leaves;

    /* The value as the row shows it, when the kind's own wording will not
       do; it is what an ACTION row shows. */
    void (*show)(const ls_opt_t *o, char *out, size_t n);
    /* NULL when the row can be used now, else why not. The row stays in the
       list saying so, and choosing it changes nothing. */
    const char *(*why_not)(const ls_opt_t *o);
};

/* What the user is doing now. */
typedef struct ls_opt_ctx_s {
    const char *name;        /* "POCSAG", "NFM", "ADS-B": title and button   */
    int job;                 /* ls_rsel_job_t whose radio is in use, or -1   */
    ls_rsel_radio_t radio;   /* with no job, the radio it drives, or NONE    */
    const ls_opt_t *opt;
    int n;
    const char *tag;         /* under OPTIONS on the button; NULL: the name  */
} ls_opt_ctx_t;

/* A table's rows, for a context's initializer: LS_OPT_ROWS(OPT_NFM). */
#define LS_OPT_ROWS(a) .opt = (a), .n = (int)(sizeof(a) / sizeof((a)[0]))

/* The radio the context is using, which picks its rows: ls_rsel_effective
   of its job, the fixed radio without one, LS_RSEL_NONE for neither. */
ls_rsel_radio_t ls_opt_radio(const ls_opt_ctx_t *ctx);

/* Whether row `i` of the context's table belongs to the radio in use. */
bool ls_opt_applies(const ls_opt_ctx_t *ctx, int i);

/* How many rows the list would have now. 0, or a NULL context, is no
   button. */
int ls_opt_count(const ls_opt_ctx_t *ctx);

/* { "OPTIONS", <tag or name>, 'o' }. Draw it only when ls_opt_count says
   there is something in it. */
ls_btn_t ls_opt_button(const ls_opt_ctx_t *ctx);

/* The one list, titled "<NAME> OPTIONS / <RADIO>". Does nothing for a
   context with nothing to set. */
void ls_opt_open(const ls_opt_ctx_t *ctx);

/* 'o' or 'O' on a screen whose context has something to set: opens the list
   and answers true. Anything else, or nothing to set, answers false and the
   screen keeps the key. */
bool ls_opt_key(const ls_opt_ctx_t *ctx, char ch);

/* What row `o` shows beside its label. */
void ls_opt_value(const ls_opt_t *o, char *out, size_t n);

/* True while a list, keypad or keyboard opened from OPTIONS is up and
   OPTIONS will come back when it closes. An app whose own list reopens a
   menu of its own when it is done asks this first, so the user lands back
   in OPTIONS rather than in that menu. */
bool ls_opt_returning(void);

/* Once a frame, from the router: the values shown follow the settings while
   the list is up, and the list comes back when a keypad or list opened from
   it closes. */
void ls_opt_poll(void);

/* Drop the list and anything waiting to bring it back. */
void ls_opt_close(void);

#ifdef __cplusplus
}
#endif

#endif /* LS_OPTIONS_H */
