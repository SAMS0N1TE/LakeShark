/* API 2.9 GATT client in the simulator (what lssim runs): long values,
 * pairing with a number check, a peer's Security Request, connection info
 * and write without response. The board side is test_lap_ble_client_host. */
#include "ls_lap_net_audio.h"
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir_one(p) _mkdir(p)
#else
#include <sys/stat.h>
#define mkdir_one(p) mkdir(p, 0777)
#endif
static uint32_t now;
static unsigned checks;
#define CHECK(e)                                                                                             \
    do {                                                                                                     \
        assert(e);                                                                                           \
        checks++;                                                                                            \
    } while (0)
static uint32_t clock_now(void) { return now; }
uint32_t (*ls_lap_api_host_clock)(void) = clock_now;
bool ls_lap_api_inside(void) { return true; }
bool ls_lap_api_readable(const void *p, size_t n) {
    return p && (uintptr_t)p > 4096 && n < 1048576 && (uintptr_t)p + n >= (uintptr_t)p;
}
bool ls_lap_net_allowed(void) { return true; }
uint32_t ls_lap_sys_caps(void) { return LAP_HW_WIFI | LAP_HW_BLE | LAP_HW_SPEAKER | LAP_HW_MIC | LAP_HW_SD; }
uint32_t ls_lap_granted(void) { return LAP_CAP_MIC; }
bool ls_lap_api_data_path(char *out, size_t cap, const char *name) {
    if (!name || !*name || strchr(name, '/') || strchr(name, '\\') || strstr(name, "..") || strlen(name) > 48)
        return false;
    snprintf(out, cap, "ble29-fixture/%s", name);
    return true;
}
static void tick(unsigned ms) {
    now += ms;
    ls_lap_av_service();
}
static lap_ble_event_t ev;
static uint8_t big[LAP_GATT_VALUE_MAX], small[LAP_GATT_VALUE_LEGACY];
static int next(void) { return lap_ble_event_next(&ev, big, sizeof(big)); }
static const lap_ble_security_t *sec(void) { return (const lap_ble_security_t *)big; }
int main(void) {
    mkdir_one("ble29-fixture");
    ls_lap_av_begin();
    tick(1);
    ls_lap_av_script("blesilence=1");
    uint8_t addr[6] = {0xc1, 2, 3, 4, 5, 6};
    CHECK(next() == LAP_IO_EAGAIN); /* a 512-byte reader: the MTU is exchanged */
    int h = lap_ble_connect(addr, 1);
    CHECK(h > 0);
    lap_ble_conn_info_t info;
    CHECK(lap_ble_conn_info(h, &info) == LAP_IO_EAGAIN);
    CHECK(lap_ble_conn_info(h, NULL) == LAP_IO_EINVAL);
    tick(40);
    CHECK(next() == 1 && ev.kind == LAP_BLE_CONNECTED);
    CHECK(!lap_ble_conn_info(h, &info) && info.size == sizeof(info) && info.version == 1 && info.mtu == 247 &&
          info.rssi < 0 && info.phy_tx == 1 && info.peer_addr_type == 1 && info.peer_addr[0] == 0xc1 &&
          info.interval && info.timeout && !info.encrypted);
    CHECK(lap_ble_conn_info(INT_MAX, &info) == LAP_IO_ECLOSED);
    /* Long read whole; a small buffer first gets ELIMIT and the event stays. */
    ls_lap_av_script("blereadlen=400");
    int op = lap_gatt_read(h, 3);
    CHECK(op > 0);
    tick(40);
    CHECK(lap_ble_event_next(&ev, big, 399) == LAP_IO_ELIMIT);
    CHECK(next() == 1 && ev.kind == LAP_GATT_VALUE && ev.operation == op && ev.payload_size == 400 &&
          big[1] == 7 && big[399] == (uint8_t)(399 * 7));
    /* A 2.8-style 20-byte reader: the long value is ELIMIT and consumed. */
    CHECK(lap_gatt_read(h, 3) > 0);
    tick(40);
    CHECK(lap_ble_event_next(&ev, small, sizeof(small)) == 1 && ev.kind == LAP_GATT_VALUE &&
          ev.error == LAP_IO_ELIMIT && !ev.payload_size);
    /* Long notification passes through the queue intact. */
    ls_lap_av_script("blenotifylen=244");
    CHECK(next() == 1 && ev.kind == LAP_GATT_NOTIFY && ev.payload_size == 244 && big[0] == 0 && big[243] == 243);
    /* Write without response: WRITTEN at once; over mtu - 3 is ELIMIT. */
    uint8_t data[300] = {0};
    CHECK((op = lap_gatt_write_cmd(h, 5, data, 244)) > 0);
    tick(1);
    CHECK(next() == 1 && ev.kind == LAP_GATT_WRITTEN && ev.operation == op && !ev.error);
    CHECK(lap_gatt_write_cmd(h, 5, data, 245) > 0);
    tick(1);
    CHECK(next() == 1 && ev.kind == LAP_GATT_WRITTEN && ev.error == LAP_IO_ELIMIT);
    CHECK(lap_gatt_write(h, 5, data, 300) > 0); /* long write */
    tick(1);
    CHECK(next() == 1 && ev.kind == LAP_GATT_WRITTEN && !ev.error);
    /* The peer asks to pair: an event, nothing else. */
    ls_lap_av_script("blesecreq=13");
    CHECK(next() == 1 && ev.kind == LAP_BLE_SECURITY_REQUEST && sec()->authreq == 13 && !sec()->encrypted);
    CHECK(!lap_ble_conn_info(h, &info) && !info.encrypted);
    /* A read refused for pairing; pair with a number check; the retry works. */
    ls_lap_av_script("bleauth=1");
    ls_lap_av_script("blereadlen=3");
    CHECK(lap_gatt_read(h, 3) > 0);
    tick(40);
    CHECK(next() == 1 && ev.kind == LAP_GATT_VALUE && ev.error == LAP_IO_EAUTH);
    CHECK(lap_ble_pair(h, 2) == LAP_IO_EINVAL);
    CHECK(lap_ble_passkey(h, LAP_BLE_PASSKEY_NUMCMP, 1) == LAP_IO_EINVAL);
    CHECK((op = lap_ble_pair(h, LAP_BLE_PAIR_BOND | LAP_BLE_PAIR_MITM)) > 0);
    CHECK(lap_gatt_read(h, 3) == LAP_IO_EBUSY);
    tick(40);
    CHECK(next() == 1 && ev.kind == LAP_BLE_PASSKEY && ev.operation == op &&
          sec()->action == LAP_BLE_PASSKEY_NUMCMP && sec()->passkey == 123456);
    now += 20000; /* the user takes their time: pairing waits up to 40 s */
    CHECK(lap_ble_passkey(h, LAP_BLE_PASSKEY_NUMCMP, 1) == 0);
    tick(1);
    CHECK(next() == 1 && ev.kind == LAP_BLE_SECURITY && ev.operation == op && !ev.error && sec()->encrypted &&
          sec()->authenticated && sec()->bonded && sec()->key_size == 16);
    CHECK(!lap_ble_conn_info(h, &info) && info.encrypted && info.authenticated && info.bonded);
    CHECK(lap_gatt_read(h, 3) > 0);
    tick(40);
    CHECK(next() == 1 && ev.kind == LAP_GATT_VALUE && !ev.error && ev.payload_size == 3);
    /* Forget the bond. */
    CHECK((op = lap_ble_forget(h)) > 0);
    tick(1);
    CHECK(next() == 1 && ev.kind == LAP_GATT_COMPLETE && ev.operation == op && !ev.error);
    CHECK(!lap_ble_conn_info(h, &info) && !info.bonded);
    lap_ble_disconnect(h);
    tick(1);
    CHECK(lap_ble_conn_info(h, &info) == LAP_IO_ECLOSED);
    CHECK(lap_ble_pair(h, 0) == LAP_IO_ECLOSED);
    ls_lap_av_end();
    tick(1);
    printf("API 2.9 GATT client (simulator): %u assertions passed\n", checks);
    return 0;
}
