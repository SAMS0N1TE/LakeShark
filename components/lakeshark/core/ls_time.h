#ifndef LS_TIME_H
#define LS_TIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Buffer sized to hold the longest thing render_stamp emits, including the
   NUL. "up 4294967295s" is 15 chars; the ISO string "YYYY-MM-DDTHH:MM:SSZ"
   is 20. 32 leaves headroom for either form plus a tag. */
#define LS_TIME_STAMP_MAX 32

/* Is a wall clock currently set? Cheap to poll from a hot path. Meaning:
   `time(NULL)` has been set to something plausibly real (post-2024, before
   2100). Before this returns true, no caller may render a wall-clock stamp;
   after it does, the stamp is authoritative for the duration of the process
   run (an SNTP re-adjust does not flip it back off). */
bool ls_time_is_synced(void);

/* Kick off SNTP once. Safe to call more than once - the second call is a
   no-op. Must be called after netif is up (station mode connected). Does
   not block: the caller returns immediately and the module's own SNTP
   callback flips the synced flag when a server answers. */
void ls_time_sntp_start(void);

/* Stop SNTP. Called when station mode is torn down. Leaves the "was
   synced" state alone - a lost network does not un-know what the time was. */
void ls_time_sntp_stop(void);

size_t ls_time_render_stamp(char *out, size_t cap);

/* Same, but taking an explicit time source and uptime value. Extracted for
   the bench, which cannot lean on time(NULL): the pure-logic form is what
   the tests drive so the "never a plausible-looking wrong date" property
   can be pinned without host-clock flakiness. `real_time` == 0 means "no
   wall clock"; any positive value is treated as authoritative. */
size_t ls_time_render_stamp_at(char *out, size_t cap,
                               time_t real_time, int64_t uptime_us);

/* Render the same instant as a filename-safe component. The wall-clock form
   keeps ISO ordering but replaces ':' with '-', while the fallback is
   "up-<seconds>s". Both forms are safe on FAT and visibly distinct. */
size_t ls_time_render_filename_stamp(char *out, size_t cap);
size_t ls_time_render_filename_stamp_at(char *out, size_t cap,
                                        time_t real_time, int64_t uptime_us);

/* Prefix a filename-safe stamp with `label` and '_'. `label` must already be
   safe for the target filesystem. These return the basename, without an
   extension, so callers can still run collision handling before opening. */
size_t ls_time_render_filename(char *out, size_t cap, const char *label);
size_t ls_time_render_filename_at(char *out, size_t cap, const char *label,
                                  time_t real_time, int64_t uptime_us);

/* Tell this module the wall clock was set by someone other than SNTP - at present the RTC, at boot. */

void ls_time_note_set(void);

/* Test-only. Force the sync flag on or off. Not for firmware callers. */
void ls_time_test_set_synced(bool synced);

#ifdef __cplusplus
}
#endif

#endif
