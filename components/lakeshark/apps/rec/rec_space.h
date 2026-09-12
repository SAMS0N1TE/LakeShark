#ifndef REC_SPACE_H
#define REC_SPACE_H

/**/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REC_CAPTURE_PART_EXT ".part"

/* Pessimistic estimate of bytes rec_save() will consume writing a capture
   with `edges` edges.  Covers the .sub header, the per-edge textual width
   at its worst (`%ld` with a minus sign), wrapping cost and a fixed budget
   for the JSON sidecar.  Returned in bytes; callers compare against the
   filesystem's free-byte count with rec_space_ok(). */
uint64_t rec_space_estimate_bytes(int edges);

bool rec_space_ok(uint64_t needed, uint64_t available);

int rec_space_format_shortage(char *out, size_t len,
                              uint64_t needed, uint64_t available);

/* Format `bytes_free` as a short human-readable string: "482 B",
   "12.3 KB", "1.5 MB", "8.7 GB".  Rounded down so we never claim more
   space than the caller actually has.  Returns chars written or 0 on
   bad args.  For the REC screen and the console `rec status` row. */
int rec_space_format_free(char *out, size_t len, uint64_t bytes_free);

/* True when `name` ends with REC_CAPTURE_PART_EXT.  Callers that walk
   the capture directory (rec_list, rec_file_info, rec_load) must skip
   these - a partial file is not a capture the user can replay, and
   listing it as one would exactly recreate the bug this module exists
   to prevent. */
bool rec_capture_name_is_partial(const char *name);

#ifdef __cplusplus
}
#endif

#endif
