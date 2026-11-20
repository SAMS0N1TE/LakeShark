/* ls_redact: scrub text that goes into a bug report. Pure C, no SDK.

   In place, never grows the text:
     - values after password/pass/psk/pwd/pin/token/secret/key/apikey/ssid/auth/bearer
       (`word=value`, `word: value`, `"word":"value"`, `word_x=value`) become ***
     - runs of 32+ hex digits (keys, digests of secrets) become [hex]
     - MAC addresses keep the vendor half: aa:bb:cc:**:**:**
     - IPv4 addresses keep two octets: 192.168.*.*
     - latitude/longitude pairs (two decimals of 4+ places side by side) become [gps] [gps],
       unless keep_gps.
   ls_json_esc appends src (n bytes) to dst as a JSON string body (no quotes). */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t ls_redact(char *s, bool keep_gps);
/* Returns the new length of dst; stops before overflowing cap (NUL kept). */
size_t ls_json_esc(char *dst, size_t len, size_t cap, const char *src, size_t n);

#ifdef __cplusplus
}
#endif
