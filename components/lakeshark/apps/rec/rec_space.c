/**/

#include "rec_space.h"

#include <stdio.h>
#include <string.h>

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
