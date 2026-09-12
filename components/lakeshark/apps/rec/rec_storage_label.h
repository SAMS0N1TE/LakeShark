#ifndef REC_STORAGE_LABEL_H
#define REC_STORAGE_LABEL_H

/**/

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Short storage name for `dir`.  "SD" for anything under /sdcard, "SPIFFS"
   for anything under /spiffs, and the raw path otherwise so a third
   backend still tells the user something honest instead of a wrong label. */
const char *rec_storage_label(const char *dir);

/* Compose the FILES tab footer.  Writes "%d file[s] on <label>[ (list
   truncated)]" into out and returns the number of characters written (or
   would-be-written, per snprintf).  Kept next to the label so the caller
   cannot phrase "on SPIFFS" independently and drift out of sync. */
int rec_files_note(char *out, size_t len, const char *dir,
                   int n_files, int truncated);

#ifdef __cplusplus
}
#endif

#endif
