/* API 2.7: raw Wi-Fi and Bluetooth capture for apps. Receive only.
 * One PSRAM ring per source, allocated while a capture is open; producers
 * try the lock and count a drop instead of waiting. No internal RAM. */
#ifndef LS_LAP_CAPTURE_H
#define LS_LAP_CAPTURE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* NimBLE host task, one advertising report. pdu is the HCI event type
 * (0 ADV_IND .. 4 SCAN_RSP, 7 extended). Cheap when no capture is open. */
void ls_lap_capture_ble(const uint8_t addr[6], uint8_t addr_type, uint8_t pdu, int rssi,
                        const uint8_t *data, size_t n, int64_t at_us);
/* The closing app's captures stop; Wi-Fi returns to its channel. */
#ifdef ESP_PLATFORM
void ls_lap_capture_end(void);
#else   /* host harnesses that link the app runtime without capture */
void ls_lap_capture_end(void) __attribute__((weak));
#endif
#ifdef __cplusplus
}
#endif
#endif
