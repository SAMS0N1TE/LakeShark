#include <lakeshark_app.h>
#include <stdio.h>
#include <string.h>
static int device, kind = LAP_HID_GAMEPAD, result;
static bool supported;
static uint32_t sent_at;
static unsigned reports, events;
static uint8_t report_map[128];
static int map_size;
static lap_ble_peripheral_status_t status;
static void enter(void) {
    lap_caps_t caps;
    lap_caps(&caps, sizeof(caps));
    supported = (caps.hardware & LAP_HW_HID_DEVICE) != 0;
    reports = events = 0;
    device = 0;
    sent_at = 0;
    if (supported) {
        map_size = lap_hid_device_report_map(kind, report_map, sizeof(report_map));
        device = lap_hid_device_open(kind, "LakeShark Controller");
    }
}
static void leave(void) {
    if (device > 0)
        lap_hid_device_close(device);
    device = 0;
}
static bool key(lap_key_t k, char ch) {
    if (k != LAP_KEY_CHAR)
        return false;
    /* Switch after close acknowledgement (A opens; 1/2/3 select). */
    if (ch >= '1' && ch <= '3') {
        leave();
        kind = ch - '0';
        return true;
    }
    if (ch == 'a' && device <= 0) {
        enter();
        return true;
    }
    if (ch == 's') {
        leave();
        return true;
    }
    return false;
}
static void draw(void) {
    if (device > 0) {
        lap_ble_peripheral_status(device, &status);
        lap_gatt_server_event_t e;
        uint8_t payload[128];
        for (int i = 0; i < 8 && lap_gatt_server_next(device, &e, payload, sizeof(payload)) == 1;
             i++)
            events++;
        if (status.connected && lap_millis() - sent_at >= 40) {
            uint8_t report[8] = {0};
            size_t n = kind == 1 ? 8 : kind == 2 ? 4 : 6;
            /* Held input sends release reports too. Keyboard sends space; mouse
               uses arrow movement; gamepad axes follow arrows and space=button1. */
            bool left = lap_key_down(LAP_KEY_LEFT), right = lap_key_down(LAP_KEY_RIGHT);
            bool up = lap_key_down(LAP_KEY_UP), down = lap_key_down(LAP_KEY_DOWN);
            bool fire = lap_key_down(128 + ' ');
            if (kind == LAP_HID_KEYBOARD) {
                if (fire)
                    report[2] = 0x2c;
            } else if (kind == LAP_HID_MOUSE) {
                report[0] = fire;
                report[1] = (right - left) * 4;
                report[2] = (down - up) * 4;
            } else {
                report[0] = fire;
                report[2] = (right - left) * 100;
                report[3] = (down - up) * 100;
            }
            result = lap_hid_device_send(device, report, n);
            if (!result)
                reports++;
            sent_at = lap_millis();
        }
    }
    char text[100];
    snprintf(text, sizeof(text), "Board HID %s type %d handle %d map %d bytes",
             supported ? "available" : "unsupported", kind, device, map_size);
    lap_put_str(0, 0, text, LAP_ATTR(LAP_CYAN, LAP_BLACK));
    snprintf(text, sizeof(text), "Connected %u encrypted %u reports %u events %u result %d",
             status.connected, status.encrypted, reports, events, result);
    lap_put_str(0, 2, text, LAP_ATTR(LAP_WHITE, LAP_BLACK));
    lap_put_str(0, 4, "1 keyboard / 2 mouse / 3 gamepad; A open; S stop",
                LAP_ATTR(LAP_WHITE, LAP_BLACK));
    lap_put_str(0, 5, "Pair on PC/phone. Arrows move; space button. Release clears.",
                LAP_ATTR(LAP_WHITE, LAP_BLACK));
}
LAP_APP(.id = "board_gamepad", .name = "BOARD GAMEPAD", .sub = "BLE keyboard/mouse/gamepad",
        .version = "1.0", .author = "LakeShark", .flags = LAP_CAP_RADIO_TX | LAP_CAP_HID_DEVICE,
        .color = LAP_CYAN, .enter = enter, .leave = leave, .draw = draw, .key = key);
