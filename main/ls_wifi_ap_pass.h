/* The file hotspot's password: made once per board, kept, and shown.

   It used to be one word compiled into every board, so anyone who had read
   the source could join anyone's board. Now each board makes its own the
   first time it is needed and keeps it in NVS; Settings and the console show
   it, and a new one can be asked for.

   Ten letters and digits in two groups of five ("k7mq2-x9tbe"), from an
   alphabet without the look-alikes 0/o, 1/l/i: about 49 bits, and easy to
   read off a small screen and type on a phone. Header only, so the host
   bench tests the same code the board runs. */
#ifndef LS_WIFI_AP_PASS_H
#define LS_WIFI_AP_PASS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define LS_AP_PASS_ALPHABET "abcdefghjkmnpqrstuvwxyz23456789"
#define LS_AP_PASS_GROUP    5
#define LS_AP_PASS_LEN      (LS_AP_PASS_GROUP * 2 + 1)   /* with the dash */

/* True for a password this module could have made: what NVS hands back is
   checked before it is used, so a damaged entry is replaced, not served. */
static inline bool ls_ap_pass_valid(const char *p)
{
    if (!p || strlen(p) != LS_AP_PASS_LEN) return false;
    for (int i = 0; i < LS_AP_PASS_LEN; i++) {
        if (i == LS_AP_PASS_GROUP) { if (p[i] != '-') return false; continue; }
        if (!p[i] || !strchr(LS_AP_PASS_ALPHABET, p[i])) return false;
    }
    return true;
}

/* Fills out (at least LS_AP_PASS_LEN + 1 bytes) from rnd, a source of
   uniform 32-bit words (esp_random on the board). Rejection sampling keeps
   every letter equally likely. False when out is too small. */
static inline bool ls_ap_pass_make(char *out, size_t cap, uint32_t (*rnd)(void))
{
    static const char A[] = LS_AP_PASS_ALPHABET;
    const uint32_t n = (uint32_t)(sizeof(A) - 1);
    const uint32_t limit = UINT32_MAX - UINT32_MAX % n;
    if (!out || cap < LS_AP_PASS_LEN + 1 || !rnd) return false;
    for (int i = 0; i < LS_AP_PASS_LEN; i++) {
        if (i == LS_AP_PASS_GROUP) { out[i] = '-'; continue; }
        uint32_t r;
        do r = rnd(); while (r >= limit);
        out[i] = A[r % n];
    }
    out[LS_AP_PASS_LEN] = 0;
    return true;
}

#endif
