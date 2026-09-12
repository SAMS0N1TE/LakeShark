#ifndef LS_WIFI_H
#define LS_WIFI_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**/
/* SoftAP + HTTP file transfer, for getting photos and music on and off the board without giving up the RTL-SDR. */

esp_err_t ls_wifi_start(void);
esp_err_t ls_wifi_stop(void);
bool      ls_wifi_running(void);

/* Station-mode API. */

/* Operations reject concurrent ownership with ESP_ERR_INVALID_STATE (scan
 * returns -1; status prints busy). No caller waits for another operation.
 * Underlying driver calls still run on the calling task. */
typedef struct {
    char    ssid[33];
    int     rssi;
    bool    secure;
    int     channel;
} ls_wifi_scan_ap_t;

esp_err_t ls_wifi_sta_join(const char *ssid, const char *pass);
esp_err_t ls_wifi_sta_leave(void);
esp_err_t ls_wifi_sta_forget(void);
esp_err_t ls_wifi_sta_autojoin(void);
int       ls_wifi_sta_scan(ls_wifi_scan_ap_t *out, int cap);
esp_err_t ls_wifi_sta_info(ls_wifi_scan_ap_t *out);
bool      ls_wifi_sta_running(void);
bool      ls_wifi_sta_connected(void);
/* The station's address, or "" when not associated. Reads the cached
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
