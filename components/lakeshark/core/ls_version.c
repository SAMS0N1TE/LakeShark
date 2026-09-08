/* LS-220  Version string assembly - see ls_version.h.

   The formatter has no SDK dependency so the bench can drive it against
   fabricated inputs (including the dirty-tree case, which is hard to
   arrange in-tree).  The device wrappers at the bottom sit behind an
   #ifdef so this whole file is legal in the host bench too. */

#include "ls_version.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

bool ls_version_is_dirty(const char *version)
{
    if (!version || !*version) return false;

    /* `git describe --dirty` appends "-dirty" - but somebody might swap it
       for "-modified" or capitalise it (git config dirty), so scan the
       whole string for "dirty" case-insensitively rather than pinning to
       the suffix. */
    for (const char *p = version; *p; p++) {
        if ((p[0] == 'd' || p[0] == 'D') &&
            (p[1] == 'i' || p[1] == 'I') &&
            (p[2] == 'r' || p[2] == 'R') &&
            (p[3] == 't' || p[3] == 'T') &&
            (p[4] == 'y' || p[4] == 'Y')) {
            return true;
        }
    }
    return false;
}

static const char *or_unknown(const char *s)
{
    return (s && *s) ? s : "?";
}

int ls_version_format(char *out, size_t len, const ls_version_info_t *v)
{
    if (!out || len == 0) return 0;
    out[0] = '\0';
    if (!v) return 0;

    const char *ver  = or_unknown(v->version);
    const char *bd   = or_unknown(v->board);
    const char *idf  = or_unknown(v->idf);
    const char *date = or_unknown(v->date);
    const char *tm   = or_unknown(v->time);
    const char *dirty = ls_version_is_dirty(v->version) ? "  [DIRTY]" : "";

    int n = snprintf(out, len,
                     "LakeShark %s%s  board=%s  built %s %s  IDF %s",
                     ver, dirty, bd, date, tm, idf);
    if (n < 0) { out[0] = '\0'; return 0; }
    if ((size_t)n >= len) n = (int)len - 1;
    return n;
}

/* ------------------------------------------------------------------ device */

/* The bench compiles this file too (ls_version_format is what test_ls_version
   drives), so keep the SDK-touching wrappers behind an #ifdef.  The bench
   never calls them; the firmware always has ESP_PLATFORM defined by IDF. */
#ifdef ESP_PLATFORM

#include "esp_app_desc.h"
#include "esp_idf_version.h"

#include "ls_board.h"

void ls_version_get(ls_version_info_t *v)
{
    if (!v) return;

    const esp_app_desc_t *d = esp_app_get_description();
    v->version = d ? d->version : "?";
    v->date    = d ? d->date    : "?";
    v->time    = d ? d->time    : "?";
    v->idf     = esp_get_idf_version();
    /* LS_BOARD_NAME is the physical fact each variants header declares.
       Nothing outside board/ tests CONFIG_LS_BOARD_* - see ls_board.h. */
    v->board   = LS_BOARD_NAME;
}

int ls_version_line(char *out, size_t len)
{
    ls_version_info_t v;
    ls_version_get(&v);
    return ls_version_format(out, len, &v);
}

#endif /* ESP_PLATFORM */
