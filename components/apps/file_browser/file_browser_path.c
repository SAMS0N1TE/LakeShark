/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "file_browser_path.h"

#include <stdio.h>
#include <string.h>

/*LS-757*/
/* Before this file existed, openEntry() built the browsed path once and then
   built it AGAIN with "/sdcard/" glued on the front for the music handoff.
   A file under /sdcard/music/song.mp3 turned into /sdcard//sdcard/music/song.mp3
   and anything under /spiffs was silently redirected to SD, which failed to
   open.  The join lives here so the same rule - cwd is authoritative, no root
   is added on top of it - is enforced once and can be covered by a host test. */
int file_browser_join_path(const char *cwd, const char *name,
                           char *out, size_t out_sz)
{
    if (!name || !out || out_sz == 0) return -1;

    const char *c = cwd ? cwd : "";
    int n;
    if (c[0] == '\0') {
        n = snprintf(out, out_sz, "%s", name);
    } else {
        n = snprintf(out, out_sz, "%s/%s", c, name);
    }
    if (n < 0 || (size_t)n >= out_sz) return -1;
    return n;
}
