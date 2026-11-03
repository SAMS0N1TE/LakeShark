/* API 2.6 Bluetooth peripheral roles. Copied state in PSRAM, try-only lock,
 * monotonically tagged sessions, close acknowledgement before lease release. */
#include "ls_lap_ble_peripheral.h"
#include "ls_lap_radio.h"
#include <stdlib.h>
#if LAP_PERIPH_ADV
static atomic_flag lock = ATOMIC_FLAG_INIT;
static atomic_int published, closing;
static atomic_uint drops, owner_epoch, owner_slot;
static atomic_bool release_inputs, resource_reserved;
static unsigned serial;
static EXT_RAM_BSS_ATTR struct {
    int handle, hid, count, state, error;
    unsigned epoch, head, tail;
    bool resource, lease, opened, ready, advert_pending, stop_pending, send_pending, close_sent;
    bool advertising, connected, bonded, encrypted, authenticated;
    uint16_t mtu;
    int8_t power;
    uint8_t subscribed[LAP_BLE_SERVER_ATTRS];
    lap_gatt_attribute_t attrs[LAP_BLE_SERVER_ATTRS];
    lap_ble_advert_config_t advert;
    struct {
        int kind, handle;
        uint16_t id;
        uint8_t len, indicate, data[20];
    } send;
    struct {
        lap_gatt_server_event_t e;
        uint8_t data[128];
    } events[8];
} p;
/* HID maps use one input report ID 1. Keyboard also has output ID 1. */
static const uint8_t keyboard[] = {
    0x05, 1, 0x09, 6, 0xa1, 1,    0x85, 1, 0x05, 7,    0x19, 0xe0, 0x29, 0xe7, 0x15, 0,
    0x25, 1, 0x75, 1, 0x95, 8,    0x81, 2, 0x95, 1,    0x75, 8,    0x81, 1,    0x95, 6,
    0x75, 8, 0x15, 0, 0x25, 0x65, 0x19, 0, 0x29, 0x65, 0x81, 0,    0x05, 8,    0x19, 1,
    0x29, 5, 0x95, 5, 0x75, 1,    0x91, 2, 0x95, 1,    0x75, 3,    0x91, 1,    0xc0};
static const uint8_t mouse[] = {0x05, 1,    0x09, 2,    0xa1, 1,    0x85, 1,    0x09, 1,    0xa1,
                                0,    0x05, 9,    0x19, 1,    0x29, 3,    0x15, 0,    0x25, 1,
                                0x95, 3,    0x75, 1,    0x81, 2,    0x95, 1,    0x75, 5,    0x81,
                                1,    0x05, 1,    0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81,
                                0x25, 0x7f, 0x75, 8,    0x95, 3,    0x81, 6,    0xc0, 0xc0};
static const uint8_t gamepad[] = {
    0x05, 1,    0x09, 5,    0xa1, 1,    0x85, 1,    0x05, 9,    0x19, 1,    0x29, 16,   0x15,
    0,    0x25, 1,    0x75, 1,    0x95, 16,   0x81, 2,    0x05, 1,    0x09, 0x30, 0x09, 0x31,
    0x09, 0x32, 0x09, 0x35, 0x15, 0x81, 0x25, 0x7f, 0x75, 8,    0x95, 4,    0x81, 2,    0xc0};
static int map(int kind, const uint8_t **data) {
    switch (kind) {
    case LAP_HID_KEYBOARD:
        *data = keyboard;
        return sizeof(keyboard);
    case LAP_HID_MOUSE:
        *data = mouse;
        return sizeof(mouse);
    case LAP_HID_GAMEPAD:
        *data = gamepad;
        return sizeof(gamepad);
    default:
        return LAP_IO_EINVAL;
    }
}
uint32_t ls_lap_peripheral_caps(void) {
    if (!(ls_lap_sys_caps() & LAP_HW_BLE))
        return 0;
    uint32_t flags = (LAP_PERIPH_ADV ? LAP_HW_BLE_ADVERTISER : 0) |
                     (LAP_PERIPH_GATT ? LAP_HW_GATT_SERVER : 0) |
                     (LAP_PERIPH_HID ? LAP_HW_HID_DEVICE : 0);
#if !LAP_DEVICE && !defined(LS_LAP_PERIPH_HOST_TEST)
    const char *features = getenv("LAPSIM_PERIPHERAL");
    if (features && !strcmp(features, "none"))
        return 0;
    if (features && !strcmp(features, "advertiser"))
        return flags & LAP_HW_BLE_ADVERTISER;
    if (features && !strcmp(features, "gatt"))
        return flags & ~LAP_HW_HID_DEVICE;
#endif
    return flags;
}
bool ls_lap_peripheral_reserved(void) { return atomic_load(&resource_reserved); }
static int access(uint32_t hw, bool tx) {
    int e = ls_lap_av_access(0, false);
    if (e)
        return e;
    if ((ls_lap_peripheral_caps() & hw) != hw)
        return LAP_IO_ENOTSUP;
    if (tx && !(ls_lap_granted() & LAP_CAP_RADIO_TX))
        return LAP_IO_EACCES;
    return 0;
}
void ls_lap_peripheral_lost(void) { atomic_fetch_add(&drops, 1); }
void ls_lap_peripheral_release_inputs(void) {
    if (atomic_load(&owner_slot) != ls_lap_context_index()) return;
    atomic_store(&release_inputs, true);
    ls_lap_av_wake();
}
bool ls_lap_peripheral_live(int h) {
    return h > 0 && h == atomic_load(&published) &&
           atomic_load(&owner_epoch) == atomic_load(&ls_lap_av_epochs[atomic_load(&owner_slot)]) &&
           atomic_load(&ls_lap_av_actives[atomic_load(&owner_slot)]) && atomic_load(&closing) != h;
}
static bool valid(int h) { return h == p.handle && ls_lap_peripheral_live(h); }
static bool owned(int h) { return atomic_load(&owner_slot) == ls_lap_context_index() && valid(h); }
static int index_of(int id) {
    for (int i = 0; i < p.count; i++)
        if (p.attrs[i].id == id)
            return i;
    return -1;
}
static void event(int kind, int id, int error, int value, const void *data, size_t n) {
    if (p.head - p.tail == 8) {
        atomic_fetch_add(&drops, 1);
        return;
    }
    unsigned i = p.head++ % 8;
    p.events[i].e = (lap_gatt_server_event_t){.size = sizeof(lap_gatt_server_event_t),
                                              .version = 1,
                                              .kind = kind,
                                              .id = id,
                                              .error = error,
                                              .value = value,
                                              .payload_size = n};
    if (n)
        memcpy(p.events[i].data, data, n);
}
static int create(void) {
    if (p.handle || p.lease || p.resource || ls_lap_ble_in_use() ||
        !ls_lap_resource_claim(LS_LAP_LEASE_BLE, true))
        return LAP_IO_EBUSY;
    memset(&p, 0, sizeof(p));
    serial = serial % 0x7ffffffeu + 1;
    p.handle = serial;
    p.resource = true;
    atomic_store(&resource_reserved, true);
    p.epoch = atomic_load(&ls_lap_av_epoch);
    p.mtu = 23;
    p.state = LAP_OPERATION_PENDING;
    atomic_store(&release_inputs, false);
    atomic_store(&drops, 0);
    atomic_store(&owner_slot, ls_lap_context_index());
    atomic_store(&owner_epoch, p.epoch);
    atomic_store(&published, p.handle);
    return p.handle;
}
static bool ad_valid(const uint8_t *d, unsigned n) {
    for (unsigned i = 0; i < n;) {
        unsigned len = d[i];
        if (!len || i + 1 + len > n)
            return false;
        i += 1 + len;
    }
    return true;
}
int lap_ble_advertise(const lap_ble_advert_config_t *cfg) {
    int e = access(LAP_HW_BLE_ADVERTISER, true);
    if (e)
        return e;
    if (!ls_lap_api_readable(cfg, sizeof(*cfg)))
        return LAP_IO_EINVAL;
    lap_ble_advert_config_t a = *cfg;
    if (a.size != sizeof(a) || a.version != 1 || a.connectable > 1 || a.data_len > 31 ||
        a.scan_response_len > 31 || a.interval_min_ms < 20 || a.interval_max_ms > 10240 ||
        a.interval_min_ms > a.interval_max_ms || !ad_valid(a.data, a.data_len) ||
        !ad_valid(a.scan_response, a.scan_response_len))
        return LAP_IO_EINVAL;
    if (a.connectable && !a.server)
        return LAP_IO_EINVAL;
    if (a.tx_power_dbm < -24 || a.tx_power_dbm > 20)
        return LAP_IO_EPOWER;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int r;
    if (a.server) {
        r = owned(a.server) ? a.server : LAP_IO_ECLOSED;
        if (r > 0 && (!p.ready || p.advert_pending || p.connected))
            r = LAP_IO_EBUSY;
    } else
        r = create();
    if (r > 0) {
        if (a.connectable && !p.count) {
            if (!a.server) {
                p.handle = 0;
                atomic_store(&published, 0);
            }
            r = LAP_IO_EINVAL;
        } else {
            p.advert = a;
            p.advert_pending = true;
            p.state = LAP_OPERATION_PENDING;
        }
    }
    av_give(&lock);
    ls_lap_av_wake();
    return r;
}
static bool uuid_valid(const lap_uuid_t *u) {
    return u->length == 2 || u->length == 4 || u->length == 16;
}
static int database(const lap_gatt_attribute_t *a, int count) {
    int e = access(LAP_HW_GATT_SERVER, true);
    if (e)
        return e;
    if (count < 2 || count > LAP_BLE_SERVER_ATTRS || !ls_lap_api_readable(a, count * sizeof(*a)))
        return LAP_IO_EINVAL;
    /* Validate before taking the worker lock; caller is synchronous screen task.
     */
    int svc = 0, chr = 0;
    for (int i = 0; i < count; i++) {
        const lap_gatt_attribute_t *v = &a[i];
        if (v->size != sizeof(*v) || v->version != 1 || !v->id || v->reserved ||
            !uuid_valid(&v->uuid) || v->value_len > 128 || (v->flags & ~127u))
            return LAP_IO_EINVAL;
        for (int j = 0; j < i; j++)
            if (a[j].id == v->id)
                return LAP_IO_EINVAL;
        if (v->kind == LAP_GATTS_SERVICE) {
            if (v->parent || v->flags || v->value_len ||
                (v->uuid.length == 2 && v->uuid.bytes[1] == 0x18 && v->uuid.bytes[0] <= 1))
                return LAP_IO_EINVAL;
            svc = v->id;
            chr = 0;
        } else if (v->kind == LAP_GATTS_CHARACTERISTIC) {
            if (!svc || v->parent != svc || !(v->flags & 31))
                return LAP_IO_EINVAL;
            chr = v->id;
        } else if (v->kind == LAP_GATTS_DESCRIPTOR) {
            if (!chr || v->parent != chr || (v->flags & 28) || !(v->flags & 3) ||
                (v->uuid.length == 2 && v->uuid.bytes[0] == 2 && v->uuid.bytes[1] == 0x29))
                return LAP_IO_EINVAL;
        } else
            return LAP_IO_EINVAL;
    }
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int r = create();
    if (r > 0) {
        memcpy(p.attrs, a, count * sizeof(*a));
        p.count = count;
    }
    av_give(&lock);
    ls_lap_av_wake();
    return r;
}
int lap_gatt_server_open(const lap_gatt_attribute_t *a, int count) { return database(a, count); }
int lap_gatt_server_set(int h, uint16_t id, const void *data, size_t n) {
    int e = access(0, true);
    if (e)
        return e;
    if (n > 128 || (n && !ls_lap_api_readable(data, n)))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int r = LAP_IO_ECLOSED, i = index_of(id);
    if (owned(h)) {
        r = LAP_IO_EINVAL;
        if (i >= 0 && p.attrs[i].kind != LAP_GATTS_SERVICE) {
            p.attrs[i].value_len = n;
            if (n)
                memcpy(p.attrs[i].value, data, n);
            r = 0;
        }
    }
    av_give(&lock);
    return r;
}
int lap_gatt_notify(int h, uint16_t id, const void *data, size_t n, bool indicate) {
    int e = access(LAP_HW_GATT_SERVER, true);
    if (e)
        return e;
    if (n > 20)
        return LAP_IO_ELIMIT;
    if (n && !ls_lap_api_readable(data, n))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int r = LAP_IO_ECLOSED, i = index_of(id), bit = indicate ? 2 : 1;
    if (owned(h)) {
        r = LAP_IO_EINVAL;
        if (i >= 0 && (p.attrs[i].flags & (indicate ? LAP_GATTS_INDICATE : LAP_GATTS_NOTIFY))) {
            r = LAP_IO_EAGAIN;
            if (p.ready && p.connected && (p.subscribed[i] & bit)) {
                if (((p.attrs[i].flags & LAP_GATTS_ENCRYPT) && !p.encrypted) ||
                    ((p.attrs[i].flags & LAP_GATTS_AUTHENTICATE) && !p.authenticated))
                    r = LAP_IO_EAUTH;
                else {
                    r = LAP_IO_EBUSY;
                    if (!p.send_pending) {
                        p.send.kind = PERIPH_SEND;
                        p.send.handle = h;
                        p.send.id = id;
                        p.send.indicate = indicate;
                        p.send.len = n;
                        p.attrs[i].value_len = n;
                        if (n) {
                            memcpy(p.send.data, data, n);
                            memcpy(p.attrs[i].value, data, n);
                        }
                        p.send_pending = true;
                        r = 0;
                    }
                }
            }
        }
    }
    av_give(&lock);
    ls_lap_av_wake();
    return r;
}
int lap_ble_peripheral_status(int h, lap_ble_peripheral_status_t *out) {
    int e = access(0, false);
    if (e)
        return e;
    if (!ls_lap_api_readable(out, sizeof(*out)))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int r = LAP_IO_ECLOSED;
    if (owned(h)) {
        *out = (lap_ble_peripheral_status_t){.size = sizeof(*out),
                                             .version = 1,
                                             .state = p.state,
                                             .error = p.error,
                                             .dropped = atomic_load(&drops),
                                             .mtu = p.mtu,
                                             .advertising = p.advertising,
                                             .connected = p.connected,
                                             .bonded = p.bonded,
                                             .encrypted = p.encrypted,
                                             .authenticated = p.authenticated,
                                             .tx_power_dbm = p.power};
        r = 0;
    }
    av_give(&lock);
    return r;
}
int lap_gatt_server_next(int h, lap_gatt_server_event_t *out, void *data, size_t cap) {
    int e = access(0, false);
    if (e)
        return e;
    if (cap > 128 || !ls_lap_api_readable(out, sizeof(*out)) ||
        (cap && !ls_lap_api_readable(data, cap)))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int r = LAP_IO_ECLOSED;
    if (owned(h)) {
        r = LAP_IO_EAGAIN;
        if (p.head != p.tail) {
            unsigned i = p.tail % 8;
            if (p.events[i].e.payload_size > cap)
                r = LAP_IO_ELIMIT;
            else {
                *out = p.events[i].e;
                out->dropped = atomic_load(&drops);
                if (out->payload_size)
                    memcpy(data, p.events[i].data, out->payload_size);
                p.tail++;
                r = 1;
            }
        }
    }
    av_give(&lock);
    return r;
}
void lap_gatt_server_close(int h) {
    if (ls_lap_api_inside() && atomic_load(&owner_slot) == ls_lap_context_index())
        av_close(&closing, &published, h);
    ls_lap_av_wake();
}
void lap_ble_advertise_stop(int h) {
    if (!ls_lap_api_inside() || !av_take(&lock))
        return;
    if (owned(h)) {
        if (!p.count)
            av_close(&closing, &published, h);
        else {
            p.stop_pending = true;
            p.advert_pending = false;
        }
    }
    av_give(&lock);
    ls_lap_av_wake();
}
int lap_hid_device_report_map(int kind, void *out, size_t cap) {
    int e = access(LAP_HW_HID_DEVICE, false);
    if (e)
        return e;
    const uint8_t *m;
    int n = map(kind, &m);
    if (n < 0)
        return n;
    if (cap < (unsigned)n)
        return LAP_IO_ELIMIT;
    if (!ls_lap_api_readable(out, n))
        return LAP_IO_EINVAL;
    memcpy(out, m, n);
    return n;
}
static lap_gatt_attribute_t attr(int id, int parent, int kind, int uuid, int flags) {
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
int lap_hid_device_open(int kind, const char *name) {
    int e = access(LAP_HW_HID_DEVICE, true);
    if (e)
        return e;
    if (!(ls_lap_granted() & LAP_CAP_HID_DEVICE))
        return LAP_IO_EACCES;
    char label[25];
    if (!ls_lap_av_string(label, sizeof(label), name) || !label[0])
        return LAP_IO_EINVAL;
    const uint8_t *m;
    int n = map(kind, &m);
    if (n < 0)
        return n;
    /* No large stack database: claim session then build copied state under lock.
     */
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int h = create();
    if (h > 0) {
        p.hid = kind;
        p.count = kind == LAP_HID_KEYBOARD ? 9 : 7;
        int read = LAP_GATTS_READ | LAP_GATTS_ENCRYPT;
        p.attrs[0] = attr(1, 0, LAP_GATTS_SERVICE, 0x1812, 0);
        p.attrs[1] = attr(2, 1, LAP_GATTS_CHARACTERISTIC, 0x2a4a, read); /* HID information */
        const uint8_t info[] = {0x11, 1, 0, 2};
        memcpy(p.attrs[1].value, info, 4);
        p.attrs[1].value_len = 4;
        p.attrs[2] = attr(3, 1, LAP_GATTS_CHARACTERISTIC, 0x2a4b, read); /* report map */
        memcpy(p.attrs[2].value, m, n);
        p.attrs[2].value_len = n;
        p.attrs[3] =
            attr(4, 1, LAP_GATTS_CHARACTERISTIC, 0x2a4c, LAP_GATTS_WRITE_NR | LAP_GATTS_ENCRYPT);
        p.attrs[4] = attr(5, 1, LAP_GATTS_CHARACTERISTIC, 0x2a4e, read | LAP_GATTS_WRITE_NR);
        p.attrs[4].value[0] = 1;
        p.attrs[4].value_len = 1; /* report protocol only */
        p.attrs[5] = attr(6, 1, LAP_GATTS_CHARACTERISTIC, 0x2a4d, read | LAP_GATTS_NOTIFY);
        p.attrs[5].value_len = kind == 1 ? 8 : kind == 2 ? 4 : 6;
        p.attrs[6] = attr(7, 6, LAP_GATTS_DESCRIPTOR, 0x2908, read);
        p.attrs[6].value[0] = 1;
        p.attrs[6].value[1] = 1;
        p.attrs[6].value_len = 2;
        if (kind == LAP_HID_KEYBOARD) {
            p.attrs[7] = attr(8, 1, LAP_GATTS_CHARACTERISTIC, 0x2a4d,
                              read | LAP_GATTS_WRITE | LAP_GATTS_WRITE_NR);
            p.attrs[7].value_len = 1;
            p.attrs[8] = attr(9, 8, LAP_GATTS_DESCRIPTOR, 0x2908, read);
            p.attrs[8].value[0] = 1;
            p.attrs[8].value[1] = 2;
            p.attrs[8].value_len = 2;
        }
        p.advert = (lap_ble_advert_config_t){.size = sizeof(p.advert),
                                             .version = 1,
                                             .server = h,
                                             .interval_min_ms = 100,
                                             .interval_max_ms = 100,
                                             .connectable = 1,
                                             .data_len = 11};
        const uint8_t ad[] = {2, 1, 6, 3, 3, 0x12, 0x18, 3, 0x19, 0xc0, 3};
        memcpy(p.advert.data, ad, sizeof(ad));
        p.advert.data[9] = kind == 1 ? 0xc1 : kind == 2 ? 0xc2 : 0xc4;
        size_t len = strlen(label);
        p.advert.scan_response[0] = len + 1;
        p.advert.scan_response[1] = 9;
        memcpy(p.advert.scan_response + 2, label, len);
        p.advert.scan_response_len = len + 2;
        p.advert_pending = true;
    }
    av_give(&lock);
    ls_lap_av_wake();
    return h;
}
int lap_hid_device_send(int h, const void *data, size_t n) {
    int e = access(LAP_HW_HID_DEVICE, true);
    if (e)
        return e;
    if (!(ls_lap_granted() & LAP_CAP_HID_DEVICE))
        return LAP_IO_EACCES;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int kind = owned(h) ? p.hid : 0;
    av_give(&lock);
    if (!kind)
        return LAP_IO_ECLOSED;
    if (n != (size_t)(kind == 1 ? 8 : kind == 2 ? 4 : 6))
        return LAP_IO_EINVAL;
    return lap_gatt_notify(h, 6, data, n, false);
}
void lap_hid_device_close(int h) { lap_gatt_server_close(h); }
/* Host bridge. The host never retains pointers to this state or app memory. */
bool ls_lap_peripheral_command(ls_lap_peripheral_command_t *out) {
    if (!av_take(&lock))
        return false;
    if (p.hid && p.ready && p.connected && p.encrypted && atomic_load(&release_inputs)) {
        if (!p.send_pending || p.send.kind) {
            p.send.kind = PERIPH_SEND;
            p.send.handle = p.handle;
            p.send.id = 6;
            p.send.indicate = 0;
            p.send.len = p.hid == 1 ? 8 : p.hid == 2 ? 4 : 6;
            memset(p.send.data, 0, 20);
            p.send_pending = true;
            int i = index_of(6);
            if (i >= 0) {
                p.attrs[i].value_len = p.send.len;
                memset(p.attrs[i].value, 0, p.send.len);
            }
            atomic_store(&release_inputs, false);
        }
    }
    bool have = p.handle && p.lease;
    if (have) {
        memset(out, 0, sizeof(*out));
        out->handle = p.handle;
        if (!valid(p.handle)) {
            if (!p.close_sent) {
                out->kind = PERIPH_CLOSE;
                p.close_sent = true;
            } else
                have = false;
        } else if (!p.opened) {
            out->kind = PERIPH_OPEN;
            out->count = p.count;
            memcpy(out->attrs, p.attrs, sizeof(p.attrs));
            p.opened = true;
        } else if (p.ready && p.stop_pending) {
            out->kind = PERIPH_STOP;
            p.stop_pending = false;
        } else if (p.ready && p.advert_pending) {
            out->kind = PERIPH_ADVERTISE;
            out->advert = p.advert;
            p.advert_pending = false;
        } else if (p.ready && p.send_pending) {
            out->kind = p.send.kind;
            out->handle = p.send.handle;
            out->id = p.send.id;
            out->len = p.send.len;
            out->indicate = p.send.indicate;
            memcpy(out->data, p.send.data, 20);
            /* retain send_pending until result */ p.send.kind = 0;
            if (!out->kind)
                have = false;
        } else
            have = false;
    }
    av_give(&lock);
    return have;
}
bool ls_lap_peripheral_complete(int h, int kind, int error, int power) {
    if (!av_take(&lock))
        return false;
    if (h == p.handle) {
        if (kind == PERIPH_CLOSE) {
            p.handle = 0;
            p.opened = false;
            atomic_store(&published, 0);
        } else if (valid(h)) {
            if (kind == PERIPH_OPEN) {
                p.ready = !error;
                event(LAP_PERIPHERAL_READY, 0, error, 0, NULL, 0);
            }
            if (kind == PERIPH_ADVERTISE || kind == PERIPH_STOP) {
                p.advertising = kind == PERIPH_ADVERTISE && !error;
                p.power = power;
                event(LAP_PERIPHERAL_ADVERTISING, 0, error, p.advertising, NULL, 0);
            }
            if (kind == PERIPH_SEND) {
                p.send_pending = false;
                event(LAP_PERIPHERAL_SENT, p.send.id, error, 0, NULL, 0);
            }
            p.error = error;
            p.state = error ? LAP_OPERATION_DONE : LAP_OPERATION_READY;
        }
    }
    av_give(&lock);
    ls_lap_av_wake();
    return true;
}
bool ls_lap_peripheral_event(int h, int kind, int id, int error, int value) {
    if (!av_take(&lock))
        return false;
    if (valid(h)) {
        if (kind == LAP_PERIPHERAL_CONNECTED) {
            p.connected = !error;
            p.advertising = false;
        }
        if (kind == LAP_PERIPHERAL_DISCONNECTED) {
            p.connected = p.bonded = p.encrypted = p.authenticated = false;
            p.mtu = 23;
            memset(p.subscribed, 0, sizeof(p.subscribed));
            p.send_pending = false;
        }
        if (kind == LAP_PERIPHERAL_ADVERTISING)
            p.advertising = value != 0;
        if (kind == LAP_PERIPHERAL_MTU)
            p.mtu = value;
        if (kind == LAP_PERIPHERAL_BOND) {
            p.bonded = !error && (value & 1);
            p.encrypted = !error && (value & 2);
            p.authenticated = !error && (value & 4);
        }
        int i = index_of(id);
        if (kind == LAP_PERIPHERAL_SUBSCRIBE && i >= 0)
            p.subscribed[i] = value;
        event(kind, id, error, value, NULL, 0);
    }
    av_give(&lock);
    return true;
}
int ls_lap_peripheral_access(int h, int id, bool write, void *data, size_t *n) {
    if (!av_take(&lock))
        return 0x11;
    int r = 0x0e, i = index_of(id);
    if (valid(h) && i >= 0 && p.attrs[i].kind != LAP_GATTS_SERVICE) {
        lap_gatt_attribute_t *a = &p.attrs[i];
        r = write ? 3 : 2;
        if (p.hid && write &&
            ((id == 5 && (*n != 1 || ((uint8_t *)data)[0] != 1)) ||
             (id == 4 && (*n != 1 || ((uint8_t *)data)[0] > 1)) || (id == 8 && *n != 1))) {
            av_give(&lock);
            return 0x13;
        }
        if (a->flags & (write ? (LAP_GATTS_WRITE | LAP_GATTS_WRITE_NR) : LAP_GATTS_READ)) {
            r = 0x11;
            if (p.head - p.tail < 8) {
                if (write) {
                    r = *n > 128 ? 0x0d : 0;
                    if (!r) {
                        memcpy(a->value, data, *n);
                        a->value_len = *n;
                        event(LAP_PERIPHERAL_WRITE, id, 0, 0, data, *n);
                    }
                } else {
                    r = *n < a->value_len ? 0x11 : 0;
                    if (!r) {
                        *n = a->value_len;
                        memcpy(data, a->value, *n);
                        event(LAP_PERIPHERAL_READ, id, 0, 0, NULL, 0);
                    }
                }
            } else
                atomic_fetch_add(&drops, 1);
        }
    }
    av_give(&lock);
    return r;
}
/* The simulator peer lives in bench/shims; it uses the same ATT/cache path. */
int ls_lap_peripheral_peer_access(int h, int id, bool write, void *data, size_t *n) {
    if (!av_take(&lock))
        return 0x11;
    int i = index_of(id);
    bool permitted = valid(h) && p.connected && i >= 0 &&
                     (!(p.attrs[i].flags & LAP_GATTS_ENCRYPT) || p.encrypted) &&
                     (!(p.attrs[i].flags & LAP_GATTS_AUTHENTICATE) || p.authenticated);
    av_give(&lock);
    return permitted ? ls_lap_peripheral_access(h, id, write, data, n) : 0x0f;
}
#if LAP_DEVICE || defined(LS_LAP_PERIPH_HOST_TEST)
void ls_lap_peripheral_script(const char *c) { (void)c; }
#endif
bool ls_lap_peripheral_service(void) {
    if (!av_take(&lock))
        return true;
    bool want = p.handle != 0, lease = p.lease;
    if (!want && !lease && p.resource) {
        unsigned saved = ls_lap_producer_select(LS_LAP_LEASE_BLE);
        ls_lap_resource_claim(LS_LAP_LEASE_BLE, false);
        ls_lap_producer_restore(saved);
        p.resource = false;
        atomic_store(&resource_reserved, false);
    }
    bool failed = p.error && p.state == LAP_OPERATION_DONE;
    if (want && !lease && !valid(p.handle)) {
        p.handle = 0;
        atomic_store(&published, 0);
        want = false;
    }
    av_give(&lock);
    if (want && !lease && !failed) {
        int r = ls_lap_peripheral_lease(true);
        if (av_take(&lock)) {
            if (r) {
                p.error = r;
                p.state = LAP_OPERATION_DONE;
                event(LAP_PERIPHERAL_READY, 0, r, 0, NULL, 0);
                /* retain failed session until app closes it */
                if (!valid(p.handle)) {
                    p.handle = 0;
                    atomic_store(&published, 0);
                }
            } else
                p.lease = true;
            av_give(&lock);
        } else if (!r)
            ls_lap_peripheral_lease(false);
    } else if (!want && lease) {
        ls_lap_peripheral_lease(false);
        if (av_take(&lock)) {
            p.lease = false;
            av_give(&lock);
        }
    }
#if !LAP_DEVICE && !defined(LS_LAP_PERIPH_HOST_TEST)
    ls_lap_peripheral_host_pump();
#endif
    return want || lease;
}

#else /* All peripheral roles compiled out: no state, pools or lease. */
uint32_t ls_lap_peripheral_caps(void) { return 0; }
bool ls_lap_peripheral_reserved(void) { return false; }
bool ls_lap_peripheral_live(int h) {
    (void)h;
    return false;
}
void ls_lap_peripheral_lost(void) {}
void ls_lap_peripheral_release_inputs(void) {}
bool ls_lap_peripheral_service(void) { return false; }
bool ls_lap_peripheral_command(ls_lap_peripheral_command_t *out) {
    (void)out;
    return false;
}
bool ls_lap_peripheral_complete(int h, int k, int e, int v) {
    (void)h;
    (void)k;
    (void)e;
    (void)v;
    return true;
}
bool ls_lap_peripheral_event(int h, int k, int id, int e, int v) {
    (void)h;
    (void)k;
    (void)id;
    (void)e;
    (void)v;
    return true;
}
int ls_lap_peripheral_access(int h, int id, bool w, void *d, size_t *n) {
    (void)h;
    (void)id;
    (void)w;
    (void)d;
    (void)n;
    return 0x0e;
}
int ls_lap_peripheral_peer_access(int h, int id, bool w, void *d, size_t *n) {
    (void)h;
    (void)id;
    (void)w;
    (void)d;
    (void)n;
    return 0x0e;
}
void ls_lap_peripheral_script(const char *c) { (void)c; }
int ls_lap_peripheral_lease(bool on) {
    (void)on;
    return LAP_IO_ENOTSUP;
}
void ls_lap_peripheral_host_pump(void) {}
int lap_ble_advertise(const lap_ble_advert_config_t *c) {
    (void)c;
    return LAP_IO_ENOTSUP;
}
void lap_ble_advertise_stop(int h) { (void)h; }
int lap_gatt_server_open(const lap_gatt_attribute_t *a, int n) {
    (void)a;
    (void)n;
    return LAP_IO_ENOTSUP;
}
int lap_gatt_server_set(int h, uint16_t id, const void *d, size_t n) {
    (void)h;
    (void)id;
    (void)d;
    (void)n;
    return LAP_IO_ENOTSUP;
}
int lap_gatt_notify(int h, uint16_t id, const void *d, size_t n, bool i) {
    (void)h;
    (void)id;
    (void)d;
    (void)n;
    (void)i;
    return LAP_IO_ENOTSUP;
}
int lap_ble_peripheral_status(int h, lap_ble_peripheral_status_t *o) {
    (void)h;
    (void)o;
    return LAP_IO_ENOTSUP;
}
int lap_gatt_server_next(int h, lap_gatt_server_event_t *o, void *d, size_t n) {
    (void)h;
    (void)o;
    (void)d;
    (void)n;
    return LAP_IO_ENOTSUP;
}
void lap_gatt_server_close(int h) { (void)h; }
int lap_hid_device_open(int k, const char *n) {
    (void)k;
    (void)n;
    return LAP_IO_ENOTSUP;
}
int lap_hid_device_send(int h, const void *d, size_t n) {
    (void)h;
    (void)d;
    (void)n;
    return LAP_IO_ENOTSUP;
}
void lap_hid_device_close(int h) { (void)h; }
int lap_hid_device_report_map(int k, void *d, size_t n) {
    (void)k;
    (void)d;
    (void)n;
    return LAP_IO_ENOTSUP;
}
#endif
