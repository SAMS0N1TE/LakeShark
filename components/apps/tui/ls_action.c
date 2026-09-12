/* See ls_action.h for why this exists and what the three load-bearing
   properties are. This file is the dispatch and nothing else: it knows about
   names, signatures and capabilities, and knows nothing about radios. The
   actions themselves are registered by the subsystems that own the hardware,
   which is what keeps this file from becoming the place every driver leaks
   into. */
#include "ls_action.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MAX_ACTIONS 48

typedef struct {
    const char *path;
    const char *sig;
    const char *help;
    ls_cap_t    needs;
    ls_act_fn   fn;
} entry_t;

static entry_t s_tab[MAX_ACTIONS];
static int     s_count;

static bool sig_ok(const char *sig)
{
    if (!sig) return false;
    int n = 0;
    for (const char *p = sig; *p; p++, n++) {
        if (n >= LS_ACT_MAX_ARGS) return false;
        if (*p != 'i' && *p != 'f' && *p != 's' && *p != 'b') return false;
    }
    return true;
}

static ls_val_kind_t kind_of(char c)
{
    switch (c) {
        case 'i': return LS_VAL_INT;
        case 'f': return LS_VAL_FLOAT;
        case 's': return LS_VAL_TEXT;
        case 'b': return LS_VAL_BOOL;
        default:  return LS_VAL_NONE;
    }
}

bool ls_action_register(const char *path, const char *sig, ls_cap_t needs,
                        ls_act_fn fn, const char *help)
{
    if (!path || !fn || !sig_ok(sig)) return false;
    if (s_count >= MAX_ACTIONS) return false;

    for (int i = 0; i < s_count; i++) {
        if (!strcmp(s_tab[i].path, path)) {
            s_tab[i] = (entry_t){ path, sig, help, needs, fn };
            return true;
        }
    }
    s_tab[s_count++] = (entry_t){ path, sig, help, needs, fn };
    return true;
}

static int find(const char *path)
{
    if (!path) return -1;
    for (int i = 0; i < s_count; i++)
        if (!strcmp(s_tab[i].path, path)) return i;
    return -1;
}

ls_act_status_t ls_action_call(const char *path, const ls_args_t *in,
                               ls_val_t *out, ls_cap_t granted)
{
    ls_val_t scratch;
    if (!out) out = &scratch;
    memset(out, 0, sizeof(*out));

    int idx = find(path);
    if (idx < 0) return LS_ACT_UNKNOWN;
    const entry_t *e = &s_tab[idx];

    /* Capability before arity on purpose: a caller that may not do this at
       all should not learn whether it got the arguments right. */
    if ((e->needs & ~(unsigned)granted) != 0) return LS_ACT_DENIED;

    int want = (int)strlen(e->sig);
    int have = in ? in->n : 0;
    if (have != want) return LS_ACT_BADARG;

    for (int i = 0; i < want; i++) {
        ls_val_kind_t k = kind_of(e->sig[i]);
        if (in->v[i].kind != k) {
            /* One widening is allowed, because a file on an SD card writes
               `1` and means 1.0 and there is no way for it to say otherwise.
               Nothing else converts: a text where an int belongs is a bug in
               the app, and reporting it is more useful than guessing. */
            if (k == LS_VAL_FLOAT && in->v[i].kind == LS_VAL_INT) continue;
            return LS_ACT_BADARG;
        }
    }
    return e->fn(in, out);
}

int         ls_action_count(void)            { return s_count; }
const char *ls_action_name(int i)  { return (i >= 0 && i < s_count) ? s_tab[i].path : NULL; }
const char *ls_action_sig(int i)   { return (i >= 0 && i < s_count) ? s_tab[i].sig  : NULL; }
const char *ls_action_help(int i)  { return (i >= 0 && i < s_count) ? s_tab[i].help : NULL; }
ls_cap_t    ls_action_needs(int i) { return (i >= 0 && i < s_count) ? s_tab[i].needs : LS_CAP_NONE; }

const char *ls_act_status_str(ls_act_status_t s)
{
    switch (s) {
        case LS_ACT_OK:          return "ok";
        case LS_ACT_UNKNOWN:     return "no such action";
        case LS_ACT_DENIED:      return "denied";
        case LS_ACT_BADARG:      return "bad argument";
        case LS_ACT_UNAVAILABLE: return "not fitted";
        case LS_ACT_BUSY:        return "busy";
        case LS_ACT_FAILED:      return "failed";
        default:                 return "?";
    }
}

const char *ls_cap_str(ls_cap_t caps)
{
    static char buf[48];
    buf[0] = 0;
    const struct { ls_cap_t bit; const char *name; } names[] = {
        { LS_CAP_READ, "read" }, { LS_CAP_TUNE, "tune" }, { LS_CAP_UI, "ui" },
        { LS_CAP_STORE, "store" }, { LS_CAP_POWER, "power" }, { LS_CAP_TX, "tx" },
    };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!(caps & names[i].bit)) continue;
        if (buf[0]) strncat(buf, "+", sizeof(buf) - strlen(buf) - 1);
        strncat(buf, names[i].name, sizeof(buf) - strlen(buf) - 1);
    }
    if (!buf[0]) strncpy(buf, "-", sizeof(buf));
    return buf;
}

ls_cap_t ls_action_grant_user(void)
{

    return (ls_cap_t)(LS_CAP_READ | LS_CAP_TUNE | LS_CAP_UI);
}

bool ls_action_parse_arg(int index, int argn, const char *text, ls_val_t *out)
{
    const char *sig = ls_action_sig(index);
    if (!sig || !text || !out) return false;
    if (argn < 0 || argn >= (int)strlen(sig)) return false;

    memset(out, 0, sizeof(*out));
    char *end;
    switch (sig[argn]) {
        case 'i': {
            long v = strtol(text, &end, 0);
            if (end == text || *end) return false;
            out->kind = LS_VAL_INT; out->i = v; return true;
        }
        case 'f': {
            float v = strtof(text, &end);
            if (end == text || *end) return false;
            out->kind = LS_VAL_FLOAT; out->f = v; return true;
        }
        case 'b': {
            bool on;
            if (!strcmp(text,"1")||!strcasecmp(text,"true")||!strcasecmp(text,"on"))       on = true;
            else if (!strcmp(text,"0")||!strcasecmp(text,"false")||!strcasecmp(text,"off")) on = false;
            else return false;
            out->kind = LS_VAL_BOOL; out->i = on; return true;
        }
        case 's':
            /* The caller owns the string. Every current caller passes either a
               console argv entry or a parsed .lsapp field, and both outlive
               the dispatch, so this does not copy. */
            out->kind = LS_VAL_TEXT; out->s = text; return true;
        default:
            return false;
    }
}
