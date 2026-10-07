#pragma once

/* Updates over WiFi from the signed manifest on the update server. */

#ifdef __cplusplus
extern "C" {
#endif

/* The ota.state value, the ota.step action and the first-boot check of a
   freshly installed build. Once, beside the other built-in values. */
void ls_ota_publish(void);

/* The `ota` console command. */
void ls_ota_console_register(void);

#ifdef __cplusplus
}
#endif
