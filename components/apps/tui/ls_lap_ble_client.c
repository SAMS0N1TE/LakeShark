#include "ls_lap_context.h"
#include "ls_lap_net_audio.h"
#include "ls_lap_ble_peripheral.h"
#include <stddef.h>
#include <stdlib.h>
static atomic_flag lock = ATOMIC_FLAG_INIT;
static unsigned serial;
static LS_LAP_POOL atomic_int p4_canceled_scan_slots[LS_LAP_CONTEXTS], p4_canceled_conn_slots[LS_LAP_CONTEXTS], p4_closed_slots[LS_LAP_CONTEXTS], p4_scan_published_slots[LS_LAP_CONTEXTS], p4_conn_published_slots[LS_LAP_CONTEXTS];
#define p4_canceled_conn (p4_canceled_conn_slots[ls_lap_context_index()])
#define p4_canceled_scan (p4_canceled_scan_slots[ls_lap_context_index()])
#define p4_closed (p4_closed_slots[ls_lap_context_index()])
#define p4_conn_published (p4_conn_published_slots[ls_lap_context_index()])
#define p4_scan_published (p4_scan_published_slots[ls_lap_context_index()])

static void ble_closed(int h) { atomic_store(&p4_closed, h); }
static EXT_RAM_BSS_ATTR struct {
    int scan, connection, operation, closing;
    unsigned epoch, wide_epoch;
    bool lease, pending, command_ready, connected, pairing, wide, info_valid;
    uint32_t timeout_ms;
    lap_ble_conn_info_t info;
    lap_operation_t status;
    ls_lap_ble_command_t command;
    struct {
        lap_ble_advert_t meta;
        uint8_t data[LAP_BLE_AD_MAX];
    } ads[8];
    unsigned ah, at;
    /* 16 x LAP_GATT_VALUE_MAX in the app's PSRAM share: long reads and notifications. */
    struct {
        lap_ble_event_t meta;
        uint8_t data[LAP_GATT_VALUE_MAX];
    } events[16];
    unsigned eh, et;
    uint32_t started, last_ad, notify_at;
} p4_b_slots[LS_LAP_CONTEXTS];
#define p4_b (p4_b_slots[ls_lap_context_index()])

static LS_LAP_POOL atomic_uint p4_lost_slots[LS_LAP_CONTEXTS];
#define p4_lost (p4_lost_slots[ls_lap_context_index()])

static int id(void) {
    serial = serial % 0xfffffff + 1;
    return serial;
}
bool ls_lap_ble_in_use(void) {
    if (!av_take(&lock)) return true;
    bool busy = false;
    for (unsigned i=0;i<LS_LAP_CONTEXTS;i++)
        if (p4_b_slots[i].scan || p4_b_slots[i].connection || p4_b_slots[i].closing ||
            p4_b_slots[i].command_ready || p4_b_slots[i].lease) busy = true;
    av_give(&lock);
    return busy;
}
int lap_ble_scan_start(void) {
    if (ls_lap_peripheral_reserved()) return LAP_IO_EBUSY;
    int e = ls_lap_av_access(LAP_HW_BLE, false);
    if (e)
        return e;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int h = LAP_IO_EBUSY;
    if (!p4_b.scan && (!p4_b.connection || p4_b.epoch == atomic_load(&ls_lap_av_epoch)) && ls_lap_resource_claim(LS_LAP_LEASE_BLE,true)) {
        p4_b.scan = h = id();
        atomic_store(&p4_scan_published, h);
        p4_b.epoch = atomic_load(&ls_lap_av_epoch);
        p4_b.status = av_status(LAP_OPERATION_PENDING, 0);
        p4_b.ah = p4_b.at = 0;
    }
    av_give(&lock);
    ls_lap_av_wake();
    return h;
}
int lap_ble_advert_next(int scan, lap_ble_advert_t *out, void *data, size_t cap) {
    if (scan <= 0)
        return LAP_IO_ECLOSED;
    int e = ls_lap_av_access(LAP_HW_BLE, false);
    if (e)
        return e;
    if (!ls_lap_api_readable(out, sizeof(*out)) || cap > LAP_BLE_AD_MAX ||
        (cap && !ls_lap_api_readable(data, cap)))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int r = LAP_IO_ECLOSED;
    if (p4_b.scan == scan && p4_b.epoch == atomic_load(&ls_lap_av_epoch) && atomic_load(&p4_canceled_scan) != p4_b.scan) {
        r = p4_b.status.error ? p4_b.status.error : LAP_IO_EAGAIN;
        if (p4_b.ah != p4_b.at) {
            unsigned i = p4_b.at % 8;
            if (p4_b.ads[i].meta.payload_size > cap)
                r = LAP_IO_ELIMIT;
            else {
                *out = p4_b.ads[i].meta;
                r = out->payload_size;
                if (r)
                    memcpy(data, p4_b.ads[i].data, r);
                p4_b.at++;
            }
        }
    }
    av_give(&lock);
    return r;
}
int lap_ble_scan_status(int scan, lap_operation_t *out) {
    if (scan <= 0)
        return LAP_IO_ECLOSED;
    int e = ls_lap_av_access(0, false);
    if (e)
        return e;
    if (!ls_lap_api_readable(out, sizeof(*out)))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int r = LAP_IO_ECLOSED;
    if (p4_b.scan == scan && p4_b.epoch == atomic_load(&ls_lap_av_epoch) && atomic_load(&p4_canceled_scan) != p4_b.scan) {
        *out = p4_b.status;
        out->dropped = atomic_load(&p4_lost);
        r = 0;
    }
    av_give(&lock);
    return r;
}
void lap_ble_scan_stop(int h) {
    if (ls_lap_api_inside() && h > 0)
        av_close(&p4_canceled_scan, &p4_scan_published, h);
    ls_lap_av_wake();
}

static void ble_advert_push(const uint8_t *addr, uint8_t type, int rssi, bool response, const void *data,
                            size_t n, uint32_t at) {
    if (!av_take(&lock)) {
        atomic_fetch_add(&p4_lost, 1);
        return;
    }
    if (p4_b.scan && atomic_load(&p4_canceled_scan) != p4_b.scan && p4_b.epoch == atomic_load(&ls_lap_av_epoch)) {
        if (n > LAP_BLE_AD_MAX || p4_b.ah - p4_b.at == 8)
            atomic_fetch_add(&p4_lost, 1);
        else {
            unsigned i = p4_b.ah++ % 8;
            p4_b.ads[i].meta = (lap_ble_advert_t){.size = sizeof(lap_ble_advert_t),
                                               .version = 1,
                                               .payload_size = n,
                                               .addr_type = type,
                                               .rssi = rssi,
                                               .scan_response = response,
                                               .at_ms = at};
            memcpy(p4_b.ads[i].meta.addr, addr, 6);
            if (n)
                memcpy(p4_b.ads[i].data, data, n);
            p4_b.status.transferred++;
        }
    }
    av_give(&lock);
}
/* Header fields only; the data copy is sized by len. No stack temporary of
   the whole command (it holds LAP_GATT_VALUE_MAX bytes). */
static void set_command(int kind, int handle, int operation, uint16_t attribute, uint16_t end) {
    memset(&p4_b.command, 0, offsetof(ls_lap_ble_command_t, data));
    p4_b.command.kind = kind;
    p4_b.command.handle = handle;
    p4_b.command.operation = operation;
    p4_b.command.attribute = attribute;
    p4_b.command.end = end;
}
static int command_ex(int connection, int kind, uint16_t attribute, uint16_t end, const void *data, size_t n,
                      uint8_t flags, uint32_t value) {
    int e = ls_lap_av_access(LAP_HW_BLE, false);
    if (e)
        return e;
    if (n > LAP_GATT_VALUE_MAX)
        return LAP_IO_ELIMIT;
    if (n && !ls_lap_api_readable(data, n))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int r = LAP_IO_ECLOSED;
    if (p4_b.connection == connection && p4_b.epoch == atomic_load(&ls_lap_av_epoch) &&
        atomic_load(&p4_canceled_conn) != p4_b.connection) {
        /* A passkey answers the pairing in flight; it is not a procedure. */
        bool answer = kind == LS_BLE_CMD_PASSKEY;
        r = LAP_IO_EBUSY;
        if (answer && p4_b.connected && !p4_b.pairing)
            r = LAP_IO_EINVAL;
        else if (p4_b.connected && !p4_b.command_ready && (answer || !p4_b.pending)) {
            int op = answer ? p4_b.operation : id();
            set_command(kind, connection, op, attribute, end);
            p4_b.command.len = n;
            p4_b.command.flags = flags;
            p4_b.command.value = value;
            if (n)
                memcpy(p4_b.command.data, data, n);
            p4_b.command_ready = true;
            p4_b.started = ls_lap_av_now();
            if (answer)
                r = 0;
            else {
                p4_b.operation = r = op;
                p4_b.pending = true;
                p4_b.pairing = kind == LS_BLE_CMD_PAIR;
                /* The user may type a passkey: NimBLE's own SM timeout is 30 s. */
                p4_b.timeout_ms = p4_b.pairing ? 40000 : 12000;
            }
        }
    }
    av_give(&lock);
    ls_lap_av_wake();
    return r;
}
static int command(int connection, int kind, uint16_t attribute, uint16_t end, const void *data, size_t n) {
    return command_ex(connection, kind, attribute, end, data, n, 0, 0);
}
int lap_ble_connect(const uint8_t addr[6], uint8_t type) {
    if (ls_lap_peripheral_reserved()) return LAP_IO_EBUSY;
    int e = ls_lap_av_access(LAP_HW_BLE, false);
    if (e)
        return e;
    if (type > 1 || !ls_lap_api_readable(addr, 6))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int r = LAP_IO_EBUSY;
    if (!p4_b.connection && !p4_b.closing && !p4_b.command_ready &&
        (!p4_b.scan || p4_b.epoch == atomic_load(&ls_lap_av_epoch)) && ls_lap_resource_claim(LS_LAP_LEASE_BLE,true)) {
        p4_b.connection = r = id();
        atomic_store(&p4_conn_published, r);
        bool wide = p4_b.wide && p4_b.wide_epoch == atomic_load(&ls_lap_av_epoch);
        p4_b.epoch = atomic_load(&ls_lap_av_epoch);
        p4_b.connected = p4_b.pairing = p4_b.info_valid = false;
        p4_b.timeout_ms = 12000;
        set_command(LS_BLE_CMD_CONNECT, r, 0, 0, 0);
        p4_b.command.addr_type = type;
        p4_b.command.flags = wide ? LS_BLE_CONNECT_WIDE : 0;
        memcpy(p4_b.command.addr, addr, 6);
        p4_b.command_ready = p4_b.pending = true;
        p4_b.started = ls_lap_av_now();
        p4_b.eh = p4_b.et = 0;
    }
    av_give(&lock);
    ls_lap_av_wake();
    return r;
}
int lap_gatt_discover(int h, uint16_t first, uint16_t last) {
    if ((first || last) && (!first || last < first))
        return LAP_IO_EINVAL;
    return command(h, 2, first, last, NULL, 0);
}
int lap_gatt_descriptors(int h, uint16_t first, uint16_t last) {
    if (!first || last <= first)
        return LAP_IO_EINVAL;
    return command(h, 6, first, last, NULL, 0);
}
int lap_gatt_read(int h, uint16_t a) {
    if (!a)
        return LAP_IO_EINVAL;
    return command(h, 3, a, 0, NULL, 0);
}
int lap_gatt_write(int h, uint16_t a, const void *p, size_t n) {
    if (!a)
        return LAP_IO_EINVAL;
    return command(h, 4, a, 0, p, n);
}
int lap_gatt_write_cmd(int h, uint16_t a, const void *p, size_t n) {
    if (!a)
        return LAP_IO_EINVAL;
    return command(h, LS_BLE_CMD_WRITE_CMD, a, 0, p, n);
}
int lap_ble_pair(int h, unsigned flags) {
    if (flags & ~(unsigned)(LAP_BLE_PAIR_BOND | LAP_BLE_PAIR_MITM))
        return LAP_IO_EINVAL;
    return command_ex(h, LS_BLE_CMD_PAIR, 0, 0, NULL, 0, flags, 0);
}
int lap_ble_passkey(int h, int action, uint32_t value) {
    if (action == LAP_BLE_PASSKEY_INPUT ? value > 999999u : action == LAP_BLE_PASSKEY_NUMCMP ? value > 1 : true)
        return LAP_IO_EINVAL;
    return command_ex(h, LS_BLE_CMD_PASSKEY, 0, 0, NULL, 0, (uint8_t)action, value);
}
int lap_ble_forget(int h) { return command(h, LS_BLE_CMD_FORGET, 0, 0, NULL, 0); }
int lap_ble_conn_info(int h, lap_ble_conn_info_t *out) {
    if (h <= 0)
        return LAP_IO_ECLOSED;
    int e = ls_lap_av_access(0, false);
    if (e)
        return e;
    if (!ls_lap_api_readable(out, sizeof(*out)))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int r = LAP_IO_ECLOSED;
    if (p4_b.connection == h && p4_b.epoch == atomic_load(&ls_lap_av_epoch) &&
        atomic_load(&p4_canceled_conn) != p4_b.connection) {
        r = LAP_IO_EAGAIN;
        if (p4_b.connected && p4_b.info_valid) {
            *out = p4_b.info;
            out->size = sizeof(*out);
            out->version = 1;
            r = 0;
        }
    }
    av_give(&lock);
    return r;
}
int lap_gatt_subscribe(int h, uint16_t a, bool on) {
    /* Stack input is copied synchronously. */
    uint8_t data[2] = {on ? 1 : 0, 0};
    return lap_gatt_write(h, a, data, 2);
}
void lap_ble_disconnect(int h) {
    if (ls_lap_api_inside() && h > 0)
        av_close(&p4_canceled_conn, &p4_conn_published, h);
    ls_lap_av_wake();
}

int lap_ble_event_next(lap_ble_event_t *out, void *data, size_t cap) {
    int e = ls_lap_av_access(0, false);
    if (e)
        return e;
    if (cap > LAP_GATT_VALUE_MAX)
        cap = LAP_GATT_VALUE_MAX;
    if (!ls_lap_api_readable(out, sizeof(*out)) || (cap && !ls_lap_api_readable(data, cap)))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    /* A buffer over 20 bytes marks a 2.9 app: its next connect exchanges the MTU. */
    if (cap > LAP_GATT_VALUE_LEGACY) {
        p4_b.wide = true;
        p4_b.wide_epoch = atomic_load(&ls_lap_av_epoch);
    }
    int r = LAP_IO_EAGAIN;
    if (p4_b.eh != p4_b.et) {
        unsigned i = p4_b.et % 16;
        size_t n = p4_b.events[i].meta.payload_size;
        if (cap < n && n > LAP_GATT_VALUE_LEGACY && cap <= LAP_GATT_VALUE_LEGACY) {
            /* A 20-byte reader sees a long value as 2.8 reported it. */
            *out = p4_b.events[i].meta;
            out->dropped = atomic_load(&p4_lost);
            out->error = LAP_IO_ELIMIT;
            out->payload_size = 0;
            p4_b.et++;
            r = 1;
        } else if (cap < n)
            r = LAP_IO_ELIMIT;
        else {
            *out = p4_b.events[i].meta;
            out->dropped = atomic_load(&p4_lost);
            if (out->payload_size)
                memcpy(data, p4_b.events[i].data, out->payload_size);
            p4_b.et++;
            r = 1;
        }
    }
    av_give(&lock);
    return r;
}
static bool ble_command(ls_lap_ble_command_t *out) {
    if (!av_take(&lock))
        return false;
    bool ready = p4_b.command_ready;
    if (ready) {
        memcpy(out, &p4_b.command, offsetof(ls_lap_ble_command_t, data) + p4_b.command.len);
        p4_b.command_ready = false;
    }
    av_give(&lock);
    return ready;
}
static void ble_event(const lap_ble_event_t *event, const void *data, size_t n) {
    if (!av_take(&lock)) {
        atomic_fetch_add(&p4_lost, 1);
        return;
    }
    if (event->connection == p4_b.connection && p4_b.epoch == atomic_load(&ls_lap_av_epoch) &&
        atomic_load(&p4_canceled_conn) != p4_b.connection) {
        if (event->kind == LAP_BLE_CONNECTED) {
            p4_b.connected = !event->error;
            p4_b.pending = false;
        }
        if (event->kind == LAP_BLE_DISCONNECTED) {
            p4_b.connected = false;
            p4_b.pending = p4_b.pairing = p4_b.info_valid = false;
        }
        if (event->kind == LAP_GATT_COMPLETE || event->kind == LAP_GATT_WRITTEN ||
            event->kind == LAP_GATT_VALUE)
            p4_b.pending = false;
        if (event->kind == LAP_BLE_SECURITY && p4_b.pairing && event->operation == p4_b.operation)
            p4_b.pending = p4_b.pairing = false;
        if (event->kind == LAP_BLE_PASSKEY && p4_b.pairing)
            p4_b.started = ls_lap_av_now(); /* the user's turn starts now */
        if (p4_b.eh - p4_b.et == 16) {
            /* Full. A notification is the one to lose: the next brings the
               value again, while a lost reply (value, written, complete)
               leaves the app waiting on a request forever. The oldest
               notification goes; a new notification with none queued is
               dropped itself; only a queue of replies loses its oldest. */
            atomic_fetch_add(&p4_lost, 1);
            unsigned victim = p4_b.et;
            bool found = false;
            for (unsigned k = p4_b.et; k != p4_b.eh; k++)
                if (p4_b.events[k % 16].meta.kind == LAP_GATT_NOTIFY) { victim = k; found = true; break; }
            if (!found && event->kind == LAP_GATT_NOTIFY) {
                av_give(&lock);
                return;
            }
            for (unsigned k = victim; k != p4_b.et; k--)          /* close the gap from the tail side */
                p4_b.events[k % 16] = p4_b.events[(k - 1) % 16];
            p4_b.et++;
        }
        unsigned i = p4_b.eh++ % 16;
        p4_b.events[i].meta = *event;
        p4_b.events[i].meta.size = sizeof(*event);
        p4_b.events[i].meta.version = 1;
        if (n > LAP_GATT_VALUE_MAX) {
            n = 0;
            p4_b.events[i].meta.error = LAP_IO_ELIMIT;
        }
        p4_b.events[i].meta.payload_size = n;
        if (n)
            memcpy(p4_b.events[i].data, data, n);
    }
    av_give(&lock);
}
static void ble_info(int handle, const lap_ble_conn_info_t *info) {
    if (!av_take(&lock))
        return;
    if (handle == p4_b.connection && p4_b.epoch == atomic_load(&ls_lap_av_epoch) &&
        atomic_load(&p4_canceled_conn) != p4_b.connection) {
        p4_b.info = *info;
        p4_b.info_valid = true;
    }
    av_give(&lock);
}
#if !LAP_DEVICE
int ls_lap_ble_lease(bool on) {
    (void)on;
    return 0;
}
static int sim_error;
static bool sim_silence, sim_subscribed;
/* The synthetic peer: a value for reads (bleread=HEX or blereadlen=N), reads
   refused until paired (bleauth=1), passkey style (blepasskey=2 INPUT, 4
   NUMCMP, 0 just works). Its code is 123456. */
static uint8_t sim_value[LAP_GATT_VALUE_MAX] = {87};
static size_t sim_value_len = 1;
static bool sim_auth, sim_encrypted, sim_authenticated, sim_bonded, sim_waiting;
static int sim_passkey = LAP_BLE_PASSKEY_NUMCMP, sim_pair_op;
static unsigned sim_pair_flags;
static uint16_t sim_mtu = 23;
static uint8_t sim_peer[6], sim_peer_type;
static size_t sim_hex(const char *p, uint8_t *out, size_t cap) {
    size_t n = 0;
    unsigned v;
    while (n < cap && p[0] && p[1] && sscanf(p, "%2x", &v) == 1) {
        out[n++] = v;
        p += 2;
    }
    return n;
}
static void sim_info(int h) {
    lap_ble_conn_info_t info = {.size = sizeof(info), .version = 1, .mtu = sim_mtu, .rssi = -61,
                                .phy_tx = 1, .phy_rx = 1, .peer_addr_type = sim_peer_type,
                                .encrypted = sim_encrypted, .authenticated = sim_authenticated,
                                .bonded = sim_bonded, .key_size = sim_encrypted ? 16 : 0,
                                .interval = 24, .latency = 0, .timeout = 400};
    memcpy(info.peer_addr, sim_peer, 6);
    ls_lap_ble_info(h, &info);
}
static void sim_security(int h, int op, int kind, int error, uint8_t action, uint32_t passkey) {
    lap_ble_security_t s = {.size = sizeof(s), .version = 1, .encrypted = sim_encrypted,
                            .authenticated = sim_authenticated, .bonded = sim_bonded,
                            .key_size = sim_encrypted ? 16 : 0, .action = action, .passkey = passkey};
    lap_ble_event_t e = {.connection = h, .operation = op, .kind = kind, .error = error};
    if (kind == LAP_BLE_SECURITY)
        sim_info(h);
    ls_lap_ble_event(&e, &s, sizeof(s));
}
static void sim_paired(int h, bool authenticated) {
    sim_encrypted = true;
    sim_authenticated = authenticated;
    sim_bonded = sim_pair_flags & LAP_BLE_PAIR_BOND;
    sim_waiting = false;
    sim_security(h, sim_pair_op, LAP_BLE_SECURITY, 0, 0, 0);
}
void ls_lap_ble_script(const char *c) {
    if (!strncmp(c, "gatterror=", 10)) {
        sim_error = atoi(c + 10);
        return;
    }
    if (!strncmp(c, "blesilence=", 11)) {
        sim_silence = atoi(c + 11) != 0;
        return;
    }
    if (!strncmp(c, "bledisconnect", 13)) {
        lap_ble_event_t e = {.connection = p4_b.connection, .kind = LAP_BLE_DISCONNECTED, .error = LAP_IO_EIO};
        ls_lap_ble_event(&e, NULL, 0);
        return;
    }
    if (!strncmp(c, "blenotify=", 10)) {
        uint8_t data[LAP_GATT_VALUE_MAX];
        size_t n = sim_hex(c + 10, data, sizeof(data));
        lap_ble_event_t e = {.connection = p4_b.connection, .kind = LAP_GATT_NOTIFY, .attribute = 3};
        ls_lap_ble_event(&e, data, n);
        return;
    }
    if (!strncmp(c, "blenotifylen=", 13)) { /* N bytes counting from 0, up to mtu - 3 */
        uint8_t data[LAP_GATT_VALUE_MAX];
        size_t n = (size_t)atoi(c + 13);
        if (n > (size_t)sim_mtu - 3u)
            n = (size_t)sim_mtu - 3u;
        for (size_t i = 0; i < n; i++)
            data[i] = (uint8_t)i;
        lap_ble_event_t e = {.connection = p4_b.connection, .kind = LAP_GATT_NOTIFY, .attribute = 3};
        ls_lap_ble_event(&e, data, n);
        return;
    }
    if (!strncmp(c, "bleread=", 8)) {
        sim_value_len = sim_hex(c + 8, sim_value, sizeof(sim_value));
        return;
    }
    if (!strncmp(c, "blereadlen=", 11)) {
        size_t n = (size_t)atoi(c + 11);
        sim_value_len = n < sizeof(sim_value) ? n : sizeof(sim_value);
        for (size_t i = 0; i < sim_value_len; i++)
            sim_value[i] = (uint8_t)(i * 7);
        return;
    }
    if (!strncmp(c, "bleauth=", 8)) {
        sim_auth = atoi(c + 8) != 0;
        return;
    }
    if (!strncmp(c, "blepasskey=", 11)) {
        sim_passkey = atoi(c + 11);
        return;
    }
    if (!strncmp(c, "blesecreq=", 10)) { /* the peer asks to pair; authreq bits */
        lap_ble_security_t s = {.size = sizeof(s), .version = 1, .encrypted = sim_encrypted,
                                .authreq = (uint8_t)atoi(c + 10)};
        lap_ble_event_t e = {.connection = p4_b.connection, .kind = LAP_BLE_SECURITY_REQUEST};
        ls_lap_ble_event(&e, &s, sizeof(s));
        return;
    }
    if (!strncmp(c, "blead=", 6)) {
        int rssi = atoi(c + 6);
        const char *p = strchr(c + 6, ',');
        if (!p)
            return;
        p++;
        uint8_t payload[LAP_BLE_AD_MAX];
        size_t n = sim_hex(p, payload, sizeof(payload));
        uint8_t addr[] = {1, 2, 3, 4, 5, (uint8_t)(ls_lap_av_now() / 1000)};
        ls_lap_ble_advert_push(addr, 1, rssi, false, payload, n, ls_lap_av_now());
    }
}
#ifndef LS_LAP_BLE_NO_SIM
static void simulate(void) {
    ls_lap_ble_command_t c;
    if (!ls_lap_ble_command(&c))
        return;
    lap_ble_event_t ev = {.connection = c.handle, .operation = c.operation, .attribute = c.attribute};
    if (sim_error && c.kind != LS_BLE_CMD_CLOSE && c.kind != LS_BLE_CMD_PASSKEY) {
        ev.kind = c.kind == LS_BLE_CMD_CONNECT ? LAP_BLE_CONNECTED
                  : c.kind == LS_BLE_CMD_PAIR  ? LAP_BLE_SECURITY
                                               : LAP_GATT_COMPLETE;
        ev.error = sim_error;
        ls_lap_ble_event(&ev, NULL, 0);
        return;
    }
    switch (c.kind) {
    case LS_BLE_CMD_CONNECT:
        sim_mtu = c.flags & LS_BLE_CONNECT_WIDE ? 247 : 23;
        sim_encrypted = sim_authenticated = sim_bonded = sim_waiting = false;
        memcpy(sim_peer, c.addr, 6);
        sim_peer_type = c.addr_type;
        sim_info(c.handle);
        ev.kind = LAP_BLE_CONNECTED;
        ls_lap_ble_event(&ev, NULL, 0);
        break;
    case LS_BLE_CMD_DISCOVER:
        ev.kind = c.attribute ? LAP_GATT_CHARACTERISTIC : LAP_GATT_SERVICE;
        ev.attribute = c.attribute ? 3 : 1;
        ev.end_handle = 8;
        ev.properties = 0x1a;
        ev.uuid.length = 2;
        ev.uuid.bytes[0] = c.attribute ? 0x19 : 0x0f;
        ev.uuid.bytes[1] = c.attribute ? 0x2a : 0x18;
        ls_lap_ble_event(&ev, NULL, 0);
        ev.kind = LAP_GATT_COMPLETE;
        ls_lap_ble_event(&ev, NULL, 0);
        break;
    case LS_BLE_CMD_DESCRIPTORS:
        ev.kind = LAP_GATT_DESCRIPTOR;
        ev.attribute = 4;
        ev.uuid.length = 2;
        ev.uuid.bytes[0] = 2;
        ev.uuid.bytes[1] = 0x29;
        ls_lap_ble_event(&ev, NULL, 0);
        ev.kind = LAP_GATT_COMPLETE;
        ls_lap_ble_event(&ev, NULL, 0);
        break;
    case LS_BLE_CMD_READ:
        ev.kind = LAP_GATT_VALUE;
        if (sim_auth && !sim_encrypted) {
            ev.error = LAP_IO_EAUTH;
            ls_lap_ble_event(&ev, NULL, 0);
        } else
            ls_lap_ble_event(&ev, sim_value, sim_value_len);
        break;
    case LS_BLE_CMD_WRITE:
        if (c.attribute == 4 && c.len == 2) {
            sim_subscribed = c.data[0] != 0;
            p4_b.notify_at = ls_lap_av_now();
        }
        ev.kind = LAP_GATT_WRITTEN;
        ls_lap_ble_event(&ev, NULL, 0);
        break;
    case LS_BLE_CMD_WRITE_CMD:
        ev.kind = LAP_GATT_WRITTEN;
        ev.error = c.len > sim_mtu - 3u ? LAP_IO_ELIMIT : 0;
        ls_lap_ble_event(&ev, NULL, 0);
        break;
    case LS_BLE_CMD_PAIR:
        sim_pair_op = c.operation;
        sim_pair_flags = c.flags;
        if (sim_encrypted && (sim_authenticated || !(c.flags & LAP_BLE_PAIR_MITM)))
            sim_security(c.handle, c.operation, LAP_BLE_SECURITY, 0, 0, 0);
        else if (!(c.flags & LAP_BLE_PAIR_MITM) || !sim_passkey)
            sim_paired(c.handle, false);
        else {
            sim_waiting = true;
            sim_security(c.handle, c.operation, LAP_BLE_PASSKEY, 0, (uint8_t)sim_passkey,
                         sim_passkey == LAP_BLE_PASSKEY_NUMCMP ? 123456 : 0);
        }
        break;
    case LS_BLE_CMD_PASSKEY:
        if (!sim_waiting || c.flags != sim_passkey)
            sim_security(c.handle, sim_pair_op, LAP_BLE_SECURITY, LAP_IO_EAUTH, 0, 0);
        else if (c.flags == LAP_BLE_PASSKEY_NUMCMP ? c.value == 1 : c.value == 123456)
            sim_paired(c.handle, true);
        else {
            sim_waiting = false;
            sim_security(c.handle, sim_pair_op, LAP_BLE_SECURITY, LAP_IO_EAUTH, 0, 0);
        }
        break;
    case LS_BLE_CMD_FORGET:
        sim_bonded = false;
        sim_info(c.handle);
        ev.kind = LAP_GATT_COMPLETE;
        ls_lap_ble_event(&ev, NULL, 0);
        break;
    case LS_BLE_CMD_CLOSE:
        ls_lap_ble_closed(c.handle);
        sim_subscribed = false;
        sim_encrypted = sim_authenticated = sim_bonded = sim_waiting = false;
        break;
    }
}
#endif /* !LS_LAP_BLE_NO_SIM */
#endif
bool ls_lap_ble_service(void) {
    if (!av_take(&lock))
        return true;
    bool stale = p4_b.epoch != atomic_load(&ls_lap_av_epoch);
    if (p4_b.closing && atomic_load(&p4_closed) == p4_b.closing)
        p4_b.closing = 0;
    if (p4_b.scan && (stale || atomic_load(&p4_canceled_scan) == p4_b.scan)) {
        atomic_store(&p4_scan_published, 0);
        p4_b.scan = 0;
        p4_b.ah = p4_b.at = 0;
    }
    if (p4_b.connection && (stale || atomic_load(&p4_canceled_conn) == p4_b.connection)) {
        /* Disconnect is retained until the host takes it, before lease release. */
        set_command(LS_BLE_CMD_CLOSE, p4_b.connection, 0, 0, 0);
        p4_b.command_ready = true;
        p4_b.closing = p4_b.connection;
        atomic_store(&p4_conn_published, 0);
        p4_b.connection = 0;
        p4_b.pending = p4_b.connected = p4_b.pairing = p4_b.info_valid = false;
        p4_b.eh = p4_b.et = 0;
    }
    bool want = p4_b.scan || p4_b.connection || p4_b.closing || p4_b.command_ready;
    bool change = want != p4_b.lease;
    if(!want && !ls_lap_peripheral_reserved()) ls_lap_resource_claim(LS_LAP_LEASE_BLE,false);
    av_give(&lock);
    if (change) {
        int r = ls_lap_ble_lease(want);
        if (av_take(&lock)) {
            p4_b.lease = want && !r;
            p4_b.status = av_status(r ? LAP_OPERATION_DONE : LAP_OPERATION_READY, r);
            av_give(&lock);
        }
    }
#if !LAP_DEVICE && !defined(LS_LAP_BLE_NO_SIM)
    simulate();
    if (sim_subscribed && p4_b.connected && ls_lap_av_now() - p4_b.notify_at >= 1000) {
        p4_b.notify_at = ls_lap_av_now();
        uint8_t level = 86;
        lap_ble_event_t e = {.connection = p4_b.connection, .kind = LAP_GATT_NOTIFY, .attribute = 3};
        ls_lap_ble_event(&e, &level, 1);
    }
    if (av_take(&lock)) {
        bool advert = !sim_silence && p4_b.scan && ls_lap_av_now() - p4_b.last_ad >= 120;
        if (advert)
            p4_b.last_ad = ls_lap_av_now();
        av_give(&lock);
        if (advert) {
            static const uint8_t addr[] = {1, 2, 3, 4, 5, 6};
            static const uint8_t ad[] = {2, 1, 6, 3, 3, 0x0f, 0x18, 4, 0xff, 0x34, 0x12, 0x01};
            ls_lap_ble_advert_push(addr, 0, -50 - (int)(ls_lap_av_now() % 20), false, ad, sizeof(ad),
                                   ls_lap_av_now());
        }
    }
#endif
    if (av_take(&lock)) {
        if (p4_b.pending && ls_lap_av_now() - p4_b.started > (p4_b.timeout_ms ? p4_b.timeout_ms : 12000)) {
            if (p4_b.eh - p4_b.et == 16)
                p4_b.et++;
            unsigned i = p4_b.eh++ % 16;
            p4_b.events[i].meta = (lap_ble_event_t){.size = sizeof(lap_ble_event_t),
                                                 .version = 1,
                                                 .connection = p4_b.connection,
                                                 .operation = p4_b.operation,
                                                 .kind = p4_b.pairing ? LAP_BLE_SECURITY : LAP_GATT_COMPLETE,
                                                 .error = LAP_IO_ETIMEOUT};
            p4_b.pending = p4_b.pairing = false;
            av_cancel(&p4_canceled_conn, p4_b.connection);
        }
        av_give(&lock);
    }
    return want;
}

void ls_lap_ble_closed(int h) { unsigned old=ls_lap_producer_select(LS_LAP_LEASE_BLE); ble_closed(h); ls_lap_producer_restore(old); }
bool ls_lap_ble_command(ls_lap_ble_command_t *out) { unsigned old=ls_lap_producer_select(LS_LAP_LEASE_BLE); bool ready=ble_command(out); ls_lap_producer_restore(old); return ready; }
void ls_lap_ble_event(const lap_ble_event_t *event,const void *data,size_t n) { unsigned old=ls_lap_producer_select(LS_LAP_LEASE_BLE); ble_event(event,data,n); ls_lap_producer_restore(old); }
void ls_lap_ble_info(int handle,const lap_ble_conn_info_t *info) { unsigned old=ls_lap_producer_select(LS_LAP_LEASE_BLE); ble_info(handle,info); ls_lap_producer_restore(old); }
void ls_lap_ble_advert_push(const uint8_t *addr,uint8_t type,int rssi,bool response,const void *data,size_t n,uint32_t at) { unsigned old=ls_lap_producer_select(LS_LAP_LEASE_BLE); ble_advert_push(addr,type,rssi,response,data,n,at); ls_lap_producer_restore(old); }

#include "ls_lap_usage.h"
ls_lap_usage_t ls_lap_ble_usage(unsigned s) {
    return (ls_lap_usage_t){sizeof(p4_b_slots[s]),(atomic_load(&p4_scan_published_slots[s])>0)+(atomic_load(&p4_conn_published_slots[s])>0)};
}
