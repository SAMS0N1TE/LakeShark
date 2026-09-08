#include "rec_storage_label.h"

#include <stdio.h>
#include <string.h>

/*LS-768*/
/* Prefix match against the mount-point roots rather than the exact strings
   rec_dir() returns: the SD path has "/lakeshark" appended and SPIFFS does
   not, so comparing the whole path would need to know that quirk in two
   places.  Prefix keeps the classifier agnostic to what rec_dir() picks
   under the root. */
const char *rec_storage_label(const char *dir)
{
    if (!dir || !*dir) return "?";
    if (strncmp(dir, "/sdcard", 7) == 0) return "SD";
    if (strncmp(dir, "/spiffs", 7) == 0) return "SPIFFS";
    return dir;
}

int rec_files_note(char *out, size_t len, const char *dir,
                   int n_files, int truncated)
{
    if (!out || len == 0) return 0;
    return snprintf(out, len, "%d file%s on %s%s",
                    n_files, n_files == 1 ? "" : "s",
                    rec_storage_label(dir),
                    truncated ? " (list truncated)" : "");
}
