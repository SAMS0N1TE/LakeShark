#ifndef REC_SIDECAR_H
#define REC_SIDECAR_H

/**/
/* A saved capture is a `.sub` file the Flipper expects to see byte-for-byte: Filetype/Version/Frequency/Preset/Protocol/RAW_Data. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sidecar file extension.  Kept short so a per-capture pair "recNNN.sub" +
   "recNNN.json" always fits any name buffer the .sub already fits in. */
#define REC_SIDECAR_EXT       ".json"

/* Long enough for the ISO-8601 UTC form ls_time_render_stamp emits, the
   "up <seconds>s" fallback (LS_TIME_STAMP_MAX is 32), and a small margin. */
#define REC_SIDECAR_TIME_MAX  32

/* Long enough for LS_BOARD_NAME plus `git describe --always --dirty`
   output the version formatter also budgets for. */
#define REC_SIDECAR_ID_MAX    64

/* Header formatter buffer size that always fits the JSON body this module
   emits, with headroom.  A single object of ~11 fields at their maximum
   textual width fits comfortably inside 512 bytes. */
#define REC_SIDECAR_JSON_MAX  512

typedef struct {
    /* Wall clock at capture time.  ISO-8601 UTC when the clock is set, the
       "up <seconds>s" uptime marker when not - 's contract, so a
       sidecar written before SNTP never looks like a wrong date. */
    char        time[REC_SIDECAR_TIME_MAX];

    uint32_t    freq_hz;
    int         gain_tenths;
    uint32_t    bw_hz;          /* 0 == auto, matches rec_get_bw() */
    uint32_t    sample_rate;
    int         edges;
    uint32_t    span_us;
    int         mag_peak;       /* peak magnitude across the whole capture */
    int         mag_floor;      /* floor at the moment the capture landed */

    char        board[REC_SIDECAR_ID_MAX];
    char        firmware[REC_SIDECAR_ID_MAX];
} rec_sidecar_t;

/* Format `s` as JSON into `out`.  Returns the number of bytes written
   (excluding the NUL) or 0 on failure.  The output is a single flat object
   so the parser can walk it with a linear scan - no nesting, no arrays. */
int  rec_sidecar_format(char *out, size_t len, const rec_sidecar_t *s);

/* Parse a JSON sidecar from `in` into `out`.  Missing fields keep whatever
   `out` was zero-initialised to; unknown fields are ignored.  Returns true
   on success (a syntactically valid object was found), false on failure -
   an all-zero struct is still safe for callers to read. */
bool rec_sidecar_parse(const char *in, rec_sidecar_t *out);

/* Write `s` as `<dir>/<base>.json`.  Returns 0 on success, -1 on error.
   Uses "wx" exclusive-create for the same reason rec_save does: a stale
   sidecar from an older capture must not be silently paved over. */
int  rec_sidecar_write(const char *dir, const char *base, const rec_sidecar_t *s);

bool rec_sidecar_read(const char *dir, const char *base, rec_sidecar_t *out);

#ifdef __cplusplus
}
#endif

#endif
