#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**/
/* Files-to-Music handoff.  The file browser stashes a path here before it
   launches MUSIC, and AppMedia consumes it in run() AND resume() - the old
   code only consumed on run(), so after the shell had built Music once, every
   later file pick landed on resume() with the path still sitting here and
   nothing playing it.  take() is one-shot so a later manual open of Music
   does not re-trigger a stale handoff. */
void ls_media_handoff_set(const char *path);
bool ls_media_handoff_take(char *out, size_t out_sz);
void ls_media_handoff_reset(void);

#ifdef __cplusplus
}
#endif
