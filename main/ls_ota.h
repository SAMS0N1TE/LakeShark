#pragma once
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

/* The `ota` console command. */
void ls_ota_console_register(void);

/* Share the updater lease with C6: a P4 install/restart and a C6 transfer
 * must never overlap. A P4 image still on trial also refuses the lease. */
int ls_ota_c6_acquire(void);
void ls_ota_c6_release(void);

#ifdef __cplusplus
}
#endif
