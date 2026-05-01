/*
 * diag_time.h — wall-clock offset for telemetry.
 *
 * The board has no battery-backed RTC. Every record carries `ts`
 * (uptime seconds, monotonic). To get a real `dateTime` field, the
 * host sends a TIME command on USB-Serial-JTAG once per session:
 *
 *   \x1e{"cmd":"set_time","epoch":1714512345.678}\n
 *
 * The JSON sink's RX command parser calls diag_time_set_epoch()
 * with the epoch value; we record (epoch - uptime_at_receipt) as the
 * offset and use it to synthesize `dateTime` in subsequent records.
 *
 * Until a TIME message arrives, diag_time_format_rfc3339() returns
 * an empty string and formatters skip the dateTime field entirely.
 */
#ifndef DIAG_TIME_H
#define DIAG_TIME_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Set wall-clock offset from host. epoch is seconds since 1970-01-01
 * UTC, fractional. Records the offset against the current uptime.
 * Calling this overwrites any prior offset (drift correction OK).
 */
void diag_time_set_epoch(double epoch);

/* True iff diag_time_set_epoch has been called at least once. */
bool diag_time_synced(void);

/* Write an RFC3339 string ("2026-04-30T18:23:45.678Z") into buf.
 * Returns number of chars written excluding NUL, or 0 if not synced
 * or buf is too small. buf_len should be ≥32.
 */
size_t diag_time_format_rfc3339(char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* DIAG_TIME_H */
