/* B3/B4 contracts, no GUI/network/device; actual shared state and fixture
 * parser. */
#include "ls_lap_ble_peripheral.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
static unsigned checks, selected_slot;
unsigned ls_lap_instance_slot(void) { return selected_slot; }
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        assert(x);                                                                                 \
        checks++;                                                                                  \
    } while (0)
static bool inside = true, central;
static uint32_t hw = LAP_HW_BLE, grants = LAP_CAP_RADIO_TX;
atomic_uint ls_lap_av_epochs[LS_LAP_CONTEXTS] = { [0 ... LS_LAP_CONTEXTS-1] = 1 };
atomic_bool ls_lap_av_actives[LS_LAP_CONTEXTS] = { [0 ... LS_LAP_CONTEXTS-1] = true };
bool ls_lap_api_inside(void) { return inside; }
bool ls_lap_api_readable(const void *d, size_t n) {
    return (uintptr_t)d > 4096 && n <= 8192 && (uintptr_t)d + n >= (uintptr_t)d;
}
uint32_t ls_lap_sys_caps(void) { return hw; }
uint32_t ls_lap_granted(void) { return grants; }
bool ls_lap_ble_in_use(void) { return central; }
void ls_lap_av_wake(void) {}
int ls_lap_av_access(uint32_t hardware, bool network) {
    (void)network;
    return !inside || !atomic_load(&ls_lap_av_active)
               ? LAP_IO_EINVAL
               : ((hw & hardware) != hardware ? LAP_IO_ENOTSUP : 0);
}
bool ls_lap_av_string(char *out, size_t cap, const char *in) {
    if (!in)
        return false;
    size_t n = strlen(in);
    if (n >= cap)
        return false;
    memcpy(out, in, n + 1);
    return true;
}
#if !defined(LS_LAP_PERIPH_DISABLED)
static void tick(void) {
    for (int i = 0; i < 5; i++)
        ls_lap_peripheral_service();
}
static void close_session(int h) {
    lap_gatt_server_close(INT_MAX);
    lap_gatt_server_close(h);
    tick();
    CHECK(!ls_lap_peripheral_reserved());
}
static void drain(int h) {
    lap_gatt_server_event_t e;
    uint8_t data[128];
    while (lap_gatt_server_next(h, &e, data, 128) == 1) {
    }
}
static lap_gatt_attribute_t a[4];
static void db(void) {
    memset(a, 0, sizeof(a));
    for (int i = 0; i < 4; i++) {
        a[i].size = sizeof(a[i]);
        a[i].version = 1;
        a[i].id = i + 1;
        a[i].uuid.length = 2;
        a[i].uuid.bytes[0] = i + 1;
    }
    a[0].kind = LAP_GATTS_SERVICE;
    a[1].kind = LAP_GATTS_CHARACTERISTIC;
    a[1].parent = 1;
    a[1].flags = LAP_GATTS_READ | LAP_GATTS_WRITE | LAP_GATTS_NOTIFY | LAP_GATTS_INDICATE;
    a[1].value_len = 1;
    a[1].value[0] = 88;
    a[2].kind = LAP_GATTS_DESCRIPTOR;
    a[2].parent = 2;
    a[2].flags = LAP_GATTS_READ;
    a[2].value_len = 1;
    a[2].value[0] = 42;
    a[3].kind = LAP_GATTS_CHARACTERISTIC;
    a[3].parent = 1;
    a[3].flags = LAP_GATTS_READ | LAP_GATTS_AUTHENTICATE;
}
#endif
int main(void) {
    lap_ble_advert_config_t adv = {.size = sizeof(adv),
                                   .version = 1,
                                   .interval_min_ms = 100,
                                   .interval_max_ms = 120,
                                   .tx_power_dbm = 0,
                                   .data_len = 3,
                                   .data = {2, 1, 6},
                                   .scan_response_len = 3,
                                   .scan_response = {2, 9, 'A'}};
    lap_ble_peripheral_status_t s;
#if defined(LS_LAP_PERIPH_DISABLED)
    CHECK(ls_lap_peripheral_caps() == 0);
    CHECK(lap_ble_advertise(&adv) == LAP_IO_ENOTSUP);
    CHECK(lap_gatt_server_open(NULL, 4) == LAP_IO_ENOTSUP);
    CHECK(lap_ble_peripheral_status(1, &s) == LAP_IO_ENOTSUP);
    CHECK(lap_hid_device_open(3, "X") == LAP_IO_ENOTSUP);
    CHECK(!ls_lap_peripheral_service());
#else
    CHECK(ls_lap_peripheral_caps() ==
          (LAP_HW_BLE_ADVERTISER | LAP_HW_GATT_SERVER | LAP_HW_HID_DEVICE));
    hw = 0;
    CHECK(lap_ble_advertise(&adv) == LAP_IO_ENOTSUP);
    hw = LAP_HW_BLE;
    grants = 0;
    CHECK(lap_ble_advertise(&adv) == LAP_IO_EACCES);
    grants = LAP_CAP_RADIO_TX;
    inside = false;
    CHECK(lap_ble_advertise(&adv) == LAP_IO_EINVAL);
    inside = true;
    CHECK(lap_ble_advertise((void *)1) == LAP_IO_EINVAL);
    adv.version = 2;
    CHECK(lap_ble_advertise(&adv) == LAP_IO_EINVAL);
    adv.version = 1;
    adv.data_len = 32;
    CHECK(lap_ble_advertise(&adv) == LAP_IO_EINVAL);
    adv.data_len = 3;
    adv.data[0] = 4;
    CHECK(lap_ble_advertise(&adv) == LAP_IO_EINVAL);
    adv.data[0] = 2;
    adv.tx_power_dbm = 21;
    CHECK(lap_ble_advertise(&adv) == LAP_IO_EPOWER);
    adv.tx_power_dbm = 0;
    adv.connectable = 1;
    CHECK(lap_ble_advertise(&adv) == LAP_IO_EINVAL);
    adv.connectable = 0;
    central = true;
    CHECK(lap_ble_advertise(&adv) == LAP_IO_EBUSY);
    central = false;
    ls_lap_peripheral_script("periphbusy=1");
    int h = lap_ble_advertise(&adv);
    CHECK(h > 0);
    tick();
    CHECK(!lap_ble_peripheral_status(h, &s) && s.error == LAP_IO_EBUSY);
    close_session(h);
    ls_lap_peripheral_script("periphbusy=0");
    h = lap_ble_advertise(&adv);
    CHECK(h > 0);
    lap_ble_advertise_stop(h);
    tick();
    CHECK(!ls_lap_peripheral_reserved());
    h = lap_ble_advertise(&adv);
    CHECK(h > 0);
    tick();
    CHECK(!lap_ble_peripheral_status(h, &s) && s.advertising && s.state == LAP_OPERATION_READY);
    /* A resident's BLE session survives another instance's worker pass/unload. */
    selected_slot = 1;
    CHECK(lap_ble_peripheral_status(h, &s) == LAP_IO_ECLOSED);
    CHECK(lap_ble_advertise(&adv) == LAP_IO_EBUSY);
    lap_gatt_server_close(h);
    atomic_fetch_add(&ls_lap_av_epoch, 1);
    tick();
    CHECK(ls_lap_peripheral_live(h));
    selected_slot = 0;
    CHECK(!lap_ble_peripheral_status(h, &s) && s.advertising);
    close_session(h);
    CHECK(lap_ble_peripheral_status(h, &s) == LAP_IO_ECLOSED);
    db();
    a[2].id = 2;
    CHECK(lap_gatt_server_open(a, 4) == LAP_IO_EINVAL);
    db();
    a[2].parent = 1;
    CHECK(lap_gatt_server_open(a, 4) == LAP_IO_EINVAL);
    db();
    h = lap_gatt_server_open(a, 4);
    CHECK(h > 0);
    memset(a, 0, sizeof(a));
    tick();
    CHECK(!lap_ble_peripheral_status(h, &s) && s.state == LAP_OPERATION_READY);
    CHECK(lap_gatt_server_open(a, 4) == LAP_IO_EINVAL);
    adv.server = h;
    adv.connectable = 1;
    CHECK(lap_ble_advertise(&adv) == h);
    tick();
    CHECK(!lap_ble_peripheral_status(h, &s) && s.advertising);
    lap_ble_advertise_stop(h);
    tick();
    CHECK(!lap_ble_peripheral_status(h, &s) && !s.advertising);
    CHECK(lap_gatt_notify(h, 2, "x", 1, false) == LAP_IO_EAGAIN);
    ls_lap_peripheral_script("periphconnect");
    ls_lap_peripheral_script("periphmtu=185");
    ls_lap_peripheral_script("periphbond=1");
    CHECK(!lap_ble_peripheral_status(h, &s) && s.connected && s.mtu == 185 && s.bonded &&
          s.encrypted);
    drain(h);
    uint8_t data[128];
    size_t n = 128;
    CHECK(ls_lap_peripheral_access(h, 2, false, data, &n) == 0 && n == 1 && data[0] == 88);
    CHECK(lap_gatt_server_set(h, 2, "new", 3) == 0);
    n = 128;
    CHECK(!ls_lap_peripheral_access(h, 2, false, data, &n) && n == 3 && !memcmp(data, "new", 3));
    drain(h);
    ls_lap_peripheral_script("periphwrite=2,010203");
    lap_gatt_server_event_t e;
    CHECK(lap_gatt_server_next(h, &e, data, 1) == LAP_IO_ELIMIT);
    CHECK(lap_gatt_server_next(h, &e, data, 128) == 1 && e.kind == LAP_PERIPHERAL_WRITE &&
          e.id == 2 && data[2] == 3);
    CHECK(ls_lap_peripheral_access(h, 3, true, data, &n) == 3);
    ls_lap_peripheral_script("periphsubscribe=2,3");
    drain(h);
    CHECK(lap_gatt_notify(h, 2, data, 21, false) == LAP_IO_ELIMIT);
    CHECK(lap_gatt_notify(h, 3, data, 1, false) == LAP_IO_EINVAL);
    CHECK(lap_gatt_notify(h, 2, "OK", 2, false) == 0);
    CHECK(lap_gatt_notify(h, 2, "OK", 2, false) == LAP_IO_EBUSY);
    tick();
    CHECK(lap_gatt_server_next(h, &e, data, 128) == 1 && e.kind == LAP_PERIPHERAL_SENT && !e.error);
    CHECK(lap_gatt_notify(h, 2, "OK", 2, true) == 0);
    tick();
    drain(h);
    for (int i = 0; i < 8; i++) {
        n = 128;
        CHECK(!ls_lap_peripheral_access(h, 2, false, data, &n));
    }
    n = 128;
    CHECK(ls_lap_peripheral_access(h, 2, true, data, &n) == 0x11);
    CHECK(!lap_ble_peripheral_status(h, &s) && s.dropped > 0);
    drain(h);
    ls_lap_peripheral_script("periphdisconnect");
    CHECK(!lap_ble_peripheral_status(h, &s) && !s.connected && s.mtu == 23 && !s.bonded);
    close_session(h);
    db();
    h = lap_gatt_server_open(a, 4);
    CHECK(h > 0);
    tick();
    atomic_fetch_add(&ls_lap_av_epoch, 1);
    tick();
    CHECK(!ls_lap_peripheral_reserved());
    CHECK(lap_gatt_server_set(h, 2, "x", 1) == LAP_IO_ECLOSED);
    grants = LAP_CAP_RADIO_TX;
    CHECK(lap_hid_device_open(3, "test") == LAP_IO_EACCES);
    grants |= LAP_CAP_HID_DEVICE;
    for (int kind = 1; kind <= 3; kind++) {
        CHECK(lap_hid_device_report_map(kind, data, 1) == LAP_IO_ELIMIT);
        CHECK(lap_hid_device_report_map(kind, data, 128) > 40);
        h = lap_hid_device_open(kind, "LakeShark");
        CHECK(h > 0);
        tick();
        CHECK(!lap_ble_peripheral_status(h, &s) && s.advertising);
        drain(h);
        ls_lap_peripheral_script("periphconnect");
        ls_lap_peripheral_script("periphsubscribe=6,1");
        size_t len = kind == 1 ? 8 : kind == 2 ? 4 : 6;
        memset(data, 0, sizeof(data));
        CHECK(lap_hid_device_send(h, data, len - 1) == LAP_IO_EINVAL);
        CHECK(lap_hid_device_send(h, data, len) == LAP_IO_EAUTH);
        ls_lap_peripheral_script("periphbond=1");
        CHECK(lap_hid_device_send(h, data, len) == 0);
        tick();
        n = 1;
        data[0] = 0;
        CHECK(ls_lap_peripheral_access(h, 5, true, data, &n) == 0x13);
        lap_hid_device_close(h);
        tick();
        CHECK(!ls_lap_peripheral_reserved());
    }
    CHECK(lap_hid_device_report_map(4, data, 128) == LAP_IO_EINVAL);
    ls_lap_peripheral_script("peripherror=-105");
    adv.server = 0;
    adv.connectable = 0;
    h = lap_ble_advertise(&adv);
    tick();
    CHECK(!lap_ble_peripheral_status(h, &s) && s.error == LAP_IO_EIO);
    close_session(h);
    ls_lap_peripheral_script("peripherror=0");
#endif
    printf("BLE peripheral: %u assertions passed\n", checks);
    return 0;
}
