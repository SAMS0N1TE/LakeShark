#ifndef LS_WIFI_STA_CORE_H
#define LS_WIFI_STA_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pure logic for WiFi station mode: credential validation and reconnect
   backoff. Kept free of ESP-IDF so the bench can exercise it on the host.
   `ls_wifi.c` layers `esp_wifi` + NVS on top of this. */

/* WPA2/WPA3-PSK passphrase bounds. 64 is reserved for a raw hex PMK, which we
   do not accept from the console. */
#define LS_WIFI_SSID_MAX_LEN 32
#define LS_WIFI_PASS_MIN_LEN 8
#define LS_WIFI_PASS_MAX_LEN 63

/* Reconnect backoff clamps. Kept as macros so both the firmware and the bench
   test see the same numbers. */
#define LS_WIFI_BACKOFF_MIN_MS  1000u
#define LS_WIFI_BACKOFF_MAX_MS  60000u

/* SSID must be 1..32 bytes, no NUL in the middle. Non-printable bytes are
   allowed by 802.11 but we reject control characters below 0x20 so a stray
   line ending from the console cannot make a saved SSID unrecognisable. */
bool ls_wifi_ssid_valid(const char *ssid);

/* Passphrase must be either empty (open network) or 8..63 chars. */
bool ls_wifi_pass_valid(const char *pass);
void ls_wifi_copy_ssid(uint8_t out[32], const char *ssid);
const char *ls_wifi_reason_text(int reason);
void ls_wifi_connecting_status(char *out, size_t cap, const char *ssid,
                               int reason, uint32_t retry_ms);

/* Next backoff after a failure. Doubles, clamped to [min, max]. `cur` is the
   previous value; pass 0 to start at `min`. Reset with ls_wifi_backoff_reset. */
uint32_t ls_wifi_backoff_next(uint32_t cur, uint32_t min_ms, uint32_t max_ms);

/* WHEN TO STOP TRYING.

   The backoff clamps the interval but nothing clamped the number of
   attempts, so a stored network that is simply not here was retried every
   sixty seconds for as long as the board was on - each attempt waking the
   co-processor link to ask a question already answered. An idle device
   should settle, not keep poking a radio forever.

   "Network not found" is treated harder than the rest: reason 201 means the
   AP is not in range at all, which does not become true by asking again, so
   it gives up in a few tries. A handshake or auth failure might be a busy
   AP or a moment of interference and gets the longer count.

   Giving up is not forgetting: an explicit join, or the network appearing in
   a scan, re-arms it. Pure so the bench can pin it. */
#define LS_WIFI_TRIES_NOT_FOUND 4
#define LS_WIFI_TRIES_OTHER     12
bool ls_wifi_should_retry(int attempts, int reason);

/* Convenience: initial backoff value. */
static inline uint32_t ls_wifi_backoff_reset(uint32_t min_ms) { return min_ms; }

#ifdef __cplusplus
}
#endif

#endif
