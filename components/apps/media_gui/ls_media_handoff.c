/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ls_media_handoff.h"

#include <stdio.h>
#include <string.h>

/*LS-753*/
/* Sized to hold /sdcard/<subdir>/<name.ext> for anything the file browser can
   reach.  A path longer than this is truncated by snprintf rather than
   overrun. */
static char s_pending[192] = {0};

void ls_media_handoff_set(const char *path)
{
    if (!path || !path[0]) {
        s_pending[0] = '\0';
        return;
    }
    snprintf(s_pending, sizeof(s_pending), "%s", path);
}

bool ls_media_handoff_take(char *out, size_t out_sz)
{
    if (!s_pending[0]) return false;
    if (out && out_sz) snprintf(out, out_sz, "%s", s_pending);
    s_pending[0] = '\0';
    return true;
}

void ls_media_handoff_reset(void)
{
    s_pending[0] = '\0';
}
