#include <lakeshark_app.h>
#include <stdio.h>
#include <string.h>
static int scan = -1, connection = -1, operation, events, bytes, rssi;
static uint16_t first, last, attribute, cccd, manufacturer, service_uuid;
static char value_hex[61];
static lap_ble_advert_t selected;
static bool fitted;
static char message[80] = "Waiting for adverts";
static void enter(void) {
    lap_caps_t c;
    lap_caps(&c, sizeof(c));
    scan = connection = -1;
    events = 0;
    first = last = attribute = cccd = 0;
    memset(&selected, 0, sizeof(selected));
    manufacturer = service_uuid = 0;
    value_hex[0] = 0;
    fitted=(c.hardware&LAP_HW_BLE)!=0;
    if (fitted)
        scan = lap_ble_scan_start();
    else
        strcpy(message, "Bluetooth not fitted");
}
static void leave(void) {
    if (scan >= 0)
        lap_ble_scan_stop(scan);
    if (connection >= 0)
        lap_ble_disconnect(connection);
}
static bool key(lap_key_t k, char ch) {
    if (k != LAP_KEY_CHAR)
        return false;
    if (ch == 'c' && connection < 0 && selected.payload_size)
        connection = lap_ble_connect(selected.addr, selected.addr_type);
    else if (ch == 'd' && connection >= 0)
        operation = lap_gatt_discover(connection, 0, 0);
    else if (ch == 'g' && first)
        operation = lap_gatt_discover(connection, first, last);
    else if (ch == 'e' && attribute && last > attribute)
        operation = lap_gatt_descriptors(connection, attribute, last);
    else if (ch == 'r' && attribute)
        operation = lap_gatt_read(connection, attribute);
    /* Explicit W writes the one-byte value shown on the key legend. */
    else if (ch == 'w' && attribute) {
        uint8_t value = 1;
        operation = lap_gatt_write(connection, attribute, &value, 1);
    } else if (ch == 'n' && cccd)
        operation = lap_gatt_subscribe(connection, cccd, true);
    else if (ch == 'u' && cccd)
        operation = lap_gatt_subscribe(connection, cccd, false);
    else if (ch == 'x' && connection >= 0) {
        lap_ble_disconnect(connection);
        connection = -1;
    } else
        return false;
    return true;
}
static void draw(void) {
    if(fitted&&(scan==LAP_IO_EBUSY||scan==LAP_IO_EAGAIN))scan=lap_ble_scan_start();
    if (scan >= 0) {
        uint8_t payload[LAP_BLE_AD_MAX];
        lap_ble_advert_t a;
        int n = lap_ble_advert_next(scan, &a, payload, sizeof(payload));
        if (n > 0) {
            selected = a;
            rssi = a.rssi;
            bytes = n;
            manufacturer = service_uuid = 0;
            for (int j = 0; j < n;) {
                int len = payload[j];
                if (!len || j + len >= n)
                    break;
                uint8_t type = payload[j + 1];
                if (type == 0xff && len >= 3)
                    manufacturer = payload[j + 2] | (uint16_t)payload[j + 3] << 8;
                if ((type == 2 || type == 3) && len >= 3)
                    service_uuid = payload[j + 2] | (uint16_t)payload[j + 3] << 8;
                j += len + 1;
            }
        }
        lap_operation_t s;
        if (!lap_ble_scan_status(scan, &s) && s.error)
            snprintf(message, sizeof(message), "scan error %ld", (long)s.error);
    }
    lap_ble_event_t e;
    uint8_t data[20];
    for (int i = 0; i < 4 && lap_ble_event_next(&e, data, sizeof(data)) == 1; i++) {
        events++;
        snprintf(message, sizeof(message), "event %u op %ld attr %u error %ld drops %lu", e.kind,
                 (long)e.operation, e.attribute, (long)e.error, (unsigned long)e.dropped);
        if (e.kind == LAP_GATT_SERVICE) {
            first = e.attribute;
            last = e.end_handle;
        }
        if (e.kind == LAP_GATT_CHARACTERISTIC)
            attribute = e.attribute;
        if (e.kind == LAP_GATT_DESCRIPTOR && e.uuid.bytes[0] == 2 && e.uuid.bytes[1] == 0x29)
            cccd = e.attribute;
        if (e.kind == LAP_GATT_VALUE || e.kind == LAP_GATT_NOTIFY) {
            unsigned pos = 0;
            for (unsigned j = 0; j < e.payload_size && pos + 3 < sizeof(value_hex); j++)
                pos += snprintf(value_hex + pos, sizeof(value_hex) - pos, "%02x ", data[j]);
        }
        if (e.kind == LAP_BLE_DISCONNECTED)
            connection = -1;
    }
    char line[100];
    lap_clear(LAP_ATTR(LAP_WHITE, LAP_BLACK));
    lap_put_str(1, 1, "BLE EXPLORER", LAP_ATTR(LAP_CYAN | LAP_BRIGHT, LAP_BLACK));
    snprintf(line, sizeof(line), "AD %d bytes RSSI %d events %d op %d", bytes, rssi, events, operation);
    lap_put_str(1, 3, line, LAP_ATTR(LAP_WHITE, LAP_BLACK));
    snprintf(line, sizeof(line), "Manufacturer %04x service %04x seen %lu ms", manufacturer, service_uuid,
             (unsigned long)selected.at_ms);
    lap_put_str(1, 4, line, LAP_ATTR(LAP_WHITE, LAP_BLACK));
    lap_put_str(1, 6, value_hex, LAP_ATTR(LAP_GREEN, LAP_BLACK));
    lap_put_str(1, 5, message, LAP_ATTR(LAP_WHITE, LAP_BLACK));
    lap_put_str(1, 7, "C connect D services G chars E descriptors", LAP_ATTR(LAP_YELLOW, LAP_BLACK));
    lap_put_str(1, 8, "R read W write 01 N/U notify X disconnect", LAP_ATTR(LAP_YELLOW, LAP_BLACK));
}
LAP_APP(.id = "ble_explorer", .name = "BLE EXPLORER", .sub = "scan and GATT", .version = "1.0",
        .author = "LakeShark", .icon = LAP_ICON_TOWER, .color = LAP_CYAN, .enter = enter, .leave = leave,
        .draw = draw, .key = key);
