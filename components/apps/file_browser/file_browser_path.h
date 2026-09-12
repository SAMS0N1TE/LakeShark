/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**/
/* Join the browser's current working directory with an entry name to form the
   absolute path the user actually browsed.  `cwd` is either "" (the roots
   view) or already carries its mount point ("/sdcard/..." or "/spiffs/...").
   The output never has a "//" between components and never rewrites /spiffs
   as /sdcard.

   Returns the number of bytes written (excluding the terminator) on success,
   or a negative value on bad arguments or truncation. */
int file_browser_join_path(const char *cwd, const char *name,
                           char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif
