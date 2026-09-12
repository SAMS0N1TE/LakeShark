#include "rec_list_format.h"

#include <stdio.h>
#include <string.h>

/**/
bool rec_files_append(char *out, size_t len, size_t *used,
                      const char *name, long size)
{
    if (!out || !used || !name || len == 0 || *used >= len) return false;

    size_t avail = len - *used;
    int w = snprintf(out + *used, avail, "%s%s (%ld B)",
                     *used ? ", " : "", name, size);
    if (w < 0 || (size_t)w >= avail) {
        /* snprintf wrote a truncated prefix into the buffer; discard it
           by nul-terminating at the last complete entry so the caller
           does not see a half-formed "name (size" row. */
        out[*used] = '\0';
        return false;
    }
    *used += (size_t)w;
    return true;
}

int rec_files_format(char *out, size_t len,
                     const char *const *names, const long *sizes, int n,
                     bool *out_truncated)
{
    if (out_truncated) *out_truncated = false;
    if (!out || len == 0) return 0;
    out[0] = '\0';
    if (!names || !sizes || n <= 0) return 0;

    size_t used = 0;
    int written = 0;
    for (int i = 0; i < n; i++) {
        if (!rec_files_append(out, len, &used, names[i], sizes[i])) {
            if (out_truncated) *out_truncated = true;
            break;
        }
        written++;
    }
    return written;
}

/**/
bool rec_files_append_row(char *out, size_t len, size_t *used,
                          const char *name, uint32_t freq_hz,
                          const char *time_iso)
{
    if (!out || !used || !name || len == 0 || *used >= len) return false;

    const char *tm = (time_iso && *time_iso) ? time_iso : "-";
    unsigned long mhz_int  = (unsigned long)(freq_hz / 1000000UL);
    unsigned long mhz_frac = (unsigned long)((freq_hz % 1000000UL) / 100UL);

    size_t avail = len - *used;
    /* Field separator is two spaces so the AppREC walker's `strchr(p, ' ')`
       still finds the end of the name at the first single-space boundary,
       and the entry separator stays ", " for its strstr walker. */
    int w = snprintf(out + *used, avail, "%s%s  %lu.%04lu MHz  %s",
                     *used ? ", " : "", name, mhz_int, mhz_frac, tm);
    if (w < 0 || (size_t)w >= avail) {
        out[*used] = '\0';
        return false;
    }
    *used += (size_t)w;
    return true;
}
