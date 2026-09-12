/* BLE control head, compiled out. */

#include "ble_link.h"
#include <string.h>

esp_err_t ble_link_start(void) { return ESP_ERR_NOT_SUPPORTED; }
void      ble_link_stop(void)  { }

ble_link_state_t ble_link_state(void)      { return BLE_LINK_OFF; }
const char      *ble_link_state_name(void) { return "DISABLED (built without CONFIG_LS_BLE_HEAD)"; }

void ble_link_set_name_filter(const char *substr) { (void)substr; }
void ble_link_get_name_filter(char *out, size_t len)
{
    if (out && len) out[0] = '\0';
}

void ble_link_peer(char *name, size_t name_len, char *addr, size_t addr_len)
{
    if (name && name_len) name[0] = '\0';
    if (addr && addr_len) addr[0] = '\0';
}

void ble_link_stats(uint32_t *rx_lines, uint32_t *tx_frames, uint32_t *drops)
{
    if (rx_lines)  *rx_lines  = 0;
    if (tx_frames) *tx_frames = 0;
    if (drops)     *drops     = 0;
}

void ble_link_rx_debug(uint16_t *tx_hnd, uint16_t *svc_start, uint16_t *svc_end,
                       uint32_t *foreign)
{
    if (tx_hnd)    *tx_hnd    = 0;
    if (svc_start) *svc_start = 0;
    if (svc_end)   *svc_end   = 0;
    if (foreign)   *foreign   = 0;
}

void ble_link_notify_stats(uint32_t *notifies, uint32_t *bytes)
{
    if (notifies) *notifies = 0;
    if (bytes)    *bytes    = 0;
}

void ble_link_set_tel_hz(int hz) { (void)hz; }
int  ble_link_tel_hz(void)       { return 0; }

void ble_link_allow_telemetry(bool allow) { (void)allow; }
void ble_link_set_verbose(bool en)        { (void)en; }
void ble_link_rescan(void)                { }

bool      ble_link_passkey_pending(void)          { return false; }
esp_err_t ble_link_submit_passkey(uint32_t code)  { (void)code; return ESP_ERR_NOT_SUPPORTED; }

/* Added to ble_link.c and ble_link.h without a twin here, which broke
   every CONFIG_LS_BLE_HEAD=n board at link time - found building the
   T-Display-P4, not by the gate, because `verify.ps1 -Level smoke` builds only
   the LCD 4.3. A board with no BLE has never seen a Flipper on any profile. */
bool      ble_link_stock_head_seen(void)          { return false; }

/* added peer pinning and bond-management entry points only to
   the BLE implementation, so CONFIG_LS_BLE_HEAD=n retained the commands but
   failed to link.  The no-radio build reports no connection or pinned peer
   and rejects operations that require a BLE host. */
bool ble_link_get_pinned(char *out_addr, size_t out_len)
{
    if (out_addr && out_len) out_addr[0] = '\0';
    return false;
}

esp_err_t ble_link_pin_peer(const char *addr)
{
    (void)addr;
    return ESP_ERR_NOT_SUPPORTED;
}

void ble_link_unpin_peer(void) { }

esp_err_t ble_link_forget_bonds(void) { return ESP_ERR_NOT_SUPPORTED; }

bool ble_link_is_connected(void) { return false; }
esp_err_t ble_link_rssi(int *rssi)
{
    (void)rssi;
    return ESP_ERR_NOT_SUPPORTED;
}
