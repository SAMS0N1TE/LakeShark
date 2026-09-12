/* See ls_value.h. Read-only, cheap, bounded. */
#include "ls_value.h"

/* The registry only. Everything that knows what a P25 receiver or a
   mesh node is lives in ls_value_builtin.c, the way ls_action and
   ls_action_builtin are already split.

   That split was forced by a host test: linking this file pulled in the P25
   state, the FM state, ADS-B, perf, two radio headers, the audio path and
   MeshCore, none of which a test of the registry has any use for. A registry
   that cannot be linked without a radio is a registry nothing will test. */

#include <stdio.h>
#include <string.h>

#define MAX_VALUES 40

typedef struct {
    const char *path;
    const char *unit;
    ls_val_fn   fn;
} entry_t;

static entry_t s_val[MAX_VALUES];
static int     s_count;

bool ls_value_publish(const char *path, const char *unit, ls_val_fn fn)
{
    if (!path || !fn || s_count >= MAX_VALUES) return false;
    for (int i = 0; i < s_count; i++)
        if (strcmp(s_val[i].path, path) == 0) { s_val[i].fn = fn; return true; }
    s_val[s_count].path = path;
    s_val[s_count].unit = unit;
    s_val[s_count].fn   = fn;
    s_count++;
    return true;
}

bool ls_value_read(const char *path, ls_val_t *out, const char **unit)
{
    if (!path || !out) return false;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_val[i].path, path) != 0) continue;
        memset(out, 0, sizeof(*out));
        if (unit) *unit = s_val[i].unit;
        return s_val[i].fn(out);
    }
    return false;
}

int         ls_value_count(void)          { return s_count; }
const char *ls_value_name(int i)          { return (i >= 0 && i < s_count) ? s_val[i].path : NULL; }
const char *ls_value_unit(int i)          { return (i >= 0 && i < s_count) ? s_val[i].unit : NULL; }
