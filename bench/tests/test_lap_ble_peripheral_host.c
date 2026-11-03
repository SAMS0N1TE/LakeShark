/* Runs actual main/ adapter, including real DB construction and GAP callbacks.
 */
#include "ble_peripheral_host_fake.h"
#include "ls_lap_ble_peripheral.h"
#include <assert.h>
#include <stdio.h>
static unsigned checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        assert(x);                                                                                 \
        checks++;                                                                                  \
    } while (0)
struct fake_ble_config ble_hs_cfg = {3, 0, 0, 0};
static size_t free_ram = 40000;
static int db_error, send_error, terminate_calls, mbufs, power_delta;
static uint8_t last_report[20];
static unsigned last_report_size, configured_services;
static bool is_connected, advertising, reset_busy;
static ble_gap_event_fn *gap_cb;
static void *gap_arg;
static const struct ble_gatt_svc_def *registered;
atomic_uint ls_lap_av_epochs[LS_LAP_CONTEXTS] = { [0 ... LS_LAP_CONTEXTS-1] = 1 };
atomic_bool ls_lap_av_actives[LS_LAP_CONTEXTS] = { [0 ... LS_LAP_CONTEXTS-1] = true };
static bool live_context = true;
bool ls_lap_api_inside(void) { return true; }
bool ls_lap_api_readable(const void *d, size_t n) { return (uintptr_t)d > 4096 && n < 8192; }
uint32_t ls_lap_sys_caps(void) { return LAP_HW_BLE; }
uint32_t ls_lap_granted(void) { return LAP_CAP_RADIO_TX | LAP_CAP_HID_DEVICE; }
bool ls_lap_ble_in_use(void) { return false; }
void ls_lap_av_wake(void) {}
int ls_lap_av_access(uint32_t h, bool n) {
    (void)h;
    (void)n;
    return live_context ? 0 : LAP_IO_EINVAL;
}
bool ls_lap_av_string(char *o, size_t n, const char *s) {
    if (!s || strlen(s) >= n)
        return false;
    strcpy(o, s);
    return true;
}
int ls_lap_peripheral_lease(bool on) {
    (void)on;
    return 0;
}
int ble_hs_synced(void) { return 1; }
size_t heap_caps_get_largest_free_block(unsigned c) {
    (void)c;
    return free_ram;
}
size_t heap_caps_get_free_size(unsigned c) {
    (void)c;
    return free_ram;
}
int ble_gatts_reset(void) {
    CHECK(!is_connected && !advertising);
    if (reset_busy)
        return 1;
    registered = NULL;
    return 0;
}
static const ble_uuid_any_t gap_uuid = {.u16 = {.u = {BLE_UUID_TYPE_16}, .value = 0x1800}};
static const struct ble_gatt_chr_def baseline_chars[1];
static const struct ble_gatt_svc_def baseline_service = {
    .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &gap_uuid.u, .characteristics = baseline_chars};
void ble_gatts_lcl_svc_foreach(ble_gatt_svc_foreach_fn cb, void *arg) {
    cb(&baseline_service, 1, 5, arg);
}
void ble_gatts_stop(void) {
    configured_services = 0;
    registered = NULL;
}
int ble_gatts_start(void) {
    CHECK(configured_services <= 5);
    return 0;
}
int ble_gatts_count_cfg(const struct ble_gatt_svc_def *s) {
    if (s->type && ((const ble_uuid_any_t *)s->uuid)->u16.value != 0x1800 && db_error)
        return db_error;
    for (; s->type; s++)
        configured_services++;
    return 0;
}
int ble_gatts_add_svcs(const struct ble_gatt_svc_def *s) {
    if (s->type && ((const ble_uuid_any_t *)s->uuid)->u16.value != 0x1800)
        registered = s;
    uint16_t v = 10;
    unsigned n = 0;
    for (; s->type; s++) {
        CHECK(s->uuid != NULL);
        const struct ble_gatt_chr_def *c = s->characteristics;
        for (; c->uuid; c++) {
            CHECK(++n < 17);
            *c->val_handle = v++;
            unsigned nd = 0;
            if (c->descriptors)
                for (const struct ble_gatt_dsc_def *d = c->descriptors; d->uuid; d++) {
                    CHECK(++nd < 17);
                    CHECK(d->access_cb != NULL);
                }
        }
    }
    return 0;
}
int ble_gap_ext_adv_active(uint8_t i) {
    (void)i;
    return advertising;
}
int ble_gap_ext_adv_stop(uint8_t i) {
    (void)i;
    advertising = false;
    return 0;
}
int ble_gap_ext_adv_remove(uint8_t i) {
    (void)i;
    return 0;
}
int ble_gap_ext_adv_configure(uint8_t i, const struct ble_gap_ext_adv_params *p, int8_t *power,
                              ble_gap_event_fn *cb, void *arg) {
    (void)i;
    CHECK(p->legacy_pdu);
    CHECK(!p->connectable || p->scannable);
    *power = p->tx_power + power_delta;
    gap_cb = cb;
    gap_arg = arg;
    return 0;
}
static struct os_mbuf mbuf;
struct os_mbuf *ble_hs_mbuf_from_flat(const void *d, uint16_t n) {
    CHECK(!mbufs);
    mbufs++;
    memcpy(mbuf.data, d, n);
    mbuf.len = n;
    return &mbuf;
}
int os_mbuf_copydata(const struct os_mbuf *m, int off, int n, void *d) {
    memcpy(d, m->data + off, n);
    return 0;
}
int os_mbuf_append(struct os_mbuf *m, const void *d, uint16_t n) {
    memcpy(m->data + m->len, d, n);
    m->len += n;
    return 0;
}
int ble_gap_ext_adv_set_data(uint8_t i, struct os_mbuf *m) {
    (void)i;
    (void)m;
    mbufs--;
    return 0;
}
int ble_gap_ext_adv_rsp_set_data(uint8_t i, struct os_mbuf *m) {
    return ble_gap_ext_adv_set_data(i, m);
}
int ble_gap_ext_adv_start(uint8_t i, int d, int n) {
    (void)i;
    (void)d;
    (void)n;
    advertising = true;
    return 0;
}
int ble_gap_terminate(uint16_t c, uint8_t r) {
    (void)c;
    (void)r;
    terminate_calls++;
    return 0;
}
int ble_gap_conn_find(uint16_t c, struct ble_gap_conn_desc *d) {
    (void)c;
    memset(d, 1, sizeof(*d));
    return 0;
}
int ble_gatts_indicate_custom(uint16_t c, uint16_t a, struct os_mbuf *m) {
    (void)c;
    (void)a;
    last_report_size = m->len;
    memcpy(last_report, m->data, m->len);
    mbufs--;
    return send_error;
}
int ble_gatts_notify_custom(uint16_t c, uint16_t a, struct os_mbuf *m) {
    return ble_gatts_indicate_custom(c, a, m);
}
static void tick(void) {
    for (int i = 0; i < 6; i++) {
        ls_lap_peripheral_service();
        ls_lap_peripheral_host_pump();
    }
}
static void gap(struct ble_gap_event e) {
    CHECK(gap_cb != NULL);
    gap_cb(&e, gap_arg);
    tick();
}
static void disconnect(void) {
    is_connected = false;
    gap((struct ble_gap_event){.type = BLE_GAP_EVENT_DISCONNECT});
}
static void close_h(int h) {
    lap_gatt_server_close(h);
    tick();
    if (is_connected)
        disconnect();
    tick();
    CHECK(!ls_lap_peripheral_reserved());
    CHECK(!ble_hs_cfg.sm_bonding);
    CHECK(!registered);
}
#if CONFIG_LS_LAP_GATT_SERVER
static void drain(int h) {
    lap_gatt_server_event_t e;
    uint8_t d[128];
    while (lap_gatt_server_next(h, &e, d, 128) == 1) {
    }
}
#endif
int main(void) {
#if !CONFIG_LS_LAP_GATT_SERVER
    CHECK(ls_lap_peripheral_caps() == LAP_HW_BLE_ADVERTISER);
    CHECK(lap_hid_device_open(3, "No HID") == LAP_IO_ENOTSUP);
    CHECK(lap_gatt_server_open(NULL, 0) == LAP_IO_ENOTSUP);
    lap_ble_advert_config_t adv = {.size = sizeof(adv),
                                   .version = 1,
                                   .interval_min_ms = 100,
                                   .interval_max_ms = 100,
                                   .data_len = 3,
                                   .data = {2, 1, 6}};
    int h = lap_ble_advertise(&adv);
    CHECK(h > 0);
    tick();
    lap_ble_peripheral_status_t s;
    CHECK(!lap_ble_peripheral_status(h, &s) && s.advertising);
    close_h(h);
    CHECK(!mbufs);
    printf("BLE advertiser-only build: %u assertions passed\n", checks);
    return 0;
#else
    int h = lap_hid_device_open(LAP_HID_KEYBOARD, "LakeShark");
    CHECK(h > 0);
    tick();
    lap_ble_peripheral_status_t s;
    CHECK(!lap_ble_peripheral_status(h, &s) && s.advertising);
    CHECK(ble_hs_cfg.sm_bonding);
    CHECK(registered && registered->characteristics[0].uuid);
    CHECK(registered->characteristics[4].descriptors);
    is_connected = true;
    advertising = false;
    gap((struct ble_gap_event){.type = BLE_GAP_EVENT_CONNECT, .connect = {0, 7}});
    CHECK(!lap_ble_peripheral_status(h, &s) && s.connected);
    gap((struct ble_gap_event){.type = BLE_GAP_EVENT_ENC_CHANGE});
    gap((struct ble_gap_event){.type = BLE_GAP_EVENT_SUBSCRIBE, .subscribe = {14, 1, 0}});
    CHECK(!lap_ble_peripheral_status(h, &s) && s.bonded && s.encrypted);
    drain(h);
    uint8_t report[8] = {2, 0, 0x2c};
    CHECK(!lap_hid_device_send(h, report, 8));
    tick();
    lap_gatt_server_event_t e;
    uint8_t data[128];
    CHECK(lap_gatt_server_next(h, &e, data, 128) == 1 && e.kind == LAP_PERIPHERAL_SENT && !e.error);
    CHECK(last_report_size == 8 && last_report[0] == 2 && last_report[2] == 0x2c);
    ls_lap_peripheral_release_inputs();
    tick();
    CHECK(last_report_size == 8);
    for (int i = 0; i < 8; i++)
        CHECK(last_report[i] == 0);
    struct os_mbuf val = {0};
    struct ble_gatt_access_ctxt ctxt = {.op = BLE_GATT_ACCESS_OP_READ_CHR, .om = &val};
    const struct ble_gatt_chr_def *map = &registered->characteristics[1];
    CHECK(!map->access_cb(7, 0, &ctxt, map->arg) && val.len > 40);
    close_h(h);
    CHECK(terminate_calls == 1);
    /* Cancellation before host consumes OPEN must still acknowledge that handle.
     */
    h = lap_hid_device_open(3, "Fast close");
    CHECK(h > 0);
    lap_hid_device_close(h);
    tick();
    CHECK(!ls_lap_peripheral_reserved());
    /* Failed registration restores baseline and remains closable. */
    db_error = 6;
    h = lap_hid_device_open(3, "No DB");
    tick();
    CHECK(!lap_ble_peripheral_status(h, &s) && s.error == LAP_IO_ENOSPC);
    close_h(h);
    db_error = 0;
    free_ram = 5000;
    h = lap_hid_device_open(3, "No RAM");
    tick();
    CHECK(!lap_ble_peripheral_status(h, &s) && s.error == LAP_IO_ENOSPC);
    close_h(h);
    free_ram = 40000;
    /* Controller may select a lower power but never a higher one. */
    power_delta = 3;
    h = lap_hid_device_open(3, "Power");
    tick();
    CHECK(!lap_ble_peripheral_status(h, &s) && s.error == LAP_IO_EPOWER && !s.advertising);
    close_h(h);
    power_delta = 0;
    /* Cleanup waits for DB reset acknowledgement before releasing its lease. */
    h = lap_hid_device_open(3, "Reset");
    tick();
    reset_busy = true;
    lap_hid_device_close(h);
    tick();
    CHECK(ls_lap_peripheral_reserved());
    reset_busy = false;
    tick();
    CHECK(!ls_lap_peripheral_reserved());
    /* A pending indication cannot delay unload until the ATT timeout. */
    lap_gatt_attribute_t attributes[2] = {{.size = sizeof(lap_gatt_attribute_t),
                                           .version = 1,
                                           .id = 1,
                                           .kind = LAP_GATTS_SERVICE,
                                           .uuid = {.length = 2, .bytes = {0x0f, 0x18}}},
                                          {.size = sizeof(lap_gatt_attribute_t),
                                           .version = 1,
                                           .id = 2,
                                           .parent = 1,
                                           .kind = LAP_GATTS_CHARACTERISTIC,
                                           .flags = LAP_GATTS_READ | LAP_GATTS_INDICATE,
                                           .uuid = {.length = 2, .bytes = {0x19, 0x2a}}}};
    h = lap_gatt_server_open(attributes, 2);
    CHECK(h > 0);
    tick();
    lap_ble_advert_config_t adv = {.size = sizeof(adv),
                                   .version = 1,
                                   .server = h,
                                   .connectable = 1,
                                   .interval_min_ms = 100,
                                   .interval_max_ms = 100,
                                   .data_len = 3,
                                   .data = {2, 1, 6}};
    CHECK(lap_ble_advertise(&adv) == h);
    tick();
    is_connected = true;
    advertising = false;
    gap((struct ble_gap_event){.type = BLE_GAP_EVENT_CONNECT, .connect = {0, 7}});
    gap((struct ble_gap_event){.type = BLE_GAP_EVENT_SUBSCRIBE, .subscribe = {10, 0, 1}});
    drain(h);
    CHECK(!lap_gatt_notify(h, 2, "x", 1, true));
    tick();
    CHECK(lap_gatt_notify(h, 2, "y", 1, true) == LAP_IO_EBUSY);
    gap((struct ble_gap_event){.type = BLE_GAP_EVENT_NOTIFY_TX,
                               .notify_tx = {10, 1, BLE_HS_EDONE}});
    CHECK(!lap_gatt_notify(h, 2, "z", 1, true));
    tick();
    int before = terminate_calls;
    lap_gatt_server_close(h);
    tick();
    CHECK(terminate_calls == before + 1);
    disconnect();
    tick();
    CHECK(!ls_lap_peripheral_reserved());
    for (int i = 0; i < 32; i++) {
        h = lap_hid_device_open(3, "Repeated open");
        CHECK(h > 0);
        tick();
        CHECK(!lap_ble_peripheral_status(h, &s) && !s.error && s.advertising);
        close_h(h);
        CHECK(configured_services == 1);
    }
    CHECK(!mbufs);
    printf("BLE peripheral firmware adapter: %u assertions passed\n", checks);
    return 0;
#endif
}
