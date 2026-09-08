#ifndef REC_SPACE_H
#define REC_SPACE_H

/*LS-961*/
/* rec_save() and screenshot_save() used to open their target file straight away
   and pour bytes at it, so a full disk landed as a truncated `.sub` (or `.bmp`)
   that still looked like a capture - the header parsed, the row helper listed
   it and rec_load happily read whatever prefix had survived.  This module is
   the pure arithmetic that lets the callers refuse a write before it starts
   and mark an in-progress write so a crash mid-stream is recognisable after.

   POLICY WHEN THE VOLUME IS FULL: refuse.  Not rotate.  A handheld recorder
   that quietly deletes an old capture to make room for a new one would burn
   evidence the operator went out specifically to gather - the exact thing
   they will not have a chance to notice on the tab where SAVE is happening.
   If a future revision needs rotation it must be opt-in, and it must never
   delete a file that was written in the current session; today, neither
   applies because we never delete anything the user did not ask us to.

   Split out from app_rec.c so the bench can pin the estimate, the check and
   the partial-name classifier without a filesystem, a screen or a radio.  The
   filesystem-facing probe (statvfs) lives in app_rec.c because it needs the
   IDF, and passes the number this module compares against. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* In-progress captures land here first and are renamed to the final `.sub`
   once the write closes cleanly.  A leftover `<name>.sub.part` is the
   trace of an unclean write - kept on purpose so the operator can see that
   a capture failed, but never listed as a good one. */
#define REC_CAPTURE_PART_EXT ".part"

/* Pessimistic estimate of bytes rec_save() will consume writing a capture
   with `edges` edges.  Covers the .sub header, the per-edge textual width
   at its worst (`%ld` with a minus sign), wrapping cost and a fixed budget
   for the JSON sidecar.  Returned in bytes; callers compare against the
   filesystem's free-byte count with rec_space_ok(). */
uint64_t rec_space_estimate_bytes(int edges);

/* True when `available` is at least `needed`.  Both sides are compared as
   unsigned 64-bit so a 4 GB SD reported through statvfs cannot underflow
   into "insufficient". */
bool rec_space_ok(uint64_t needed, uint64_t available);

/* Format the shortage message the caller shows when a save is refused.
   Emits "need <needed> B, only <available> B free" into `out`.  Returns
   the number of characters written excluding the NUL, or 0 on bad args.
   Uses bytes rather than KB/MB so the numbers match the check exactly and
   nobody has to wonder whether "12 KB free" rounded up. */
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
