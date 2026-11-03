#include <lakeshark_app.h>
#include <stdio.h>
#include <string.h>
static int server, gps;
static bool supported, advertised;
static uint32_t refreshed;
static unsigned reads, writes;
static lap_ble_peripheral_status_t status;
static lap_gatt_attribute_t attrs[5];
static lap_gatt_attribute_t attribute(int id, int parent, int kind, int uuid, int flags) {
    lap_gatt_attribute_t a = {.size = sizeof(a),
                              .version = 1,
                              .id = id,
                              .parent = parent,
                              .kind = kind,
                              .flags = flags,
                              .uuid = {.length = 2}};
    a.uuid.bytes[0] = uuid;
    a.uuid.bytes[1] = uuid >> 8;
    return a;
}
static void enter(void) {
    lap_caps_t caps;
    lap_caps(&caps, sizeof(caps));
    supported = (caps.hardware & LAP_HW_GATT_SERVER) != 0;
    server = 0;
    gps = 0;
    advertised = false;
    reads = writes = 0;
    refreshed = 0;
    if (!supported)
        return;
    attrs[0] = attribute(1, 0, LAP_GATTS_SERVICE, 0x180f, 0);
    attrs[1] = attribute(2, 1, LAP_GATTS_CHARACTERISTIC, 0x2a19, LAP_GATTS_READ | LAP_GATTS_NOTIFY);
    attrs[1].value_len = 1;
    attrs[2] = attribute(3, 2, LAP_GATTS_DESCRIPTOR, 0x2901, LAP_GATTS_READ);
    memcpy(attrs[2].value, "Battery percent", 15);
    attrs[2].value_len = 15;
    attrs[3] = attribute(4, 0, LAP_GATTS_SERVICE, 0x1819, 0);
    attrs[4] = attribute(5, 4, LAP_GATTS_CHARACTERISTIC, 0x2a67, LAP_GATTS_READ | LAP_GATTS_NOTIFY);
    /* Standard Location and Speed: flags LE16=position present, lat/lon LE32
       in 1e-7 degrees; flags=0 while no fix (two bytes). */
    attrs[4].value_len = 2;
    server = lap_gatt_server_open(attrs, 5);
    if (caps.hardware & LAP_HW_GPS)
        gps = lap_gps_acquire(0);
}
static void leave(void) {
    if (server > 0)
        lap_gatt_server_close(server);
    server = 0;
    if (gps > 0)
        lap_gps_release(gps);
    gps = 0;
}
static void draw(void) {
    if (server > 0) {
        lap_ble_peripheral_status(server, &status);
        if (status.state == LAP_OPERATION_READY && !status.error && !advertised) {
            lap_ble_advert_config_t a = {.size = sizeof(a),
                                         .version = 1,
                                         .server = server,
                                         .connectable = 1,
                                         .interval_min_ms = 100,
                                         .interval_max_ms = 120,
                                         .data_len = 9,
                                         .data = {2, 1, 6, 5, 3, 0x0f, 0x18, 0x19, 0x18},
                                         .scan_response_len = 8,
                                         .scan_response = {7, 9, 'L', 'S', ' ', 'G', 'P', 'S'}};
            advertised = lap_ble_advertise(&a) > 0;
        }
        lap_gatt_server_event_t e;
        uint8_t data[128];
        for (int i = 0; i < 8 && lap_gatt_server_next(server, &e, data, sizeof(data)) == 1; i++) {
            if (e.kind == LAP_PERIPHERAL_DISCONNECTED ||
                (e.kind == LAP_PERIPHERAL_CONNECTED && e.error))
                advertised = false;
            reads += e.kind == LAP_PERIPHERAL_READ;
            writes += e.kind == LAP_PERIPHERAL_WRITE;
        }
        if (lap_millis() - refreshed >= 1000) {
            refreshed = lap_millis();
            lap_battery_t b = {0};
            lap_battery(&b);
            uint8_t percent = b.percent < 0 ? 0 : b.percent > 100 ? 100 : b.percent;
            lap_gatt_server_set(server, 2, &percent, 1);
            if (status.connected)
                lap_gatt_notify(server, 2, &percent, 1, false);
            lap_gps_t fix;
            memset(&fix, 0, sizeof(fix));
            if (gps > 0)
                lap_gps(&fix);
            uint8_t pos[10] = {0};
            size_t n = 2;
            if (fix.fix) {
                pos[0] = 4;
                int32_t lat = (int32_t)(fix.lat * 1e7), lon = (int32_t)(fix.lon * 1e7);
                for (int j = 0; j < 4; j++) {
                    pos[2 + j] = (uint32_t)lat >> (j * 8);
                    pos[6 + j] = (uint32_t)lon >> (j * 8);
                }
                n = 10;
            }
            lap_gatt_server_set(server, 5, pos, n);
            if (status.connected)
                lap_gatt_notify(server, 5, pos, n, false);
        }
    }
    char text[120];
    snprintf(text, sizeof(text), "GATT sensor %s: %d state %ld error %ld",
             supported ? "available" : "unsupported", server, (long)status.state,
             (long)status.error);
    lap_put_str(0, 0, text, LAP_ATTR(LAP_CYAN, LAP_BLACK));
    snprintf(text, sizeof(text), "Connected %u MTU %u bonded %u read %u write %u drops %lu",
             status.connected, status.mtu, status.bonded, reads, writes,
             (unsigned long)status.dropped);
    lap_put_str(0, 2, text, LAP_ATTR(LAP_WHITE, LAP_BLACK));
    lap_put_str(0, 4, "Battery + Location/Speed. GPS absent: no position flag.",
                LAP_ATTR(LAP_WHITE, LAP_BLACK));
}
LAP_APP(.id = "gatt_sensor", .name = "GATT SENSOR", .sub = "Battery and GPS peripheral",
        .version = "1.0", .author = "LakeShark", .flags = LAP_CAP_RADIO_TX, .color = LAP_CYAN,
        .enter = enter, .leave = leave, .draw = draw);
