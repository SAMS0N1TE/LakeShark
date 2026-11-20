#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Updates over WiFi from the signed manifest on the update server. */

#ifdef __cplusplus
extern "C" {
#endif

/* The ota.state value, the ota.step action and the first-boot check of a
   freshly installed build. Once, beside the other built-in values. */
void ls_ota_publish(void);

/* Shared TLS trust, deadline and OTA-host fallback for signed data downloads.
 * A token is only sent to these hosts; token-bearing redirects are refused.
 * The worker owns closing and cleaning up the returned client. */
struct esp_http_client *ls_ota_open_data(const char *url, const char *token,
                                      int64_t deadline_us, int64_t *length, int *status);

/* For ls_fetch: any https URL, starting `from` bytes in (a Range request;
 * the answer is then 206, or 200 when the server sent it all). The caller
 * closes and cleans up. *status < 0: -1 unreachable, -2 left https, -3 deadline. */
struct esp_http_client *ls_ota_open_from(const char *url, const char *token, uint32_t from,
                                      int64_t deadline, int64_t *length, int *status);
/* An RF app is streaming or receiving: background traffic waits. */
bool ls_ota_rf_busy(void);

/* The `ota` console command. */
void ls_ota_console_register(void);

/* Share the updater lease with C6: a P4 install/restart and a C6 transfer
 * must never overlap. A P4 image still on trial also refuses the lease. */
int ls_ota_c6_acquire(void);
void ls_ota_c6_release(void);

#ifdef __cplusplus
}
#endif
