#ifndef REC_LIST_FORMAT_H
#define REC_LIST_FORMAT_H

/**/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Append "<name> (<size> B)" to `out` starting at *used, prefixed with
   ", " when *used is non-zero so the caller can walk the buffer with
   strstr(", ").  On success advances *used and returns true.  If the
   entry would not fit, `out` is nul-terminated at *used (dropping any
   partial write snprintf made past that point) and false is returned. */
bool rec_files_append(char *out, size_t len, size_t *used,
                      const char *name, long size);

/* Format `n` (name, size) pairs into `out` using rec_files_append and
   report both totals.  Returns the count of entries that fit in the
   buffer; sets *out_truncated to true when the buffer filled before all
   `n` entries were written.  Provided for tests and callers that already
   hold the list in memory - rec_list() itself calls rec_files_append
   directly from readdir so it never has to store the full list. */
int rec_files_format(char *out, size_t len,
                     const char *const *names, const long *sizes, int n,
                     bool *out_truncated);

/**/
/* Append "<name>  <MHz> MHz  <time>" (two spaces between fields) to `out`
   starting at *used, prefixed with ", " when *used is non-zero.  `time` may
   be NULL or empty - in that case "-" is emitted so a directory of mixed
   captures (some with sidecars, some without) still shows a single-column
   time field the AppREC walker can pass through without special-casing.

   The name is always the first whitespace-delimited token in the entry so
   the walker's `strchr(p, ' ')` still isolates it.  On refused append the
   buffer is nul-terminated at the previous *used, same invariant as the
   size-based rec_files_append. */
bool rec_files_append_row(char *out, size_t len, size_t *used,
                          const char *name, uint32_t freq_hz,
                          const char *time_iso);

#ifdef __cplusplus
}
#endif

#endif
