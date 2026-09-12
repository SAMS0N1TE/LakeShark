#ifndef REC_UNIQUE_NAME_H
#define REC_UNIQUE_NAME_H

/**/

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
