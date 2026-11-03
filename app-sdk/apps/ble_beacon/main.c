#include <lakeshark_app.h>
#include <stdio.h>
#include <string.h>
static int handle;
static bool supported;
static lap_ble_advert_config_t advert;
static lap_ble_peripheral_status_t status;
static void start(void) {
    if (supported && handle <= 0)
        handle = lap_ble_advertise(&advert);
}
static void enter(void) {
    lap_caps_t caps;
    lap_caps(&caps, sizeof(caps));
    supported = (caps.hardware & LAP_HW_BLE_ADVERTISER) != 0;
    handle = 0;
    advert = (lap_ble_advert_config_t){.size = sizeof(advert),
                                       .version = 1,
                                       .interval_min_ms = 500,
                                       .interval_max_ms = 600,
                                       .tx_power_dbm = -3};
    /* Flags; complete battery-service UUID; manufacturer company=0xffff (demo);
       battery service data=88. AD records may also use a 128-bit service UUID. */
    const uint8_t data[] = {2,    1,    6, 3, 3, 0x0f, 0x18, 5,    0xff,
                            0xff, 0xff, 1, 2, 4, 0x16, 0x0f, 0x18, 88};
    memcpy(advert.data, data, sizeof(data));
    advert.data_len = sizeof(data);
    const uint8_t scan[] = {10, 9, 'L', 'a', 'k', 'e', 'S', 'h', 'a', 'r', 'k'};
    memcpy(advert.scan_response, scan, sizeof(scan));
    advert.scan_response_len = sizeof(scan);
    start();
}
static void leave(void) {
    if (handle > 0)
        lap_ble_advertise_stop(handle);
    handle = 0;
}
static bool key(lap_key_t k, char ch) {
    if (k != LAP_KEY_CHAR)
        return false;
    if (ch == 's') {
        leave();
        return true;
    }
    if (ch == 'a') {
        start();
        return true;
    }
    return false;
}
static void draw(void) {
    char text[100];
    memset(&status, 0, sizeof(status));
    if (handle > 0)
        lap_ble_peripheral_status(handle, &status);
    snprintf(text, sizeof(text), "BLE beacon: %s handle %d",
             supported ? "available" : "unsupported", handle);
    lap_put_str(0, 0, text, LAP_ATTR(LAP_CYAN, LAP_BLACK));
    snprintf(text, sizeof(text), "Advertising %u state %ld error %ld power %d dBm",
             status.advertising, (long)status.state, (long)status.error, status.tx_power_dbm);
    lap_put_str(0, 2, text, LAP_ATTR(LAP_WHITE, LAP_BLACK));
    lap_put_str(0, 4, "A advertise / S stop. LINK/HID host must release BLE.",
                LAP_ATTR(LAP_WHITE, LAP_BLACK));
}
LAP_APP(.id = "ble_beacon", .name = "BLE BEACON", .sub = "Advertisement publisher",
        .version = "1.0", .author = "LakeShark", .flags = LAP_CAP_RADIO_TX, .color = LAP_CYAN,
        .enter = enter, .leave = leave, .draw = draw, .key = key);
