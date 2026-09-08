#ifndef LS_WIFI_H
#define LS_WIFI_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*LS-738*/
/* SoftAP + HTTP file transfer, for getting photos and music on and off the
   board without giving up the RTL-SDR.
   THE RADIO CANNOT DO THIS. An RTL2832U/R820T is receive-only - there is no
   transmitter in it - so it can never send a file. And the P4 has ONE USB OTG
   controller which the dongle holds in HOST mode, so USB mass-storage would
   mean unplugging the radio to move a file. WiFi is the only path that does
   not fight the receiver.

   WIFI-AP AND BLE ARE MUTUALLY EXCLUSIVE HERE, ON PURPOSE. Both run through
   the same C6 over SDIO, and LS-110 records that a failed C6 link boot-loops
   with "do NOT retry past this". Coexistence is a second thing that can go
   wrong on the one subsystem with a documented brick; a toggle is not.
   Starting the AP stops the BLE head and says so, same shape as the
   tuner-ownership rule in LS-731.

   Station mode (below) is different. It has no listener and is one association
   at most, so it does NOT tear the BLE head down - see LS-830. */
esp_err_t ls_wifi_start(void);
esp_err_t ls_wifi_stop(void);
bool      ls_wifi_running(void);

/*LS-830  Station-mode API.

   `join` validates SSID/pass, saves them to NVS under the `ls_wifi`
   namespace, and drives esp_wifi_connect() with backoff on drop. `autojoin`
   reads the last saved credentials and joins with them; call it once at boot
   after the C6 link is up. `leave` disconnects without erasing; `forget`
   erases the saved credentials as well.

   The passphrase MUST NOT be printed by any caller: logs, console echo,
   status lines. `ls_wifi_status` prints the SSID (fine) and never the
   passphrase (checked). */
/* Operations reject concurrent ownership with ESP_ERR_INVALID_STATE (scan
 * returns -1; status prints busy). No caller waits for another operation.
 * Underlying driver calls still run on the calling task. */
typedef struct {
    char    ssid[33];
    int     rssi;
    bool    secure;
} ls_wifi_scan_ap_t;

esp_err_t ls_wifi_sta_join(const char *ssid, const char *pass);
esp_err_t ls_wifi_sta_leave(void);
esp_err_t ls_wifi_sta_forget(void);
esp_err_t ls_wifi_sta_autojoin(void);
int       ls_wifi_sta_scan(ls_wifi_scan_ap_t *out, int cap);
bool      ls_wifi_sta_running(void);
bool      ls_wifi_sta_connected(void);
/*LS-815  The station's address, or "" when not associated. Reads the cached
   string set by IP_EVENT_STA_GOT_IP, so it is safe to call from a UI tick -
   ls_wifi_sta_status() serialises through the worker and can block. */
void      ls_wifi_sta_ip(char *out, int cap);
void      ls_wifi_sta_status(char *buf, int cap);

/* Fills a one-line human summary: AP state and STA state on one line. */
void      ls_wifi_status(char *buf, int cap);

#ifdef __cplusplus
}
#endif

#endif
