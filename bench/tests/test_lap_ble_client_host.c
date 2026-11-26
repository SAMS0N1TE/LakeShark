/* API 2.9 GATT client: the actual main/ble_lap_client.c over a NimBLE fake,
 * behind the real per-app queue (ls_lap_ble_client.c, simulator off).
 * Pairing on request, passkeys, a held peer Security Request, long reads,
 * notifications and writes, connection info, write without response, and
 * the global SM defaults after a pairing. */
#include "ble_client_host_fake.h"
#include "ls_lap_net_audio.h"
#include <assert.h>
#include <stdio.h>
static unsigned checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        assert(x);                                                                                 \
        checks++;                                                                                  \
    } while (0)
/* ---- the app runtime the queue needs ---- */
atomic_uint ls_lap_av_epochs[LS_LAP_CONTEXTS] = {[0 ... LS_LAP_CONTEXTS - 1] = 1};
atomic_bool ls_lap_av_actives[LS_LAP_CONTEXTS] = {[0 ... LS_LAP_CONTEXTS - 1] = true};
static uint32_t now_ms;
bool ls_lap_api_inside(void) { return true; }
bool ls_lap_api_readable(const void *p, size_t n) { return p && (uintptr_t)p > 4096 && n < 1048576; }
uint32_t ls_lap_sys_caps(void) { return LAP_HW_BLE; }
uint32_t ls_lap_granted(void) { return 0; }
bool ls_lap_peripheral_reserved(void) { return false; }
void ls_lap_av_wake(void) {}
uint32_t ls_lap_av_now(void) { return now_ms; }
int ls_lap_av_access(uint32_t h, bool n) {
    (void)h;
    (void)n;
    return 0;
}
/* ---- the NimBLE fake ---- */
struct fake_hs_cfg ble_hs_cfg = {.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT};
static ble_gap_event_fn *gap_cb;
static void *gap_arg;
static ble_gatt_mtu_fn *mtu_cb;
static void *mtu_arg;
static ble_gatt_attr_fn *read_cb, *write_cb;
static void *read_arg, *write_arg;
static uint16_t mtu = 23, write_len, nr_attr, nr_len, long_len;
static int connects, exchanges, initiates, injects, deletes, rssi_reads, sm_passed, terminates;
static struct fake_hs_cfg cfg_at_initiate;
static struct ble_sm_io injected;
static struct ble_gap_conn_desc desc;
static ble_addr_t deleted;
static int64_t clock_us;
static struct os_mbuf mbuf;
static hid_device_t hid_saved;
static size_t hid_count;
static int fake_sm_rx(struct ble_l2cap_chan *c, struct os_mbuf **om) {
    (void)c;
    (void)om;
    sm_passed++;
    return 0;
}
static struct ble_l2cap_chan sm_chan = {.conn_handle = 7, .scid = BLE_L2CAP_CID_SM, .rx_fn = fake_sm_rx};
int os_mbuf_copydata(const struct os_mbuf *m, int off, int n, void *out) {
    if (off + n > m->len)
        return -1;
    memcpy(out, m->data + off, n);
    return 0;
}
struct os_mbuf *ble_hs_mbuf_from_flat(const void *p, uint16_t n) {
    memcpy(mbuf.data, p, n);
    mbuf.len = n;
    return &mbuf;
}
int ble_gattc_disc_all_svcs(uint16_t c, ble_gatt_disc_svc_fn *f, void *a) { (void)c; (void)f; (void)a; return 0; }
int ble_gattc_disc_all_chrs(uint16_t c, uint16_t s, uint16_t e, ble_gatt_chr_fn *f, void *a) {
    (void)c; (void)s; (void)e; (void)f; (void)a;
    return 0;
}
int ble_gattc_disc_all_dscs(uint16_t c, uint16_t s, uint16_t e, ble_gatt_dsc_fn *f, void *a) {
    (void)c; (void)s; (void)e; (void)f; (void)a;
    return 0;
}
int ble_gattc_read_long(uint16_t c, uint16_t h, uint16_t off, ble_gatt_attr_fn *f, void *a) {
    CHECK(c == 7 && h && !off);
    read_cb = f;
    read_arg = a;
    return 0;
}
int ble_gattc_write_flat(uint16_t c, uint16_t h, const void *p, uint16_t n, ble_gatt_attr_fn *f, void *a) {
    (void)c; (void)h; (void)p;
    write_len = n;
    write_cb = f;
    write_arg = a;
    return 0;
}
int ble_gattc_write_long(uint16_t c, uint16_t h, uint16_t off, struct os_mbuf *om, ble_gatt_attr_fn *f, void *a) {
    (void)c; (void)h; (void)off;
    long_len = om->len;
    write_cb = f;
    write_arg = a;
    return 0;
}
int ble_gattc_write_no_rsp_flat(uint16_t c, uint16_t h, const void *p, uint16_t n) {
    (void)c; (void)p;
    nr_attr = h;
    nr_len = n;
    return 0;
}
int ble_gattc_exchange_mtu(uint16_t c, ble_gatt_mtu_fn *f, void *a) {
    (void)c;
    exchanges++;
    mtu_cb = f;
    mtu_arg = a;
    return 0;
}
uint16_t ble_att_mtu(uint16_t c) { return c == 7 ? mtu : 0; }
int ble_gap_connect(uint8_t own, const ble_addr_t *peer, int32_t ms, const void *p, ble_gap_event_fn *f, void *a) {
    (void)own; (void)ms; (void)p;
    CHECK(peer->val[0] == 0xa1);
    connects++;
    gap_cb = f;
    gap_arg = a;
    return 0;
}
int ble_gap_conn_cancel(void) { return 0; }
int ble_gap_conn_active(void) { return 0; }
int ble_gap_disc_cancel(void) { return 0; }
int ble_gap_terminate(uint16_t c, uint8_t r) { (void)c; (void)r; terminates++; return 0; }
int ble_gap_conn_find(uint16_t c, struct ble_gap_conn_desc *out) {
    if (c != 7)
        return BLE_HS_ENOENT;
    *out = desc;
    return 0;
}
int ble_gap_read_le_phy(uint16_t c, uint8_t *tx, uint8_t *rx) {
    (void)c;
    *tx = 2;
    *rx = 2;
    return 0;
}
int ble_gap_conn_rssi(uint16_t c, int8_t *out) {
    (void)c;
    rssi_reads++;
    *out = -42;
    return 0;
}
int ble_gap_security_initiate(uint16_t c) {
    CHECK(c == 7);
    initiates++;
    cfg_at_initiate = ble_hs_cfg;
    return 0;
}
int ble_sm_inject_io(uint16_t c, struct ble_sm_io *io) {
    CHECK(c == 7);
    injects++;
    injected = *io;
    return 0;
}
int ble_store_util_delete_peer(const ble_addr_t *a) {
    deletes++;
    deleted = *a;
    return 0;
}
size_t heap_caps_get_largest_free_block(unsigned c) { (void)c; return 65536; }
uint32_t esp_random(void) { return 987654321u; }
int64_t esp_timer_get_time(void) { return clock_us; }
struct ble_hs_conn *ble_hs_conn_find(uint16_t c) { return c == 7 ? (struct ble_hs_conn *)&sm_chan : NULL; }
struct ble_l2cap_chan *ble_hs_conn_chan_find_by_scid(struct ble_hs_conn *h, uint16_t cid) {
    return h && cid == BLE_L2CAP_CID_SM ? &sm_chan : NULL;
}
void ble_hs_lock(void) {}
void ble_hs_unlock(void) {}
size_t hid_list_bonds(hid_device_t *out, size_t cap, uint32_t *v) {
    (void)v;
    if (hid_count && cap)
        out[0] = hid_saved;
    return hid_count;
}
/* ---- helpers ---- */
static lap_ble_event_t ev;
static uint8_t big[LAP_GATT_VALUE_MAX], small[LAP_GATT_VALUE_LEGACY];
static void gap_event(struct ble_gap_event *e) { CHECK(gap_cb(e, gap_arg) == 0); }
static int next(void) { return lap_ble_event_next(&ev, big, sizeof(big)); }
static void fragment(const uint8_t *p, uint16_t n) {
    struct os_mbuf m = {.len = n};
    memcpy(m.data, p, n);
    struct ble_gatt_attr a = {.handle = 0x10, .om = &m};
    struct ble_gatt_error ok = {0};
    CHECK(read_cb(7, &ok, &a, read_arg) == 0);
}
static int read_end(int status) {
    struct ble_gatt_error e = {.status = (uint16_t)status};
    return read_cb(7, &e, NULL, read_arg);
}
static const lap_ble_security_t *sec(void) { return (const lap_ble_security_t *)big; }
static void sm_defaults(void) {
    CHECK(ble_hs_cfg.sm_io_cap == BLE_HS_IO_NO_INPUT_OUTPUT && !ble_hs_cfg.sm_bonding && !ble_hs_cfg.sm_mitm &&
          !ble_hs_cfg.sm_sc && !ble_hs_cfg.sm_our_key_dist && !ble_hs_cfg.sm_their_key_dist);
}
static const uint8_t peer[6] = {0xa1, 2, 3, 4, 5, 6};
static int connect_wide(void) {
    CHECK(next() == LAP_IO_EAGAIN); /* a 512-byte reader: a 2.9 app */
    int h = lap_ble_connect(peer, 0);
    CHECK(h > 0);
    ls_lap_ble_host_pump();
    CHECK(connects == 1);
    desc = (struct ble_gap_conn_desc){.conn_itvl = 24, .conn_latency = 2, .supervision_timeout = 500,
                                      .peer_id_addr = {.type = 0, .val = {0xa1, 2, 3, 4, 5, 6}}};
    struct ble_gap_event e = {.type = BLE_GAP_EVENT_CONNECT, .connect = {.status = 0, .conn_handle = 7}};
    gap_event(&e);
    CHECK(exchanges == 1 && sm_chan.rx_fn != fake_sm_rx);
    CHECK(next() == LAP_IO_EAGAIN); /* CONNECTED waits for the MTU exchange */
    lap_ble_conn_info_t info;
    CHECK(lap_ble_conn_info(h, &info) == LAP_IO_EAGAIN);
    mtu = 247;
    struct ble_gatt_error ok = {0};
    mtu_cb(7, &ok, 247, mtu_arg);
    CHECK(next() == 1 && ev.kind == LAP_BLE_CONNECTED && !ev.error);
    CHECK(!lap_ble_conn_info(h, &info) && info.size == sizeof(info) && info.mtu == 247 && info.rssi == 127 &&
          info.phy_tx == 2 && info.phy_rx == 2 && info.interval == 24 && info.latency == 2 && info.timeout == 500 &&
          !info.encrypted && info.peer_addr[0] == 0xa1 && info.peer_addr_type == 0);
    clock_us += 1500000;
    ls_lap_ble_host_pump();
    CHECK(rssi_reads == 1 && !lap_ble_conn_info(h, &info) && info.rssi == -42);
    ls_lap_ble_host_pump();
    CHECK(rssi_reads == 1); /* once a second */
    return h;
}
static void long_values(int h) {
    uint8_t value[500];
    for (int i = 0; i < 500; i++)
        value[i] = (uint8_t)(i * 13 + 1);
    int op = lap_gatt_read(h, 0x10);
    CHECK(op > 0);
    ls_lap_ble_host_pump();
    fragment(value, 246);
    fragment(value + 246, 246);
    fragment(value + 492, 8);
    CHECK(read_end(BLE_HS_EDONE) == 0);
    CHECK(lap_ble_event_next(&ev, big, 100) == LAP_IO_ELIMIT); /* kept for a bigger buffer */
    CHECK(next() == 1 && ev.kind == LAP_GATT_VALUE && ev.operation == op && !ev.error && ev.payload_size == 500 &&
          !memcmp(big, value, 500));
    /* A 20-byte reader (2.8 app) sees a long value as before: ELIMIT, consumed. */
    CHECK(lap_gatt_read(h, 0x10) > 0);
    ls_lap_ble_host_pump();
    fragment(value, 246);
    fragment(value + 246, 30);
    CHECK(read_end(BLE_HS_EDONE) == 0);
    CHECK(lap_ble_event_next(&ev, small, sizeof(small)) == 1 && ev.kind == LAP_GATT_VALUE &&
          ev.error == LAP_IO_ELIMIT && !ev.payload_size);
    CHECK(lap_ble_event_next(&ev, small, sizeof(small)) == LAP_IO_EAGAIN);
    /* A value ending on a piece boundary: the peer says "not long". */
    CHECK(lap_gatt_read(h, 0x10) > 0);
    ls_lap_ble_host_pump();
    fragment(value, 246);
    CHECK(read_end(BLE_HS_ATT_ERR(BLE_ATT_ERR_ATTR_NOT_LONG)) == 0);
    CHECK(next() == 1 && !ev.error && ev.payload_size == 246 && !memcmp(big, value, 246));
    /* Over LAP_GATT_VALUE_MAX: stopped, ELIMIT. */
    CHECK(lap_gatt_read(h, 0x10) > 0);
    ls_lap_ble_host_pump();
    fragment(value, 246);
    fragment(value, 246);
    struct os_mbuf m = {.len = 246};
    struct ble_gatt_attr a = {.handle = 0x10, .om = &m};
    struct ble_gatt_error ok = {0};
    CHECK(read_cb(7, &ok, &a, read_arg) != 0);
    CHECK(next() == 1 && ev.kind == LAP_GATT_VALUE && ev.error == LAP_IO_ELIMIT);
    /* Long notification (mtu - 3) arrives whole. */
    struct ble_gap_event e = {.type = BLE_GAP_EVENT_NOTIFY_RX};
    struct os_mbuf note = {.len = 244};
    memcpy(note.data, value, 244);
    e.notify_rx.om = &note;
    e.notify_rx.conn_handle = 7;
    e.notify_rx.attr_handle = 0x12;
    gap_event(&e);
    CHECK(next() == 1 && ev.kind == LAP_GATT_NOTIFY && ev.attribute == 0x12 && ev.payload_size == 244 &&
          !memcmp(big, value, 244));
    /* Writes: short goes flat, longer than mtu - 3 goes long. */
    CHECK(lap_gatt_write(h, 0x12, value, 244) > 0);
    ls_lap_ble_host_pump();
    CHECK(write_len == 244);
    struct ble_gatt_error done = {0};
    write_cb(7, &done, NULL, write_arg);
    CHECK(next() == 1 && ev.kind == LAP_GATT_WRITTEN && !ev.error);
    CHECK(lap_gatt_write(h, 0x12, value, 300) > 0);
    ls_lap_ble_host_pump();
    CHECK(long_len == 300 && !memcmp(mbuf.data, value, 300));
    write_cb(7, &done, NULL, write_arg);
    CHECK(next() == 1 && ev.kind == LAP_GATT_WRITTEN && !ev.error);
    CHECK(lap_gatt_write(h, 0x12, value, LAP_GATT_VALUE_MAX + 1) == LAP_IO_ELIMIT);
}
static void write_no_response(int h) {
    uint8_t data[300] = {1, 2, 3};
    int op = lap_gatt_write_cmd(h, 0x14, data, 100);
    CHECK(op > 0);
    ls_lap_ble_host_pump();
    CHECK(nr_attr == 0x14 && nr_len == 100);
    CHECK(next() == 1 && ev.kind == LAP_GATT_WRITTEN && ev.operation == op && !ev.error); /* no peer reply needed */
    nr_len = 0;
    CHECK(lap_gatt_write_cmd(h, 0x14, data, 245) > 0); /* mtu 247: 244 at most */
    ls_lap_ble_host_pump();
    CHECK(!nr_len && next() == 1 && ev.kind == LAP_GATT_WRITTEN && ev.error == LAP_IO_ELIMIT);
    CHECK(lap_gatt_write_cmd(h, 0, data, 1) == LAP_IO_EINVAL);
}
static void pairing(int h) {
    /* A read refused for missing authentication says EAUTH. */
    CHECK(lap_gatt_read(h, 0x20) > 0);
    ls_lap_ble_host_pump();
    CHECK(read_end(BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN)) == 0);
    CHECK(next() == 1 && ev.kind == LAP_GATT_VALUE && ev.error == LAP_IO_EAUTH);
    CHECK(lap_ble_pair(h, 0x80) == LAP_IO_EINVAL);
    CHECK(lap_ble_passkey(h, LAP_BLE_PASSKEY_NUMCMP, 1) == LAP_IO_EINVAL); /* nothing to answer */
    int op = lap_ble_pair(h, LAP_BLE_PAIR_BOND | LAP_BLE_PAIR_MITM);
    CHECK(op > 0);
    CHECK(lap_gatt_read(h, 0x20) == LAP_IO_EBUSY);
    sm_defaults();
    ls_lap_ble_host_pump();
    CHECK(initiates == 1);
    CHECK(cfg_at_initiate.sm_io_cap == BLE_HS_IO_KEYBOARD_DISPLAY && cfg_at_initiate.sm_bonding &&
          cfg_at_initiate.sm_mitm && cfg_at_initiate.sm_sc &&
          cfg_at_initiate.sm_our_key_dist == (BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID));
    sm_defaults(); /* back before anything else runs */
    struct ble_gap_event e = {.type = BLE_GAP_EVENT_PASSKEY_ACTION};
    e.passkey.conn_handle = 7;
    e.passkey.params.action = BLE_SM_IOACT_NUMCMP;
    e.passkey.params.numcmp = 654321;
    gap_event(&e);
    CHECK(next() == 1 && ev.kind == LAP_BLE_PASSKEY && ev.operation == op && sec()->action == LAP_BLE_PASSKEY_NUMCMP &&
          sec()->passkey == 654321);
    CHECK(lap_ble_passkey(h, LAP_BLE_PASSKEY_NUMCMP, 2) == LAP_IO_EINVAL);
    CHECK(lap_ble_passkey(h, LAP_BLE_PASSKEY_NUMCMP, 1) == 0);
    ls_lap_ble_host_pump();
    CHECK(injects == 1 && injected.action == BLE_SM_IOACT_NUMCMP && injected.numcmp_accept == 1);
    desc.sec_state.encrypted = desc.sec_state.authenticated = desc.sec_state.bonded = 1;
    desc.sec_state.key_size = 16;
    e = (struct ble_gap_event){.type = BLE_GAP_EVENT_ENC_CHANGE, .enc_change = {.status = 0, .conn_handle = 7}};
    gap_event(&e);
    CHECK(next() == 1 && ev.kind == LAP_BLE_SECURITY && ev.operation == op && !ev.error && sec()->encrypted &&
          sec()->authenticated && sec()->bonded && sec()->key_size == 16);
    sm_defaults();
    lap_ble_conn_info_t info;
    CHECK(!lap_ble_conn_info(h, &info) && info.encrypted && info.authenticated && info.bonded && info.key_size == 16);
    /* The refused read now works. */
    CHECK(lap_gatt_read(h, 0x20) > 0);
    ls_lap_ble_host_pump();
    uint8_t v[3] = {9, 8, 7};
    fragment(v, 3);
    CHECK(read_end(BLE_HS_EDONE) == 0);
    CHECK(next() == 1 && ev.kind == LAP_GATT_VALUE && !ev.error && ev.payload_size == 3 && big[0] == 9);
    /* Already as asked: answered at once, no new pairing. */
    CHECK((op = lap_ble_pair(h, LAP_BLE_PAIR_MITM)) > 0);
    ls_lap_ble_host_pump();
    CHECK(initiates == 1 && next() == 1 && ev.kind == LAP_BLE_SECURITY && ev.operation == op && !ev.error);
    /* Passkey entry: the app types the peer's code. A failure says EAUTH. */
    desc.sec_state = (struct ble_gap_sec_state){0};
    CHECK((op = lap_ble_pair(h, LAP_BLE_PAIR_MITM)) > 0);
    ls_lap_ble_host_pump();
    CHECK(initiates == 2 && !cfg_at_initiate.sm_bonding && !cfg_at_initiate.sm_our_key_dist);
    sm_defaults();
    e = (struct ble_gap_event){.type = BLE_GAP_EVENT_PASSKEY_ACTION};
    e.passkey.conn_handle = 7;
    e.passkey.params.action = BLE_SM_IOACT_INPUT;
    gap_event(&e);
    CHECK(next() == 1 && ev.kind == LAP_BLE_PASSKEY && sec()->action == LAP_BLE_PASSKEY_INPUT);
    CHECK(lap_ble_passkey(h, LAP_BLE_PASSKEY_INPUT, 1000000) == LAP_IO_EINVAL);
    CHECK(lap_ble_passkey(h, LAP_BLE_PASSKEY_INPUT, 123456) == 0);
    ls_lap_ble_host_pump();
    CHECK(injects == 2 && injected.action == BLE_SM_IOACT_INPUT && injected.passkey == 123456);
    e = (struct ble_gap_event){.type = BLE_GAP_EVENT_ENC_CHANGE, .enc_change = {.status = 0x504, .conn_handle = 7}};
    gap_event(&e);
    CHECK(next() == 1 && ev.kind == LAP_BLE_SECURITY && ev.operation == op && ev.error == LAP_IO_EAUTH &&
          !sec()->encrypted);
    /* Display: the board makes the code and shows it through the app. */
    CHECK(lap_ble_pair(h, LAP_BLE_PAIR_MITM) > 0);
    ls_lap_ble_host_pump();
    e = (struct ble_gap_event){.type = BLE_GAP_EVENT_PASSKEY_ACTION};
    e.passkey.conn_handle = 7;
    e.passkey.params.action = BLE_SM_IOACT_DISP;
    gap_event(&e);
    CHECK(injects == 3 && injected.action == BLE_SM_IOACT_DISP && injected.passkey < 1000000);
    CHECK(next() == 1 && ev.kind == LAP_BLE_PASSKEY && sec()->action == LAP_BLE_PASSKEY_DISPLAY &&
          sec()->passkey == injected.passkey);
    e = (struct ble_gap_event){.type = BLE_GAP_EVENT_ENC_CHANGE, .enc_change = {.status = BLE_HS_ETIMEOUT, .conn_handle = 7}};
    gap_event(&e);
    CHECK(next() == 1 && ev.kind == LAP_BLE_SECURITY && ev.error == LAP_IO_ETIMEOUT);
    sm_defaults();
}
static void security_request(int h) {
    (void)h;
    int before = initiates;
    struct os_mbuf m = {.data = {0x0b, 0x0d}, .len = 2}, *om = &m;
    CHECK(sm_chan.rx_fn(&sm_chan, &om) == 0);
    CHECK(sm_passed == 0 && initiates == before); /* held: never paired on the peer's word */
    CHECK(next() == 1 && ev.kind == LAP_BLE_SECURITY_REQUEST && !ev.operation && sec()->authreq == 0x0d);
    m = (struct os_mbuf){.data = {0x01, 0x04}, .len = 2};
    CHECK(sm_chan.rx_fn(&sm_chan, &om) == 0 && sm_passed == 1); /* other SM PDUs pass */
    CHECK(next() == LAP_IO_EAGAIN);
    sm_defaults();
}
static void forget(int h) {
    hid_count = 1;
    memcpy(hid_saved.address, peer, 6);
    hid_saved.address_type = 0;
    int op = lap_ble_forget(h);
    CHECK(op > 0);
    ls_lap_ble_host_pump();
    CHECK(!deletes && next() == 1 && ev.kind == LAP_GATT_COMPLETE && ev.operation == op && ev.error == LAP_IO_EACCES);
    hid_count = 0;
    CHECK(lap_ble_forget(h) > 0);
    ls_lap_ble_host_pump();
    CHECK(deletes == 1 && deleted.val[0] == 0xa1 && next() == 1 && ev.kind == LAP_GATT_COMPLETE && !ev.error);
}
static void old_app(int h) {
    /* The link drops; a 2.8 app (20-byte buffer) connects: no MTU exchange. */
    struct ble_gap_event e = {.type = BLE_GAP_EVENT_DISCONNECT};
    gap_event(&e);
    CHECK(next() == 1 && ev.kind == LAP_BLE_DISCONNECTED);
    lap_ble_conn_info_t info;
    CHECK(lap_ble_conn_info(h, &info) == LAP_IO_EAGAIN);
    lap_ble_disconnect(h);
    ls_lap_ble_service();
    ls_lap_ble_host_pump();
    ls_lap_ble_service();
    atomic_fetch_add(&ls_lap_av_epochs[0], 1);
    ls_lap_ble_service();
    CHECK(lap_ble_event_next(&ev, small, sizeof(small)) == LAP_IO_EAGAIN);
    mtu = 23;
    int h2 = lap_ble_connect(peer, 0);
    CHECK(h2 > 0);
    ls_lap_ble_host_pump();
    CHECK(connects == 2);
    e = (struct ble_gap_event){.type = BLE_GAP_EVENT_CONNECT, .connect = {.status = 0, .conn_handle = 7}};
    gap_event(&e);
    CHECK(exchanges == 1);
    CHECK(lap_ble_event_next(&ev, small, sizeof(small)) == 1 && ev.kind == LAP_BLE_CONNECTED);
    CHECK(!lap_ble_conn_info(h2, &info) && info.mtu == 23);
}
int main(void) {
    sm_defaults();
    int h = connect_wide();
    long_values(h);
    write_no_response(h);
    pairing(h);
    security_request(h);
    forget(h);
    old_app(h);
    sm_defaults();
    printf("API 2.9 GATT client host: %u assertions passed\n", checks);
    return 0;
}
