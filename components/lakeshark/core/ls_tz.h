/* ls_tz: the board's time zone, and the clock text every screen shows.

   SET > SYSTEM > Time zone picks AUTO (the default: from where the GPS last
   had a fix, or HOME), UTC, or a named zone. Each named zone is a POSIX TZ
   string with its daylight-saving rule; the firmware applies it with
   setenv("TZ") + tzset(), so localtime_r() is local time in every task.

   The zone logic (AUTO's choice, the POSIX rule arithmetic, the clock text)
   is pure and runs on the bench; ls_tz_tick() is the board's half. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_TZ_AUTO 0

/* The SET list: 0 is AUTO, then UTC, then the named zones. */
int         ls_tz_count(void);
const char *ls_tz_name(int i);      /* "AUTO", "UTC", "US Eastern" ... */
const char *ls_tz_posix(int i);     /* NULL for AUTO */

/* AUTO's choice for a position: a zone index (never AUTO), or -1 when none
   of the named zones covers it; then `posix` and `label` hold a whole-hour
   zone from the longitude ("UTC-3"). Coarse: state and country lines are
   approximated, good to the right zone away from a border. */
int ls_tz_pick(double lat, double lon, char *posix, size_t pn, char *label, size_t ln);

/* POSIX TZ arithmetic: seconds east of UTC at `utc` under `posix`, and
   whether that is daylight time. Supports name[+-]hh[:mm][dst[off]][,Mm.w.d[/h],Mm.w.d[/h]]
   with <...> quoted names. False for a string it cannot read. */
bool ls_tz_offset_at(const char *posix, time_t utc, int *east_s, bool *dst);
/* The abbreviation in force ("EST", "EDT", "UTC") into out. */
void ls_tz_abbr_of(const char *posix, bool dst, char *out, size_t n);

/* The status clock: "4:34p" (12 h) or "16:34" (24 h), at most 6 chars. */
size_t ls_tz_clock_text(char *out, size_t n, int hour, int minute, bool h24);

/* ---- the board's zone (settings, the applied TZ) ---------------------- */
int  ls_tz_choice(void);
void ls_tz_set_choice(int i);
bool ls_tz_24h(void);
void ls_tz_set_24h(bool on);

/* What is in force now: its POSIX string and its name ("US Eastern",
   "UTC-3"; for AUTO the zone it picked). */
const char *ls_tz_current_posix(void);
const char *ls_tz_current_label(void);
/* SET's text for the row: "AUTO: US Eastern", "AUTO: UTC (no fix yet)", "UTC". */
void ls_tz_show(char *out, size_t n);
bool ls_tz_is_utc(void);

/* Local broken-down time and abbreviation of `t`, in the zone in force.
   The board uses localtime_r; the bench the same rules through ls_tz_offset_at. */
bool ls_tz_local(time_t t, struct tm *out);
void ls_tz_abbr(const struct tm *local, char *out, size_t n);

/* Re-evaluate the zone now (after a setting change); the tick does it every
   30 s for AUTO. Applies TZ when it changed. */
void ls_tz_apply(void);

/* The UI loop, every frame (cheap): AUTO follows the position, a clock set
   from the network or GPS is written back to the RTC so a reboot without
   either still knows the time, and the status text is kept. */
void ls_tz_tick(void);
/* The status bar's clock: local, "--:--" until the clock is set. */
void ls_tz_status_clock(char *out, size_t n);

#ifdef __cplusplus
}
#endif
