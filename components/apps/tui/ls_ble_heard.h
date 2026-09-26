/* Bluetooth devices heard advertising, with their latest signal level, so
   a device can be chosen and followed by strength. Filled from the BLE
   scan callback; read by COMPASS. Nothing here talks to the radio. */

#ifndef LS_BLE_HEARD_H
#define LS_BLE_HEARD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_BLE_HEARD_MAX 24

typedef struct {
    uint8_t addr[6];
    char name[20];
    int8_t rssi;           /* latest advertisement */
    float smooth;          /* the same, averaged over a few adverts */
    uint32_t adverts;
    int64_t last_us;
} ls_ble_heard_t;

/* Scan callback context: cheap, bounded, no allocation. */
void ls_ble_heard_note(const uint8_t addr[6], const char *name, int name_len,
                       int rssi, int64_t now_us);
/* Strongest first, among devices heard within max_age_us. */
int  ls_ble_heard_list(ls_ble_heard_t *out, int cap, int64_t now_us, int64_t max_age_us);
bool ls_ble_heard_find(const uint8_t addr[6], ls_ble_heard_t *out);
void ls_ble_heard_clear(void);

#ifdef __cplusplus
}
#endif
#endif
