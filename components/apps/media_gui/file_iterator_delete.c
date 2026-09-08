/*
 * SPDX-License-Identifier: Apache-2.0
 */

/*LS-755*/
/* The managed chmorgan__esp-file-iterator component declares
   file_iterator_delete in its public header but ships no implementation, so
   any caller that tried to release an iterator hit a link error and gave up
   on freeing at all (see the old comment in AppMedia). We supply the missing
   symbol here so callers of file_iterator_new have a matching destructor.

   Mirrors file_iterator_new's malloc/strdup pattern with plain free.  Kept in
   the media_gui component - not the vendored source - so the upstream file
   stays untouched and CHECKSUMS.json still matches. */

#include <stdlib.h>

#include "file_iterator.h"

void file_iterator_delete(file_iterator_instance_t *i)
{
    if (!i) return;

    /* Tolerate partial state: file_iterator_new bails to NULL today, but a
       future caller that reuses this on a mid-scan struct must not double
       free or read past a shorter list. count is trusted only when list is
       non-NULL, which matches file_iterator_new's own ordering. */
    if (i->list) {
        for (size_t k = 0; k < i->count; ++k) {
            free(i->list[k]);
        }
        free(i->list);
    }

    /* directory_path is const char * because the header advertises it as
       read-only, but file_iterator_new obtained it from strdup() and it is
       the owner. */
    free((void *)i->directory_path);
    free(i);
}
