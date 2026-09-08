#ifndef REC_UNIQUE_NAME_H
#define REC_UNIQUE_NAME_H

/*LS-770*/
/* rec_save() used to build its path as "<rec_dir()>/<base>.sub" and open it
   with fopen("w"), where <base> came from the process-local s_captures
   counter as "rec%03lu".  After a reboot, a counter wrap, or the deletion of
   a newer file the same base could point at a capture that was already on
   disk, and "w" truncated it silently.  This helper picks a base whose
   ".sub" file is not present, so an automatic SAVE cannot overwrite an old
   capture without the caller opting in.  Kept as a pure classifier so the
   bench can pre-create rec000.sub and prove a subsequent save lands on a
   different name. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Highest -N suffix tried before giving up.  100 covers a full boot's worth
   of collisions without silently going past three digits of ambiguity. */
#define REC_UNIQUE_MAX_SUFFIX 100

/* Pick a name in `dir` whose "<name><ext>" file does not exist and write it
   to `out`.  If "<base><ext>" itself is free, "base" is copied verbatim.
   Otherwise "<base>-1", "<base>-2", ... "<base>-REC_UNIQUE_MAX_SUFFIX" are
   tried in order.  Returns 0 on success, -1 if every candidate is taken or
   an argument is malformed.  On failure `out` is left untouched so the
   caller cannot accidentally reuse a colliding name. */
int rec_pick_unique_name(const char *dir, const char *base, const char *ext,
                         char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif
