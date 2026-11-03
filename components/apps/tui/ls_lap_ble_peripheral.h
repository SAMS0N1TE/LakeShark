#ifndef LS_LAP_BLE_PERIPHERAL_H
#define LS_LAP_BLE_PERIPHERAL_H
#include "ls_lap_net_audio.h"
/* Feature macros are also usable in no-NimBLE builds. */
#if !defined(LS_LAP_PERIPH_DISABLED) &&                                                            \
    ((!LAP_DEVICE && !defined(LS_LAP_PERIPH_HOST_TEST)) ||                                         \
     (defined(CONFIG_LS_LAP_BLE_ADVERTISER) && CONFIG_LS_LAP_BLE_ADVERTISER))
#define LAP_PERIPH_ADV 1
#else
#define LAP_PERIPH_ADV 0
#endif
#if !defined(LS_LAP_PERIPH_DISABLED) &&                                                            \
    ((!LAP_DEVICE && !defined(LS_LAP_PERIPH_HOST_TEST)) ||                                         \
     (defined(CONFIG_LS_LAP_GATT_SERVER) && CONFIG_LS_LAP_GATT_SERVER))
#define LAP_PERIPH_GATT 1
#else
#define LAP_PERIPH_GATT 0
#endif
#if !defined(LS_LAP_PERIPH_DISABLED) &&                                                            \
    ((!LAP_DEVICE && !defined(LS_LAP_PERIPH_HOST_TEST)) ||                                         \
     (defined(CONFIG_LS_LAP_HID_DEVICE) && CONFIG_LS_LAP_HID_DEVICE))
#define LAP_PERIPH_HID 1
#else
#define LAP_PERIPH_HID 0
#endif
enum { PERIPH_OPEN = 1, PERIPH_ADVERTISE, PERIPH_STOP, PERIPH_SEND, PERIPH_CLOSE };
typedef struct {
    int kind, handle, count;
    uint16_t id;
    uint8_t len, indicate;
    uint8_t data[20];
    lap_ble_advert_config_t advert;
    lap_gatt_attribute_t attrs[LAP_BLE_SERVER_ATTRS];
} ls_lap_peripheral_command_t;
uint32_t ls_lap_peripheral_caps(void);
bool ls_lap_peripheral_service(void);
bool ls_lap_peripheral_reserved(void);
bool ls_lap_peripheral_live(int handle);
void ls_lap_peripheral_lost(void);
void ls_lap_peripheral_release_inputs(void);
bool ls_lap_ble_in_use(void);
bool ls_lap_peripheral_command(ls_lap_peripheral_command_t *out);
bool ls_lap_peripheral_complete(int h, int kind, int error, int power);
bool ls_lap_peripheral_event(int h, int kind, int id, int error, int value);
/* NimBLE-only synchronous cached ATT access; security is checked by NimBLE.
   Returns ATT codes 0, 0x0e unlikely, 0x11 resources, 0x0d length, 0x02/03
   perms. */
int ls_lap_peripheral_access(int h, int id, bool write, void *data, size_t *len);
int ls_lap_peripheral_peer_access(int h, int id, bool write, void *data, size_t *len);
void ls_lap_peripheral_script(const char *command);
/* Worker acquires exclusive host lease; host pump on existing NimBLE timer. */
int ls_lap_peripheral_lease(bool on);
void ls_lap_peripheral_host_pump(void);
#endif
