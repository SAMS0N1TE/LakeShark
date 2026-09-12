/**/

#include "rec_space.h"

#include <stdio.h>
#include <string.h>

/* .sub layout worst case:
     header block (Filetype/Version/Frequency/Preset/Protocol/Recorded)
       up to about 200 bytes across six lines - rounded up to 256 for slack.
     per-edge cost: `" %ld"` where a signed 32-bit long prints in up to 11
       characters plus the leading space - budget 12 bytes.
     line wrap: one newline + "RAW_Data:" every 512 edges, ~11 bytes/512 =
       trivial next to the per-edge budget, absorbed into that 12.
     sidecar JSON: rec_sidecar_format never exceeds REC_SIDECAR_JSON_MAX
       (512 bytes), which is what we budget here too.
   Pessimistic on purpose - refusing a save that would have fit is at worst
   a minor annoyance, whereas admitting a save that will not fit is exactly
   the bug this module exists to prevent. */
uint64_t rec_space_estimate_bytes(int edges)
{
    if (edges < 0) edges = 0;
    return 1024ull + (uint64_t)edges * 12ull;
}

bool rec_space_ok(uint64_t needed, uint64_t available)
{
    return available >= needed;
}

int rec_space_format_shortage(char *out, size_t len,
                              uint64_t needed, uint64_t available)
{
    if (!out || len == 0) return 0;
    int n = snprintf(out, len,
                     "need %llu B, only %llu B free",
                     (unsigned long long)needed,
                     (unsigned long long)available);
    if (n < 0) { out[0] = '\0'; return 0; }
    return n;
}

int rec_space_format_free(char *out, size_t len, uint64_t bytes_free)
{
    if (!out || len == 0) return 0;

    int n;
    if (bytes_free < 1024ull) {
        n = snprintf(out, len, "%llu B", (unsigned long long)bytes_free);
    } else if (bytes_free < 1024ull * 1024ull) {
        unsigned long whole = (unsigned long)(bytes_free / 1024ull);
        unsigned long frac  = (unsigned long)((bytes_free % 1024ull) * 10ull / 1024ull);
        n = snprintf(out, len, "%lu.%lu KB", whole, frac);
    } else if (bytes_free < 1024ull * 1024ull * 1024ull) {
        unsigned long long mb = bytes_free / 1024ull;
        unsigned long whole = (unsigned long)(mb / 1024ull);
        unsigned long frac  = (unsigned long)((mb % 1024ull) * 10ull / 1024ull);
        n = snprintf(out, len, "%lu.%lu MB", whole, frac);
    } else {
        unsigned long long gb = bytes_free / (1024ull * 1024ull);
        unsigned long whole = (unsigned long)(gb / 1024ull);
        unsigned long frac  = (unsigned long)((gb % 1024ull) * 10ull / 1024ull);
        n = snprintf(out, len, "%lu.%lu GB", whole, frac);
    }
    if (n < 0) { out[0] = '\0'; return 0; }
    return n;
}

bool rec_capture_name_is_partial(const char *name)
{
    if (!name) return false;
    size_t nl = strlen(name);
    size_t el = strlen(REC_CAPTURE_PART_EXT);
    if (nl < el) return false;
    return strcmp(name + nl - el, REC_CAPTURE_PART_EXT) == 0;
}
