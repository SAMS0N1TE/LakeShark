/* Bounded HOGP central. All GAP/GATT, parser and input state runs on the
 * existing NimBLE host queue. No task, stack or second BLE initialization. */
#include "ble_hid.h"
#include "ble_hid_input.h"
#include "ble_hid_report.h"
#include "ble_link.h"
#include "esp_attr.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "ls_board.h"
#include "nimble/nimble_port.h"
#include "nvs.h"
#include "os/os_mbuf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SERVICES = 4, CHARS = 32, EVENTS = 64, COMMANDS = 12, WATCH = 64 };
typedef struct {
    uint16_t start, end, map, mode, info, control, raw_offset, raw_len;
    hid_map_t compiled;
} service_t;
typedef struct {
    uint16_t def, value, end, uuid, cccd, ref;
    uint8_t service, id, type, properties;
    bool subscribed;
} characteristic_t;
typedef struct {
    hid_device_t view;
    ble_addr_t identity;
    uint8_t preference;
    uint8_t blocked;
    uint32_t fingerprint;
} saved_t;
typedef struct {
    hid_device_t view;
    uint16_t conn;
    uint32_t generation, op;
    bool pairing, just_works, security_done, closing;
    int saved;
    uint8_t ns, nc, si, ci, phase, selected[SERVICES];
    service_t services[SERVICES];
    characteristic_t chars[CHARS];
    uint8_t map[HID_MAP_MAX];
    uint16_t map_len, map_total;
    unsigned attempts;
    uint64_t deadline, retry_at;
} peer_t;
typedef enum {
    CMD_SCAN,
    CMD_STOP,
    CMD_PAIR,
    CMD_CANCEL,
    CMD_CONNECT,
    CMD_DISCONNECT,
    CMD_FORGET,
    CMD_FORGET_ALL,
    CMD_RECONNECT,
    CMD_PROTOCOL,
    CMD_INSPECT,
    CMD_RELEASE_INPUT
} command_kind_t;
typedef struct {
    uint32_t op, id;
    unsigned arg;
    uint8_t kind;
} command_t;
typedef struct {
    peer_t peers[HID_CONNECTIONS];
    ble_store_write_fn *store_write;
    saved_t saved[HID_BONDS];
    hid_device_t candidates[HID_CANDIDATES], published[HID_CANDIDATES + HID_BONDS];
    size_t npublished;
    unsigned reconnect_cursor;
    uint32_t version, scan_generation, next_generation, event_seq, next_op, forget_all_op, scan_op;
    uint64_t scan_until;
    bool initialized, synced, exclusive, watch;
    command_t commands[COMMANDS];
    unsigned cr, cw;
    hid_event_t events[EVENTS];
    ls_input_ev_t watched[WATCH];
    unsigned wr, ww;
    hid_stats_t stats, published_stats;
    hid_input_t input;
    hid_input_state_t published_input;
} hid_context_t;
static EXT_RAM_BSS_ATTR hid_context_t s;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static struct ble_npl_callout tick;
static uint64_t now(void)
{
    return esp_timer_get_time();
}
static uint32_t ms(void)
{
    return (uint32_t)(now() / 1000);
}
static bool addr_eq(const ble_addr_t *a, const ble_addr_t *b)
{
    return a->type == b->type && !memcmp(a->val, b->val, 6);
}
static ble_addr_t address(const hid_device_t *d)
{
    ble_addr_t a = {.type = d->address_type};
    memcpy(a.val, d->address, 6);
    return a;
}
static void set_address(hid_device_t *d, const ble_addr_t *a)
{
    memcpy(d->address, a->val, 6);
    d->address_type = a->type;
}
static peer_t *peer_id(uint32_t id)
{
    for (unsigned i = 0; i < HID_CONNECTIONS; i++)
        if (s.peers[i].view.id == id && id)
            return &s.peers[i];
    return NULL;
}
static hid_device_t *candidate(uint32_t id)
{
    for (unsigned i = 0; i < HID_CANDIDATES; i++)
        if (s.candidates[i].id == id && id)
            return &s.candidates[i];
    return NULL;
}
static int saved_id(uint32_t id)
{
    for (unsigned i = 0; i < HID_BONDS; i++)
        if (s.saved[i].view.id == id && id)
            return i;
    return -1;
}
static void event(peer_t *p, uint32_t op, uint32_t id, unsigned kind, unsigned error,
                  uint32_t passkey)
{
    hid_event_t e = {.operation = op,
                     .device = p ? p->view.id : id,
                     .generation = p ? p->generation : 0,
                     .timestamp_us = now(),
                     .kind = kind,
                     .state = p ? p->view.state : 0,
                     .error = error,
                     .passkey = passkey};
    portENTER_CRITICAL(&mux);
    if (kind == HID_EV_PASSKEY_EXPIRED && p)
        for (unsigned i = 0; i < EVENTS; i++)
            if (s.events[i].generation == p->generation)
                s.events[i].passkey = 0;
    e.sequence = ++s.event_seq;
    s.events[(e.sequence - 1) % EVENTS] = e;
    if (e.sequence > EVENTS)
        s.stats.event_drops++;
    portEXIT_CRITICAL(&mux);
}
static void state(peer_t *p, unsigned st, unsigned error)
{
    p->view.state = st;
    p->view.error = error;
    event(p, p->op, 0, error ? HID_EV_ERROR : HID_EV_STATE, error, 0);
}
static bool input_sink(const ls_input_ev_t *e, void *ctx)
{
    (void)ctx;
    bool ok = ls_input_post(e);
    portENTER_CRITICAL(&mux);
    if (s.watch && ok) {
        unsigned next = (s.ww + 1) % WATCH;
        if (next != s.wr) {
            s.watched[s.ww] = *e;
            s.ww = next;
        }
    }
    portEXIT_CRITICAL(&mux);
    return ok;
}
static void release(peer_t *p)
{
    hid_input_release(&s.input, p - s.peers, ms(), input_sink, NULL);
}
static void head_defaults(void)
{
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;
    s.exclusive = false;
}
static void finish_security(peer_t *p)
{
    if (p->pairing) {
        event(p, p->op, 0, HID_EV_PASSKEY_EXPIRED, HID_OK, 0);
        p->pairing = false;
        head_defaults();
    }
}
static void fail(peer_t *p, unsigned error)
{
    bool connecting = p->view.state == HID_CONNECTING;
    release(p);
    if (p->saved >= 0 && (error == HID_AUTHENTICATION_FAILED || error == HID_UNSUPPORTED_REPORT)) {
        s.saved[p->saved].blocked = error;
        s.saved[p->saved].view.state =
            error == HID_AUTHENTICATION_FAILED ? HID_NEEDS_PAIRING : HID_UNSUPPORTED;
        s.saved[p->saved].view.error = error;
    }
    state(p, error == HID_AUTHENTICATION_FAILED ? HID_NEEDS_PAIRING : HID_UNSUPPORTED, error);
    p->closing = true;
    if (p->conn != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(p->conn, BLE_ERR_REM_USER_CONN_TERM);
    else {
        if (connecting && ble_gap_conn_active())
            ble_gap_conn_cancel();
        else
            finish_security(p);
        p->deadline = 0;
    }
}
/* Metadata only. NimBLE owns keys in nimble_bond. No eviction callback. */
static int persist(void)
{
    nvs_handle_t n;
    esp_err_t rc = nvs_open("ls_hid", NVS_READWRITE, &n);
    if (rc)
        return rc;
    rc = nvs_set_blob(n, "devices_v1", s.saved, sizeof(s.saved));
    if (!rc)
        rc = nvs_commit(n);
    nvs_close(n);
    return rc;
}
static int store_status(struct ble_store_status_event *ev, void *arg)
{
    (void)ev;
    (void)arg; /* Never let ble_store_util_status_rr evict another bond. */
    for (unsigned i = 0; i < HID_CONNECTIONS; i++)
        if (s.peers[i].pairing)
            s.peers[i].view.error = HID_BOND_STORE_FULL;
    return BLE_HS_ESTORE_CAP;
}
static int store_write(int type, const union ble_store_value *value)
{
    int rc = s.store_write ? s.store_write(type, value) : BLE_HS_ESTORE_FAIL;
    if (rc)
        for (unsigned i = 0; i < HID_CONNECTIONS; i++)
            if (s.peers[i].pairing)
                s.peers[i].view.error = HID_BOND_STORE_FULL;
    return rc;
}
static void publish(void)
{
    portENTER_CRITICAL(&mux);
    size_t n = 0;
    for (unsigned i = 0; i < HID_CANDIDATES; i++)
        if (s.candidates[i].id)
            s.published[n++] = s.candidates[i];
    for (unsigned i = 0; i < HID_BONDS; i++)
        if (s.saved[i].view.id) {
            hid_device_t v = s.saved[i].view;
            peer_t *p = peer_id(v.id);
            if (p)
                v = p->view;
            s.published[n++] = v;
        }
    for (unsigned i = 0; i < HID_CONNECTIONS; i++) {
        peer_t *p = &s.peers[i];
        if (p->view.id && saved_id(p->view.id) < 0)
            for (size_t j = 0; j < n; j++)
                if (s.published[j].id == p->view.id)
                    s.published[j] = p->view;
    }
    for (size_t i = 0; i < n; i++)
        s.published[i].version = s.version + 1;
    s.npublished = n;
    s.version++;
    s.stats.queue_drops = s.input.dropped;
    s.stats.psram_bytes = sizeof(s);
    s.published_stats = s.stats;
    memcpy(s.published_input.keys, s.input.held, sizeof(s.published_input.keys));
    s.published_input.buttons = s.input.held_buttons;
    s.published_input.px = s.input.x;
    s.published_input.py = s.input.y;
    s.published_input.sequence = s.input.seq;
    portEXIT_CRITICAL(&mux);
}
/* Callback tokens include slot + generation, never just a reused handle. */
static void *token(peer_t *p)
{
    return (void *)(uintptr_t)((p->generation << 2) | (unsigned)(p - s.peers));
}
static peer_t *valid(uint16_t conn, void *arg)
{
    uintptr_t t = (uintptr_t)arg;
    unsigned slot = t & 3;
    if (slot >= HID_CONNECTIONS)
        return NULL;
    peer_t *p = &s.peers[slot];
    return p->generation == (t >> 2) && p->conn == conn && !p->closing ? p : NULL;
}
static void discover_next(peer_t *p);
static int on_services(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_svc *svc, void *arg);
static int on_chars(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr,
                    void *arg);
static int on_descriptors(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr,
                          const struct ble_gatt_dsc *dsc, void *arg);
static int on_read(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr,
                   void *arg);
static int on_write(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr,
                    void *arg);
static void start_discovery(peer_t *p)
{
    finish_security(p);
    p->security_done = true;
    p->ns = p->nc = p->si = p->ci = p->phase = 0;
    p->map_total = 0;
    p->view.keyboard = p->view.mouse = p->view.consumer = p->view.wheel = false;
    p->view.fingerprint = 0;
    p->view.unsupported = 0;
    p->view.protocol = HID_REPORT;
    state(p, HID_DISCOVERING, HID_OK);
    p->deadline = now() + 30000000;
    ble_uuid16_t uuid = BLE_UUID16_INIT(0x1812);
    if (ble_gattc_disc_svc_by_uuid(p->conn, &uuid.u, on_services, token(p)))
        fail(p, HID_IO_ERROR);
}
static int on_services(uint16_t c, const struct ble_gatt_error *e, const struct ble_gatt_svc *v,
                       void *arg)
{
    peer_t *p = valid(c, arg);
    if (!p)
        return 0;
    if (!e->status) {
        if (p->ns == SERVICES) {
            fail(p, HID_UNSUPPORTED_REPORT);
            return 0;
        }
        p->services[p->ns++] = (service_t){.start = v->start_handle, .end = v->end_handle};
    } else if (e->status == BLE_HS_EDONE) {
        if (!p->ns)
            fail(p, HID_UNSUPPORTED_REPORT);
        else
            discover_next(p);
    } else
        fail(p, HID_IO_ERROR);
    return 0;
}
static int on_chars(uint16_t c, const struct ble_gatt_error *e, const struct ble_gatt_chr *v,
                    void *arg)
{
    peer_t *p = valid(c, arg);
    if (!p)
        return 0;
    if (!e->status) {
        if (ble_uuid_u16(&v->uuid.u) == 0x2a4d) {
            unsigned reports = 0;
            for (unsigned i = 0; i < p->nc; i++)
                reports += p->chars[i].uuid == 0x2a4d;
            if (reports == HID_REPORTS_MAX) {
                fail(p, HID_UNSUPPORTED_REPORT);
                return 0;
            }
        }
        if (p->nc == CHARS) {
            fail(p, HID_UNSUPPORTED_REPORT);
            return 0;
        }
        if (p->nc && p->chars[p->nc - 1].service == p->si)
            p->chars[p->nc - 1].end = v->def_handle - 1;
        characteristic_t *ch = &p->chars[p->nc++];
        *ch = (characteristic_t){.def = v->def_handle,
                                 .value = v->val_handle,
                                 .end = p->services[p->si].end,
                                 .uuid = ble_uuid_u16(&v->uuid.u),
                                 .service = p->si,
                                 .properties = v->properties};
        service_t *svc = &p->services[p->si];
        switch (ch->uuid) {
        case 0x2a4b:
            svc->map = ch->value;
            break;
        case 0x2a4e:
            svc->mode = ch->value;
            break;
        case 0x2a4a:
            svc->info = ch->value;
            break;
        case 0x2a4c:
            svc->control = ch->value;
            break;
        default:
            break;
        }
    } else if (e->status == BLE_HS_EDONE) {
        p->si++;
        discover_next(p);
    } else
        fail(p, HID_IO_ERROR);
    return 0;
}
static int on_descriptors(uint16_t c, const struct ble_gatt_error *e, uint16_t chr,
                          const struct ble_gatt_dsc *d, void *arg)
{
    (void)chr;
    peer_t *p = valid(c, arg);
    if (!p)
        return 0;
    if (!e->status) {
        uint16_t u = ble_uuid_u16(&d->uuid.u);
        if (u == 0x2902)
            p->chars[p->ci].cccd = d->handle;
        else if (u == 0x2908)
            p->chars[p->ci].ref = d->handle;
    } else if (e->status == BLE_HS_EDONE) {
        p->ci++;
        discover_next(p);
    } else
        fail(p, HID_IO_ERROR);
    return 0;
}
static bool matching_input(peer_t *p, characteristic_t *ch)
{
    if (!(ch->properties & BLE_GATT_CHR_PROP_NOTIFY))
        return false;
    if (p->selected[ch->service] == HID_BOOT)
        return ch->uuid == 0x2a22 || ch->uuid == 0x2a33;
    if (ch->uuid != 0x2a4d || ch->type != 1)
        return false;
    hid_map_t *map = &p->services[ch->service].compiled;
    for (unsigned i = 0; i < map->nreports; i++)
        if (map->reports[i].id == ch->id && map->reports[i].type == 1)
            return true;
    return false;
}
static void capabilities(peer_t *p)
{
    for (unsigned i = 0; i < p->ns; i++) {
        hid_map_t *m = &p->services[i].compiled;
        p->view.fingerprint ^= m->fingerprint;
        p->view.unsupported += m->unsupported;
        for (unsigned j = 0; j < m->nfields; j++) {
            hid_field_t *f = &m->fields[j];
            if (f->type != 1)
                continue;
            if (f->page == 7)
                p->view.keyboard = true;
            if (f->page == 9) {
                p->view.mouse = true;
                unsigned buttons = f->usage_max > 8 ? 8 : f->usage_max;
                if (buttons > p->view.buttons)
                    p->view.buttons = buttons;
            }
            if (f->page == 1 && (f->flags & 4)) {
                if (f->usage == 0x30 || f->usage == 0x31)
                    p->view.mouse = true;
                if (f->usage == 0x38)
                    p->view.wheel = true;
            }
            if (f->page == 12)
                p->view.consumer = true;
        }
    }
    for (unsigned i = 0; i < p->nc; i++)
        if (p->chars[i].subscribed) {
            if (p->chars[i].uuid == 0x2a22)
                p->view.keyboard = true;
            if (p->chars[i].uuid == 0x2a33)
                p->view.mouse = true;
        }
    p->view.raw_state = true;
    for (unsigned i = 0; i < p->ns; i++)
        if (p->selected[i] == HID_BOOT)
            p->view.protocol = HID_BOOT;
    struct ble_gap_conn_desc desc;
    if (!ble_gap_conn_find(p->conn, &desc)) {
        p->view.interval = desc.conn_itvl;
        p->view.latency = desc.conn_latency;
        p->view.timeout = desc.supervision_timeout;
    }
}
static void discover_next(peer_t *p)
{
    int rc = 0;
    for (;;)
        switch (p->phase) {
        case 0:
            if (p->si < p->ns) {
                service_t *v = &p->services[p->si];
                rc = ble_gattc_disc_all_chrs(p->conn, v->start, v->end, on_chars, token(p));
                goto pending;
            }
            p->phase = 1;
            p->ci = 0;
            break;
        case 1:
            while (p->ci < p->nc) {
                characteristic_t *ch = &p->chars[p->ci];
                if ((ch->uuid == 0x2a4d || ch->uuid == 0x2a22 || ch->uuid == 0x2a33) &&
                    ch->end > ch->value) {
                    rc = ble_gattc_disc_all_dscs(p->conn, ch->value, ch->end, on_descriptors,
                                                 token(p));
                    goto pending;
                }
                p->ci++;
            }
            p->phase = 2;
            p->si = 0;
            break;
        case 2:
            if (p->si < p->ns) {
                service_t *v = &p->services[p->si];
                p->map_len = 0;
                if (v->map) {
                    rc = ble_gattc_read_long(p->conn, v->map, 0, on_read, token(p));
                    goto pending;
                }
                if (p->saved >= 0 && s.saved[p->saved].preference == HID_REPORT) {
                    fail(p, HID_UNSUPPORTED_REPORT);
                    return;
                }
                p->selected[p->si++] = HID_BOOT;
                break;
            }
            p->phase = 3;
            p->ci = 0;
            break;
        case 3:
            while (p->ci < p->nc) {
                characteristic_t *ch = &p->chars[p->ci];
                if (ch->uuid == 0x2a4d) {
                    if (!ch->ref) {
                        fail(p, HID_UNSUPPORTED_REPORT);
                        return;
                    }
                    rc = ble_gattc_read(p->conn, ch->ref, on_read, token(p));
                    goto pending;
                }
                p->ci++;
            }
            p->phase = 7;
            p->si = 0;
            break;
        case 7:
            if (p->si < p->ns) {
                if (p->services[p->si].info) {
                    rc = ble_gattc_read(p->conn, p->services[p->si].info, on_read, token(p));
                    goto pending;
                }
                p->si++;
                break;
            }
            p->phase = 4;
            p->si = 0;
            break;
        case 4:
            if (p->si < p->ns) {
                service_t *v = &p->services[p->si];
                if (v->mode) {
                    uint8_t mode = p->selected[p->si] == HID_BOOT ? 0 : 1;
                    bool no_rsp = false;
                    for (unsigned i = 0; i < p->nc; i++)
                        if (p->chars[i].value == v->mode)
                            no_rsp = p->chars[i].properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP;
                    if (no_rsp) {
                        rc = ble_gattc_write_no_rsp_flat(p->conn, v->mode, &mode, 1);
                        if (rc)
                            goto pending;
                        p->si++;
                        break;
                    }
                    rc = ble_gattc_write_flat(p->conn, v->mode, &mode, 1, on_write, token(p));
                    goto pending;
                }
                if (p->selected[p->si] == HID_BOOT) {
                    fail(p, HID_UNSUPPORTED_REPORT);
                    return;
                }
                p->si++;
                break;
            }
            p->phase = 5;
            p->ci = 0;
            state(p, HID_SUBSCRIBING, HID_OK);
            break;
        case 5:
            while (p->ci < p->nc) {
                characteristic_t *ch = &p->chars[p->ci];
                if (matching_input(p, ch)) {
                    if (!ch->cccd) {
                        fail(p, HID_UNSUPPORTED_REPORT);
                        return;
                    }
                    uint8_t enable[2] = {1, 0};
                    rc = ble_gattc_write_flat(p->conn, ch->cccd, enable, 2, on_write, token(p));
                    goto pending;
                }
                p->ci++;
            }
            {
                unsigned n = 0;
                for (unsigned i = 0; i < p->nc; i++)
                    n += p->chars[i].subscribed;
                if (n > HID_REPORTS_MAX) {
                    fail(p, HID_UNSUPPORTED_REPORT);
                    return;
                }
                if (!n) {
                    fail(p, HID_UNSUPPORTED_REPORT);
                    return;
                }
                capabilities(p);
                p->deadline = 0;
                state(p, HID_READY, HID_OK);
                event(p, p->op, 0, HID_EV_CAPABILITIES, HID_OK, 0);
                if (p->saved >= 0) {
                    saved_t *saved = &s.saved[p->saved];
                    saved->view = p->view;
                    saved->view.state = HID_DISCONNECTED;
                    saved->view.encrypted = false;
                    set_address(&saved->view, &saved->identity);
                    persist();
                }
                p->phase = 6;
                return;
            }
        default:
            return;
        }
pending:
    if (rc)
        fail(p, HID_IO_ERROR);
}
static int on_read(uint16_t c, const struct ble_gatt_error *e, struct ble_gatt_attr *a, void *arg)
{
    peer_t *p = valid(c, arg);
    if (!p)
        return 0;
    if (p->phase == 2) {
        if (!e->status) {
            unsigned n = OS_MBUF_PKTLEN(a->om);
            if (a->offset != p->map_len ||
                n > (unsigned)(HID_MAP_MAX - p->map_total - p->map_len)) {
                fail(p, HID_UNSUPPORTED_REPORT);
                return BLE_HS_EAPP;
            }
            os_mbuf_copydata(a->om, 0, n, p->map + p->map_total + p->map_len);
            p->map_len += n;
            return 0;
        }
        if (e->status != BLE_HS_EDONE) {
            fail(p, HID_IO_ERROR);
            return 0;
        }
        service_t *svc = &p->services[p->si];
        svc->raw_offset = p->map_total;
        svc->raw_len = p->map_len;
        hid_parse_result_t rc = hid_map_compile(&svc->compiled, p->map + p->map_total, p->map_len);
        if (rc != HID_PARSE_OK) {
            fail(p, HID_UNSUPPORTED_REPORT);
            return 0;
        }
        unsigned fields = 0, reports = 0;
        for (unsigned i = 0; i <= p->si; i++) {
            fields += p->services[i].compiled.nfields;
            reports += p->services[i].compiled.nreports;
        }
        if (fields > HID_FIELDS_MAX || reports > HID_REPORTS_MAX) {
            fail(p, HID_UNSUPPORTED_REPORT);
            return 0;
        }
        p->map_total += p->map_len;
        p->selected[p->si] =
            p->saved >= 0 && s.saved[p->saved].preference == HID_BOOT ? HID_BOOT : HID_REPORT;
        p->si++;
        discover_next(p);
    } else if (p->phase == 7) {
        if (e->status || OS_MBUF_PKTLEN(a->om) != 4)
            p->view.unsupported++;
        p->si++;
        discover_next(p);
    } else if (p->phase == 3) {
        uint8_t ref[2];
        if (e->status || OS_MBUF_PKTLEN(a->om) != 2) {
            fail(p, HID_UNSUPPORTED_REPORT);
            return 0;
        }
        os_mbuf_copydata(a->om, 0, 2, ref);
        p->chars[p->ci].id = ref[0];
        p->chars[p->ci].type = ref[1];
        p->ci++;
        discover_next(p);
    }
    return 0;
}
static int on_write(uint16_t c, const struct ble_gatt_error *e, struct ble_gatt_attr *a, void *arg)
{
    (void)a;
    peer_t *p = valid(c, arg);
    if (!p)
        return 0;
    if (e->status) {
        fail(p, HID_IO_ERROR);
        return 0;
    }
    if (p->phase == 4)
        p->si++;
    else {
        p->chars[p->ci].subscribed = true;
        p->ci++;
    }
    discover_next(p);
    return 0;
}
static int hid_gap(struct ble_gap_event *e, void *arg);
static void begin_connect(peer_t *p, const ble_addr_t *a)
{
    unsigned active = ble_link_is_connected();
    for (unsigned i = 0; i < HID_CONNECTIONS; i++)
        active += s.peers[i].conn != BLE_HS_CONN_HANDLE_NONE;
    if (ble_gap_conn_active() || active >= CONFIG_BT_NIMBLE_MAX_CONNECTIONS)
        return;
    ble_gap_disc_cancel();
    struct ble_gap_conn_params cp = {.scan_itvl = 48,
                                     .scan_window = 48,
                                     .itvl_min = 6,
                                     .itvl_max = 12,
                                     .latency = 0,
                                     .supervision_timeout = 400,
                                     .min_ce_len = 0,
                                     .max_ce_len = 0};
    state(p, HID_CONNECTING, HID_OK);
    p->deadline = now() + 15000000;
    int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, a, 10000, &cp, hid_gap, token(p));
    if (rc) {
        if (p->pairing || !p->view.reconnect)
            fail(p, rc == BLE_HS_ENOMEM ? HID_NO_CONNECTION_SLOT : HID_IO_ERROR);
        else {
            p->deadline = 0;
            state(p, HID_BACKOFF, rc == BLE_HS_ENOMEM ? HID_NO_CONNECTION_SLOT : HID_IO_ERROR);
            p->retry_at = now() + 5000000;
        }
    }
}
static bool bond_exists(const ble_addr_t *a)
{
    struct ble_store_key_sec key = {.peer_addr = *a};
    struct ble_store_value_sec val;
    return ble_store_read_peer_sec(&key, &val) == 0 && val.ltk_present;
}
static void secured(peer_t *p)
{
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(p->conn, &d))
        return;
    p->view.encrypted = d.sec_state.encrypted;
    p->view.authenticated = d.sec_state.authenticated;
    p->view.bonded = d.sec_state.bonded;
    if (!d.sec_state.encrypted)
        return;
    if (p->pairing && !p->just_works && !d.sec_state.authenticated) {
        fail(p, HID_AUTHENTICATION_FAILED);
        return;
    }
    if (p->view.error == HID_BOND_STORE_FULL) {
        fail(p, HID_BOND_STORE_FULL);
        return;
    }
    if (!d.sec_state.bonded || !bond_exists(&d.peer_id_addr))
        return;
    if (p->pairing) {
        int slot = saved_id(p->view.id);
        if (slot < 0)
            for (unsigned i = 0; i < HID_BONDS; i++)
                if (!s.saved[i].view.id) {
                    slot = i;
                    break;
                }
        if (slot < 0 || p->view.error == HID_BOND_STORE_FULL) {
            fail(p, HID_BOND_STORE_FULL);
            return;
        }
        saved_t *v = &s.saved[slot];
        v->blocked = 0;
        v->identity = d.peer_id_addr;
        v->view = p->view;
        v->view.id = 0x80000001u + slot;
        v->view.identity_id = v->view.id;
        v->view.saved = v->view.bonded = v->view.reconnect = true;
        set_address(&v->view, &d.peer_id_addr);
        strcpy(v->view.layout, "US");
        p->saved = slot;
        p->view = v->view;
        v->view.state = HID_DISCONNECTED;
        v->view.encrypted = false;
        if (persist()) {
            ble_gap_unpair(&d.peer_id_addr);
            memset(v, 0, sizeof(*v));
            fail(p, HID_BOND_STORE_FULL);
            return;
        }
        event(p, p->op, 0, HID_EV_PAIRED, HID_OK, 0);
        event(p, p->op, 0, HID_EV_BONDS, HID_OK, 0);
    }
    p->view.bonded = true;
    start_discovery(p);
}
static int hid_gap(struct ble_gap_event *e, void *arg)
{
    uintptr_t t = (uintptr_t)arg;
    unsigned slot = t & 3;
    if (slot >= HID_CONNECTIONS)
        return 0;
    peer_t *p = &s.peers[slot];
    if (p->generation != (t >> 2))
        return 0;
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (e->connect.status) {
            if (p->closing) {
                finish_security(p);
                p->deadline = 0;
                return 0;
            }
            if (p->pairing || !p->view.reconnect)
                fail(p, HID_IO_ERROR);
            else {
                p->deadline = 0;
                state(p, HID_BACKOFF, HID_OK);
                p->retry_at = now() + (++p->attempts < 4 ? 2000000 : 30000000);
            }
            return 0;
        }
        p->conn = e->connect.conn_handle;
        p->attempts = 0;
        if (p->closing) {
            ble_gap_terminate(p->conn, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        state(p, HID_SECURING, HID_OK);
        p->deadline = now() + 60000000;
        if (p->pairing) {
            ble_hs_cfg.sm_io_cap =
                p->just_works ? BLE_HS_IO_NO_INPUT_OUTPUT : BLE_HS_IO_DISPLAY_ONLY;
            ble_hs_cfg.sm_bonding = 1;
            ble_hs_cfg.sm_mitm = !p->just_works;
            ble_hs_cfg.sm_sc = 1;
        }
        if (ble_gap_security_initiate(p->conn))
            fail(p, HID_AUTHENTICATION_FAILED);
        return 0;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (!p->pairing || e->passkey.params.action != BLE_SM_IOACT_DISP) {
            fail(p, HID_AUTHENTICATION_FAILED);
            return 0;
        }
        {
            uint32_t random;
            do {
                random = esp_random();
            } while (random >= 4294000000u);
            struct ble_sm_io io = {.action = BLE_SM_IOACT_DISP, .passkey = random % 1000000u};
            if (ble_sm_inject_io(p->conn, &io))
                fail(p, HID_AUTHENTICATION_FAILED);
            else
                event(p, p->op, 0, HID_EV_PASSKEY_DISPLAY, HID_OK, io.passkey);
            return 0;
        }
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (p->closing)
            return 0;
        if (e->enc_change.status)
            fail(p, p->view.error == HID_BOND_STORE_FULL ? HID_BOND_STORE_FULL
                                                         : HID_AUTHENTICATION_FAILED);
        else
            secured(p);
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* Replacing a rejected key always requires an explicit pair operation. */
        fail(p, HID_AUTHENTICATION_FAILED);
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    case BLE_GAP_EVENT_DISCONNECT:
        if (e->disconnect.reason == BLE_HS_HCI_ERR(BLE_ERR_AUTH_FAIL) ||
            e->disconnect.reason == BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING))
            fail(p, HID_AUTHENTICATION_FAILED);
        if (p->pairing && !p->security_done)
            ble_gap_unpair(&e->disconnect.conn.peer_id_addr);
        release(p);
        finish_security(p);
        p->conn = BLE_HS_CONN_HANDLE_NONE;
        p->deadline = 0;
        p->security_done = false;
        p->view.encrypted = false;
        if (p->saved >= 0 && s.saved[p->saved].blocked)
            persist();
        if (!p->closing && p->view.reconnect) {
            state(p, HID_BACKOFF, HID_OK);
            p->retry_at = now() + 2000000;
        } else if (!p->closing) {
            state(p, HID_DISCONNECTED, HID_OK);
            p->retry_at = 0;
        }
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (p->closing || p->view.state != HID_READY || !p->view.encrypted)
            return 0;
        characteristic_t *ch = NULL;
        unsigned ri = 0;
        for (unsigned i = 0; i < p->nc; i++)
            if (p->chars[i].value == e->notify_rx.attr_handle && p->chars[i].subscribed) {
                ch = &p->chars[i];
                ri = i;
                break;
            }
        if (!ch)
            return 0;
        /* At most 16 selected input characteristics, independent of other GATT chars. */
        unsigned report_slot = 0;
        for (unsigned i = 0; i < ri; i++)
            report_slot += p->chars[i].subscribed;
        unsigned n = OS_MBUF_PKTLEN(e->notify_rx.om);
        uint8_t bytes[HID_PAYLOAD_MAX];
        hid_sample_t sample;
        uint64_t start = now();
        s.stats.reports++;
        if (n > sizeof(bytes) || report_slot >= HID_REPORTS_MAX) {
            s.stats.malformed++;
            return 0;
        }
        os_mbuf_copydata(e->notify_rx.om, 0, n, bytes);
        bool ok = ch->uuid == 0x2a4d ? hid_map_decode(&p->services[ch->service].compiled, ch->id,
                                                      bytes, n, &sample)
                                     : hid_boot_decode(ch->uuid == 0x2a33, bytes, n, &sample);
        if (!ok) {
            s.stats.malformed++;
            return 0;
        }
        int cols = 0, rows = 0, cw = 0, cheight = 0;
#if LS_HAS_COMPACT_UI
        ls_tui_geometry(&cols, &rows, &cw, &cheight);
#endif
        hid_input_report(&s.input, slot, report_slot, &sample, ms(), cols, rows, cw, cheight,
                         input_sink, NULL);
        uint32_t elapsed = now() - start;
        if (elapsed > s.stats.decode_max_us)
            s.stats.decode_max_us = elapsed;
        return 0;
    }
    case BLE_GAP_EVENT_MTU:
        p->view.mtu = e->mtu.value;
        return 0;
    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc d;
        if (!ble_gap_conn_find(p->conn, &d)) {
            p->view.interval = d.conn_itvl;
            p->view.latency = d.conn_latency;
            p->view.timeout = d.supervision_timeout;
        }
        return 0;
    }
    default:
        return 0;
    }
}
static peer_t *allocate_peer(const hid_device_t *v, uint32_t op, bool pairing, bool mouse)
{
    if (pairing && s.exclusive)
        return NULL;
    /* Host and controller must both have capacity; one remains the safe default. */
    unsigned hid_active = 0;
    for (unsigned i = 0; i < HID_CONNECTIONS; i++)
        hid_active += s.peers[i].conn != BLE_HS_CONN_HANDLE_NONE;
    unsigned active = hid_active + ble_link_is_connected();
    if (pairing && hid_active >= CONFIG_BT_NIMBLE_MAX_CONNECTIONS)
        return NULL;
    if (!pairing && active >= CONFIG_BT_NIMBLE_MAX_CONNECTIONS)
        return NULL;
    peer_t *chosen = NULL;
    for (unsigned pass = 0; pass < 3 && !chosen; pass++)
        for (unsigned i = 0; i < HID_CONNECTIONS; i++) {
            peer_t *slot = &s.peers[i];
            if (slot->conn == BLE_HS_CONN_HANDLE_NONE && slot->view.state != HID_CONNECTING &&
                !slot->pairing &&
                (slot->view.id == v->id || !slot->view.id || (pass == 1 && slot->closing) ||
                 (pass == 2 && (slot->view.state == HID_BACKOFF ||
                                (slot->view.state == HID_DISCONNECTED && !slot->retry_at))))) {
                chosen = slot;
                break;
            }
        }
    if (chosen) {
        peer_t *p = chosen;
        memset(p, 0, sizeof(*p));
        p->conn = BLE_HS_CONN_HANDLE_NONE;
        p->view = *v;
        p->generation = ++s.next_generation;
        p->view.generation = p->generation;
        p->saved = saved_id(v->id);
        p->op = op;
        p->pairing = pairing;
        p->just_works = mouse;
        p->view.state = HID_DISCONNECTED;
        if (pairing)
            s.exclusive = true;
        return p;
    }
    return NULL;
}
static void inspect(peer_t *p)
{
    if (!p)
        return;
    printf("hid %lu map bytes=%u fingerprint=%08lx unsupported=%u\n", (unsigned long)p->view.id,
           p->map_total, (unsigned long)p->view.fingerprint, p->view.unsupported);
    for (unsigned i = 0; i < p->ns; i++) {
        service_t *v = &p->services[i];
        printf(" service %u: %u..%u map=%u mode=%u selected=%s\n", i, v->start, v->end, v->map,
               v->mode, p->selected[i] == HID_BOOT ? "boot" : "report");
        printf("  map:");
        for (unsigned j = 0; j < v->raw_len; j++)
            printf(" %02x", p->map[v->raw_offset + j]);
        puts("");
        for (unsigned j = 0; j < v->compiled.nfields; j++) {
            hid_field_t *f = &v->compiled.fields[j];
            printf(
                "  id=%u type=%u page=%04x usage=%04x..%04x bit=%u size=%u count=%u flags=%02x\n",
                f->id, f->type, f->page, f->usage, f->usage_max, f->bit, f->size, f->count,
                f->flags);
        }
    }
    for (unsigned i = 0; i < p->nc; i++) {
        characteristic_t *c = &p->chars[i];
        printf(" chr %04x value=%u end=%u cccd=%u ref=%u id=%u type=%u subscribed=%u\n", c->uuid,
               c->value, c->end, c->cccd, c->ref, c->id, c->type, c->subscribed);
    }
}
static void execute(command_t c)
{
    int si = saved_id(c.id);
    peer_t *p = peer_id(c.id);
    hid_device_t *v = si >= 0 ? &s.saved[si].view : candidate(c.id);
    switch (c.kind) {
    case CMD_SCAN:
        s.scan_op = c.op;
        s.scan_generation++;
        memset(s.candidates, 0, sizeof(s.candidates));
        s.scan_until = now() + (uint64_t)c.arg * 1000000;
        break;
    case CMD_STOP:
        s.scan_op = 0;
        s.scan_until = 0;
        break;
    case CMD_PAIR: {
        if (p && (p->conn != BLE_HS_CONN_HANDLE_NONE || p->pairing)) {
            event(p, c.op, 0, HID_EV_ERROR, HID_BUSY, 0);
            return;
        }
        unsigned count = 0;
        for (unsigned i = 0; i < HID_BONDS; i++)
            count += s.saved[i].view.id != 0;
        if (count == HID_BONDS && si < 0) {
            event(NULL, c.op, c.id, HID_EV_ERROR, HID_BOND_STORE_FULL, 0);
            return;
        }
        if (!v || (!v->connectable && si < 0)) {
            event(NULL, c.op, c.id, HID_EV_ERROR, HID_NOT_FOUND, 0);
            return;
        }
        if (si >= 0) {
            ble_gap_unpair(&s.saved[si].identity);
        }
        p = allocate_peer(v, c.op, true, c.arg);
        if (!p) {
            event(NULL, c.op, c.id, HID_EV_ERROR, HID_NO_CONNECTION_SLOT, 0);
            return;
        }
        p->deadline = now() + 65000000;
        p->retry_at = 0;
        state(p, HID_SECURING, HID_OK);
        return;
    }
    case CMD_CONNECT:
        if (si < 0) {
            event(NULL, c.op, c.id, HID_EV_ERROR, HID_NOT_FOUND, 0);
            return;
        }
        if (s.saved[si].blocked == HID_AUTHENTICATION_FAILED) {
            event(NULL, c.op, c.id, HID_EV_ERROR, HID_AUTHENTICATION_FAILED, 0);
            return;
        }
        if (p && p->conn != BLE_HS_CONN_HANDLE_NONE) {
            event(p, c.op, 0, HID_EV_DONE, HID_OK, 0);
            return;
        }
        p = allocate_peer(v, c.op, false, false);
        if (!p) {
            event(NULL, c.op, c.id, HID_EV_ERROR, HID_NO_CONNECTION_SLOT, 0);
            return;
        }
        p->retry_at = now();
        break;
    case CMD_CANCEL:
        if (s.scan_op == c.id) {
            s.scan_until = 0;
            s.scan_op = 0;
            break;
        }
        for (unsigned i = 0; i < HID_CONNECTIONS; i++)
            if (s.peers[i].op == c.id && s.peers[i].view.state != HID_READY && !s.peers[i].closing)
                fail(&s.peers[i], HID_CANCELLED);
        break;
    case CMD_DISCONNECT:
        if (!p) {
            event(NULL, c.op, c.id, HID_EV_ERROR, HID_NOT_FOUND, 0);
            return;
        }
        fail(p, HID_CANCELLED);
        state(p, HID_DISCONNECTED, HID_OK);
        break;
    case CMD_FORGET:
        if (si < 0) {
            event(NULL, c.op, c.id, HID_EV_ERROR, HID_NOT_FOUND, 0);
            return;
        }
        if (p)
            fail(p, HID_CANCELLED);
        if (ble_gap_unpair(&s.saved[si].identity)) {
            event(NULL, c.op, c.id, HID_EV_ERROR, HID_IO_ERROR, 0);
            return;
        }
        memset(&s.saved[si], 0, sizeof(saved_t));
        if (p)
            memset(&p->view, 0, sizeof(p->view));
        persist();
        event(NULL, c.op, c.id, HID_EV_BONDS, HID_OK, 0);
        break;
    case CMD_FORGET_ALL:
        s.forget_all_op = c.op;
        for (unsigned i = 0; i < HID_CONNECTIONS; i++)
            if (s.peers[i].view.id)
                fail(&s.peers[i], HID_CANCELLED);
        ble_link_rescan(); /* cancel a head attempt / close the head before global deletion */
        return;            /* clear only after SMP and all disconnect callbacks finish */
    case CMD_RECONNECT:
        if (si < 0) {
            event(NULL, c.op, c.id, HID_EV_ERROR, HID_NOT_FOUND, 0);
            return;
        }
        s.saved[si].view.reconnect = c.arg;
        if (p) {
            p->view.reconnect = c.arg;
            if (!c.arg)
                p->retry_at = 0;
            if (c.arg && p->conn == BLE_HS_CONN_HANDLE_NONE && !s.saved[si].blocked) {
                p->closing = false;
                state(p, HID_BACKOFF, HID_OK);
                p->retry_at = now() + 100000;
            }
        }
        persist();
        break;
    case CMD_PROTOCOL:
        if (si < 0) {
            event(NULL, c.op, c.id, HID_EV_ERROR, HID_NOT_FOUND, 0);
            return;
        }
        if (p && p->conn != BLE_HS_CONN_HANDLE_NONE && p->view.state != HID_READY) {
            event(p, c.op, 0, HID_EV_ERROR, HID_BUSY, 0);
            return;
        }
        if (s.saved[si].blocked == HID_UNSUPPORTED_REPORT) {
            s.saved[si].blocked = 0;
            if (p && p->conn == BLE_HS_CONN_HANDLE_NONE) {
                p->closing = false;
                state(p, HID_BACKOFF, HID_OK);
                p->retry_at = now() + 100000;
            }
        }
        s.saved[si].preference = c.arg;
        persist();
        if (p && p->conn != BLE_HS_CONN_HANDLE_NONE) {
            release(p);
            start_discovery(p);
        }
        break;
    case CMD_INSPECT:
        inspect(p);
        break;
    case CMD_RELEASE_INPUT:
        for (unsigned i = 0; i < HID_CONNECTIONS; i++)
            release(&s.peers[i]);
        break;
    }
    event(NULL, c.op, c.id, HID_EV_DONE, HID_OK, 0);
}
static void timer_cb(struct ble_npl_event *ev)
{
    (void)ev;
    if (!s.synced)
        return;
    for (unsigned n = 0; n < COMMANDS; n++) {
        portENTER_CRITICAL(&mux);
        bool have = s.cr != s.cw;
        command_t c = s.commands[s.cr];
        if (have)
            s.cr = (s.cr + 1) % COMMANDS;
        portEXIT_CRITICAL(&mux);
        if (!have)
            break;
        execute(c);
    }
    uint64_t t = now();
    if (s.forget_all_op && !ble_link_is_connected() && !ble_gap_conn_active()) {
        bool connected = false;
        for (unsigned i = 0; i < HID_CONNECTIONS; i++)
            connected |= s.peers[i].conn != BLE_HS_CONN_HANDLE_NONE;
        if (!connected) {
            uint32_t op = s.forget_all_op;
            s.forget_all_op = 0;
            if (ble_store_clear())
                event(NULL, op, 0, HID_EV_ERROR, HID_IO_ERROR, 0);
            else {
                memset(s.saved, 0, sizeof(s.saved));
                for (unsigned i = 0; i < HID_CONNECTIONS; i++) {
                    s.peers[i].view.id = 0;
                    s.peers[i].saved = -1;
                }
                unsigned error = persist() ? HID_IO_ERROR : HID_OK;
                event(NULL, op, 0, HID_EV_BONDS, error, 0);
                event(NULL, op, 0, HID_EV_DONE, error, 0);
            }
        }
    }
    for (unsigned i = 0; i < HID_CONNECTIONS; i++) {
        peer_t *p = &s.peers[i];
        if (!p->view.id)
            continue;
        if (p->deadline && t > p->deadline) {
            fail(p, HID_TIMEOUT);
            p->deadline = 0;
        }
        if (p->closing)
            continue;
        if (p->view.state == HID_SECURING && p->conn != BLE_HS_CONN_HANDLE_NONE &&
            !p->security_done)
            secured(p);
        if (p->pairing && p->conn == BLE_HS_CONN_HANDLE_NONE && p->view.state == HID_SECURING &&
            !ble_link_is_connected() && !ble_gap_conn_active()) {
            ble_addr_t a = address(&p->view);
            begin_connect(p, &a);
        }
        if (p->conn == BLE_HS_CONN_HANDLE_NONE && !p->pairing && p->retry_at && t >= p->retry_at &&
            p->view.state != HID_CONNECTING) {
            p->retry_at = 0;
            if (p->saved >= 0) {
                ble_addr_t a = s.saved[p->saved].identity;
                s.stats.reconnects++;
                begin_connect(p, &a);
                if (p->view.state != HID_CONNECTING)
                    p->retry_at = t + 5000000;
            }
        }
    }
    /* Bonded devices stay visible when asleep. Retry scan handles directed and
     * undirected advertising without requiring HID UUIDs or names. */
    for (unsigned n = 0; n < HID_BONDS; n++) {
        unsigned i = (s.reconnect_cursor + n) % HID_BONDS;
        if (s.saved[i].view.id && s.saved[i].view.reconnect && !s.saved[i].blocked &&
            !s.exclusive && !s.forget_all_op) {
            peer_t *p = peer_id(s.saved[i].view.id);
            if (!p) {
                p = allocate_peer(&s.saved[i].view, 0, false, false);
                if (p) {
                    p->retry_at = t + 100000;
                    s.reconnect_cursor = (i + 1) % HID_BONDS;
                    break;
                }
            } else if (!p->closing && p->conn == BLE_HS_CONN_HANDLE_NONE &&
                       p->view.state == HID_BACKOFF && !p->retry_at)
                p->retry_at = t + 5000000;
        }
    }
    hid_input_repeat(&s.input, ms(), input_sink, NULL);
    publish();
    ble_npl_callout_reset(&tick, ble_npl_time_ms_to_ticks32(10));
}
void ble_hid_init(void)
{
    if (s.initialized)
        return;
    s.initialized = true;
    for (unsigned i = 0; i < HID_CONNECTIONS; i++) {
        s.peers[i].conn = BLE_HS_CONN_HANDLE_NONE;
        s.peers[i].saved = -1;
    }
    nvs_handle_t n;
    if (!nvs_open("ls_hid", NVS_READONLY, &n)) {
        size_t len = sizeof(s.saved);
        if (nvs_get_blob(n, "devices_v1", s.saved, &len) != ESP_OK || len != sizeof(s.saved))
            memset(s.saved, 0, sizeof(s.saved));
        nvs_close(n);
    }
    for (unsigned i = 0; i < HID_BONDS; i++) {
        s.saved[i].view.state = s.saved[i].blocked == HID_AUTHENTICATION_FAILED ? HID_NEEDS_PAIRING
                                : s.saved[i].blocked                            ? HID_UNSUPPORTED
                                                                                : HID_DISCONNECTED;
        s.saved[i].view.encrypted = false;
    }
    s.store_write = ble_hs_cfg.store_write_cb;
    ble_hs_cfg.store_write_cb = store_write;
    ble_hs_cfg.store_status_cb = store_status;
    ble_npl_callout_init(&tick, nimble_port_get_dflt_eventq(), timer_cb, NULL);
    publish();
}
void ble_hid_sync(void)
{
    s.synced = true;
    ble_npl_callout_reset(&tick, ble_npl_time_ms_to_ticks32(10));
}
void ble_hid_reset(void)
{
    s.synced = false;
    ble_npl_callout_stop(&tick);
    head_defaults();
    for (unsigned i = 0; i < HID_CONNECTIONS; i++) {
        peer_t *p = &s.peers[i];
        release(p);
        p->conn = BLE_HS_CONN_HANDLE_NONE;
        p->generation = ++s.next_generation;
        p->pairing = false;
        p->closing = false;
        p->deadline = 0;
        state(p, HID_BACKOFF, HID_RADIO_UNAVAILABLE);
        p->retry_at = now() + 2000000;
    }
}
bool ble_hid_busy(void)
{
    return ble_gap_conn_active();
}
bool ble_hid_head_blocked(void)
{
    if (s.exclusive || s.forget_all_op)
        return true;
    unsigned active = 0;
    for (unsigned i = 0; i < HID_CONNECTIONS; i++)
        active += s.peers[i].conn != BLE_HS_CONN_HANDLE_NONE;
    return active >= CONFIG_BT_NIMBLE_MAX_CONNECTIONS;
}
bool ble_hid_scan_wanted(void)
{
    if (s.scan_until > now())
        return true;
    for (unsigned i = 0; i < HID_BONDS; i++)
        if (s.saved[i].view.id && s.saved[i].view.reconnect && !s.saved[i].blocked) {
            peer_t *p = peer_id(s.saved[i].view.id);
            if (!p || (!p->closing && p->conn == BLE_HS_CONN_HANDLE_NONE))
                return true;
        }
    return false;
}
bool ble_hid_discovery_active(void)
{
    return s.scan_until > now();
}
void ble_hid_advert(const void *disc)
{
    const struct ble_gap_disc_desc *a = disc;
    hid_device_t *v = NULL;
    for (unsigned i = 0; i < HID_CANDIDATES; i++) {
        ble_addr_t b = address(&s.candidates[i]);
        if (s.candidates[i].id && addr_eq(&b, &a->addr)) {
            v = &s.candidates[i];
            break;
        }
    }
    bool hint = false;
    uint16_t appearance = 0;
    char name[32] = {0};
    for (unsigned i = 0; i < a->length_data;) {
        unsigned n = a->data[i++];
        if (!n)
            break;
        if (n > a->length_data - i)
            break;
        unsigned type = a->data[i];
        const uint8_t *data = &a->data[i + 1];
        unsigned len = n - 1;
        if (type == 8 || type == 9) {
            unsigned z = len < 31 ? len : 31;
            memcpy(name, data, z);
            name[z] = 0;
        }
        if ((type == 2 || type == 3) && len % 2 == 0)
            for (unsigned j = 0; j < len; j += 2)
                if (data[j] == 0x12 && data[j + 1] == 0x18)
                    hint = true;
        if (type == 0x19 && len == 2) {
            appearance = data[0] | data[1] << 8;
            if ((appearance & 0xffc0) == 0x3c0)
                hint = true;
        }
        i += n;
    }
    /* Retain unnamed packets during explicit discovery so scan responses can
     * add the HID hint or name to the same address/type candidate. */
    if (!v && s.scan_until > now())
        for (unsigned i = 0; i < HID_CANDIDATES; i++)
            if (!s.candidates[i].id) {
                v = &s.candidates[i];
                memset(v, 0, sizeof(*v));
                v->id = ((s.scan_generation & 0x7ffff) << 8) | (a->addr.type << 5) | (i + 1);
                set_address(v, &a->addr);
                break;
            }
    if (!v && (hint || name[0]) && s.scan_until > now()) {
        /* A busy room must not let unnamed, non-HID adverts monopolize the
         * discovery cache. Reused slots receive a fresh generation ID. */
        unsigned oldest = HID_CANDIDATES;
        for (unsigned i = 0; i < HID_CANDIDATES; i++)
            if (!s.candidates[i].advertised_hid &&
                (oldest == HID_CANDIDATES ||
                 s.candidates[i].last_seen_us < s.candidates[oldest].last_seen_us))
                oldest = i;
        if (oldest < HID_CANDIDATES) {
            v = &s.candidates[oldest];
            s.scan_generation++;
            memset(v, 0, sizeof(*v));
            v->id = ((s.scan_generation & 0x7ffff) << 8) | (a->addr.type << 5) | (oldest + 1);
            set_address(v, &a->addr);
        }
    }
    if (v) {
        v->last_seen_us = now();
        v->rssi = a->rssi;
        v->advertised_hid |= hint;
        if (appearance)
            v->appearance = appearance;
        if (name[0])
            strlcpy(v->name, name, sizeof(v->name));
        if (a->event_type != BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) {
            v->connectable = a->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                             a->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND;
            v->directed = a->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND;
        }
        if (hint || name[0])
            event(NULL, 0, v->id, HID_EV_DISCOVERY, HID_OK, 0);
    }
    if (s.exclusive || s.forget_all_op)
        return;
    for (unsigned i = 0; i < HID_BONDS; i++)
        if (s.saved[i].view.id && s.saved[i].view.reconnect && !s.saved[i].blocked) {
            ble_addr_t id = a->addr;
            if (id.type == BLE_ADDR_PUBLIC_ID)
                id.type = BLE_ADDR_PUBLIC;
            else if (id.type == BLE_ADDR_RANDOM_ID)
                id.type = BLE_ADDR_RANDOM;
            if (!addr_eq(&id, &s.saved[i].identity))
                continue;
            if (v)
                v->identity_id = s.saved[i].view.id;
            peer_t *p = peer_id(s.saved[i].view.id);
            if (!p)
                p = allocate_peer(&s.saved[i].view, 0, false, false);
            if (p && !p->closing && p->conn == BLE_HS_CONN_HANDLE_NONE &&
                p->view.state != HID_CONNECTING && p->view.state != HID_NEEDS_PAIRING &&
                !ble_gap_conn_active())
                begin_connect(p, &a->addr);
        }
}
static uint32_t submit(unsigned kind, uint32_t id, unsigned arg)
{
    if (!s.initialized)
        return 0;
    portENTER_CRITICAL(&mux);
    unsigned next = (s.cw + 1) % COMMANDS;
    uint32_t op = 0;
    if (next != s.cr) {
        op = ++s.next_op;
        if (!op)
            op = ++s.next_op;
        s.commands[s.cw] = (command_t){op, id, arg, kind};
        s.cw = next;
    }
    portEXIT_CRITICAL(&mux);
    return op;
}
uint32_t hid_scan_start(unsigned seconds)
{
    if (ble_link_hid_observer(true) != ESP_OK)
        return 0;
    return submit(CMD_SCAN, 0, seconds ? seconds > 120 ? 120 : seconds : 15);
}
uint32_t hid_scan_stop(void)
{
    return submit(CMD_STOP, 0, 0);
}
uint32_t hid_pair(uint32_t id, bool mouse)
{
    return submit(CMD_PAIR, id, mouse);
}
uint32_t hid_cancel(uint32_t op)
{
    return submit(CMD_CANCEL, op, 0);
}
uint32_t hid_connect(uint32_t id)
{
    return submit(CMD_CONNECT, id, 0);
}
uint32_t hid_disconnect(uint32_t id)
{
    return submit(CMD_DISCONNECT, id, 0);
}
uint32_t hid_forget(uint32_t id)
{
    return submit(CMD_FORGET, id, 0);
}
uint32_t hid_forget_all(void)
{
    return submit(CMD_FORGET_ALL, 0, 0);
}
uint32_t hid_set_reconnect(uint32_t id, bool on)
{
    return submit(CMD_RECONNECT, id, on);
}
uint32_t hid_set_protocol(uint32_t id, hid_protocol_t proto)
{
    return proto <= HID_REPORT ? submit(CMD_PROTOCOL, id, proto) : 0;
}
static size_t list(hid_device_t *out, size_t cap, uint32_t *version, bool bonds)
{
    portENTER_CRITICAL(&mux);
    size_t n = 0;
    for (size_t i = 0; i < s.npublished; i++)
        if (!bonds || s.published[i].saved) {
            if (out && n < cap)
                out[n] = s.published[i];
            n++;
        }
    if (version)
        *version = s.version;
    portEXIT_CRITICAL(&mux);
    return n;
}
size_t hid_list_devices(hid_device_t *out, size_t cap, uint32_t *v)
{
    return list(out, cap, v, false);
}
size_t hid_list_bonds(hid_device_t *out, size_t cap, uint32_t *v)
{
    return list(out, cap, v, true);
}
bool hid_get_capabilities(uint32_t id, hid_device_t *out)
{
    bool found = false;
    portENTER_CRITICAL(&mux);
    for (size_t i = 0; i < s.npublished; i++)
        if (s.published[i].id == id) {
            *out = s.published[i];
            found = true;
            break;
        }
    portEXIT_CRITICAL(&mux);
    return found;
}
void hid_get_stats(hid_stats_t *out)
{
    portENTER_CRITICAL(&mux);
    *out = s.published_stats;
    portEXIT_CRITICAL(&mux);
}
void hid_get_input_state(hid_input_state_t *out)
{
    portENTER_CRITICAL(&mux);
    *out = s.published_input;
    portEXIT_CRITICAL(&mux);
}
uint32_t hid_release_input(void)
{
    return submit(CMD_RELEASE_INPUT, 0, 0);
}
bool hid_events_next(uint32_t *cursor, hid_event_t *out)
{
    portENTER_CRITICAL(&mux);
    uint32_t last = s.event_seq;
    bool have = *cursor < last;
    if (have) {
        uint32_t next = *cursor + 1;
        if (last - next >= EVENTS)
            next = last - EVENTS + 1;
        *out = s.events[(next - 1) % EVENTS];
        *cursor = next;
    }
    portEXIT_CRITICAL(&mux);
    return have;
}
static const char *const states[] = {"DISCONNECTED", "CONNECTING",    "SECURING",
                                     "DISCOVERING",  "SUBSCRIBING",   "READY",
                                     "BACKOFF",      "NEEDS_PAIRING", "UNSUPPORTED"};
static const char *const errors[] = {"OK",
                                     "AUTHENTICATION_FAILED",
                                     "BOND_STORE_FULL",
                                     "NO_CONNECTION_SLOT",
                                     "RADIO_UNAVAILABLE",
                                     "UNSUPPORTED_REPORT",
                                     "CANCELLED",
                                     "TIMEOUT",
                                     "NOT_FOUND",
                                     "IO_ERROR",
                                     "BUSY"};
void ble_hid_console_pump(void)
{
    static uint32_t cursor;
    hid_event_t e;
    for (unsigned i = 0; i < 16 && hid_events_next(&cursor, &e); i++) {
        if (e.kind == HID_EV_DISCOVERY)
            continue;
        if (e.kind == HID_EV_PASSKEY_DISPLAY)
            printf("ble hid op=%lu device=%lu PASSKEY %06lu: type on the keyboard, then Enter\n",
                   (unsigned long)e.operation, (unsigned long)e.device, (unsigned long)e.passkey);
        else
            printf("ble hid seq=%lu op=%lu device=%lu gen=%lu %s %s%s\n", (unsigned long)e.sequence,
                   (unsigned long)e.operation, (unsigned long)e.device, (unsigned long)e.generation,
                   states[e.state], errors[e.error],
                   e.kind == HID_EV_PASSKEY_EXPIRED ? " PASSKEY_EXPIRED"
                   : e.kind == HID_EV_PAIRED        ? " PAIRED"
                   : e.kind == HID_EV_DONE          ? " DONE"
                                                    : "");
    }
    for (unsigned i = 0; i < 16; i++) {
        portENTER_CRITICAL(&mux);
        bool have = s.wr != s.ww;
        ls_input_ev_t v = s.watched[s.wr];
        if (have)
            s.wr = (s.wr + 1) % WATCH;
        portEXIT_CRITICAL(&mux);
        if (!have)
            break;
        printf("input src=%u seq=%lu kind=%u key=%u ch=%02x mods=%u clicks=%u cell=%d,%d px=%d,%d "
               "buttons=%u button=%u wheel=%d\n",
               v.source, (unsigned long)v.seq, v.kind, v.key, (unsigned char)v.ch, v.mods, v.clicks,
               v.col, v.row, v.px, v.py, v.buttons, v.button, v.wheel);
    }
}
int ble_hid_input_console(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "watch") || argc > 3 ||
        (argc == 3 && strcmp(argv[2], "on") && strcmp(argv[2], "off"))) {
        puts("input watch [on|off]");
        return 1;
    }
    portENTER_CRITICAL(&mux);
    s.watch = argc < 3 || strcmp(argv[2], "off");
    if (!s.watch)
        s.wr = s.ww;
    bool enabled = s.watch;
    portEXIT_CRITICAL(&mux);
    printf("input watch %s\n", enabled ? "on" : "off");
    return 0;
}
int ble_hid_console(int argc, char **argv)
{
    if (argc < 1) {
        puts("ble hid "
             "scan|pair|cancel|devices|bonds|connect|disconnect|forget|reconnect|inspect|protocol|"
             "stats");
        return 1;
    }
    if (!strcmp(argv[0], "scan"))
        ble_link_hid_observer(true);
    uint32_t id = argc > 1 ? strtoul(argv[1], NULL, 0) : 0, op = 0;
    if (!strcmp(argv[0], "devices") || !strcmp(argv[0], "bonds")) {
        /* Large snapshot is in PSRAM, not on the console task stack. */
        for (size_t i = 0;; i++) {
            hid_device_t d;
            bool have;
            portENTER_CRITICAL(&mux);
            have = i < s.npublished;
            if (have)
                d = s.published[i];
            portEXIT_CRITICAL(&mux);
            if (!have)
                break;
            if (!strcmp(argv[0], "bonds") && !d.saved)
                continue;
            printf("%lu %s %02x:%02x:%02x:%02x:%02x:%02x/%u rssi=%d hid=%u appearance=%04x %s "
                   "bonded=%u enc=%u auth=%u reconnect=%u kbd=%u mouse=%u wheel=%u interval=%u "
                   "latency=%u timeout=%u mtu=%u\n",
                   (unsigned long)d.id, d.name, d.address[5], d.address[4], d.address[3],
                   d.address[2], d.address[1], d.address[0], d.address_type, d.rssi,
                   d.advertised_hid, d.appearance, states[d.state], d.bonded, d.encrypted,
                   d.authenticated, d.reconnect, d.keyboard, d.mouse, d.wheel, d.interval,
                   d.latency, d.timeout, d.mtu);
        }
        return 0;
    }
    if (!strcmp(argv[0], "stats")) {
        hid_stats_t st;
        hid_get_stats(&st);
        printf("hid reports=%lu malformed=%lu input_drops=%lu overwritten_events=%lu "
               "reconnects=%lu max_decode_us=%lu PSRAM=%u internal_control=%u connections=%u\n",
               (unsigned long)st.reports, (unsigned long)st.malformed,
               (unsigned long)st.queue_drops, (unsigned long)st.event_drops,
               (unsigned long)st.reconnects, (unsigned long)st.decode_max_us,
               (unsigned)st.psram_bytes, (unsigned)(sizeof(mux) + sizeof(tick)),
               CONFIG_BT_NIMBLE_MAX_CONNECTIONS);
        return 0;
    }
    if (!strcmp(argv[0], "scan")) {
        if (argc > 1 && !strcmp(argv[1], "stop"))
            op = hid_scan_stop();
        else
            op = hid_scan_start(id ? id : 15);
    } else if (!strcmp(argv[0], "pair") && id)
        op = hid_pair(id, argc > 2 && !strcmp(argv[2], "--mouse"));
    else if (!strcmp(argv[0], "cancel") && id)
        op = hid_cancel(id);
    else if (!strcmp(argv[0], "connect") && id)
        op = hid_connect(id);
    else if (!strcmp(argv[0], "disconnect") && id)
        op = hid_disconnect(id);
    else if (!strcmp(argv[0], "forget") && id)
        op = hid_forget(id);
    else if (!strcmp(argv[0], "reconnect") && id && argc == 3 &&
             (!strcmp(argv[2], "on") || !strcmp(argv[2], "off")))
        op = hid_set_reconnect(id, !strcmp(argv[2], "on"));
    else if (!strcmp(argv[0], "protocol") && id && argc == 3) {
        if (!strcmp(argv[2], "auto"))
            op = hid_set_protocol(id, HID_AUTO);
        else if (!strcmp(argv[2], "boot"))
            op = hid_set_protocol(id, HID_BOOT);
        else if (!strcmp(argv[2], "report"))
            op = hid_set_protocol(id, HID_REPORT);
    } else if (!strcmp(argv[0], "inspect") && id)
        op = submit(CMD_INSPECT, id, 0);
    else {
        puts("ble hid scan [15|stop] | pair <id> [--mouse] | cancel <op> | devices | bonds | "
             "connect|disconnect|forget|inspect <id> | reconnect <id> on|off | protocol <id> "
             "boot|report|auto | stats");
        return 1;
    }
    printf("ble hid operation=%lu%s\n", (unsigned long)op,
           op ? "" : " RADIO_UNAVAILABLE or command queue full");
    return op ? 0 : 1;
}
