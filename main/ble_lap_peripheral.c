/* API 2.6 peripheral backend. Only NimBLE's existing host timer calls here.
 * Definitions/values in PSRAM; host stack and transport/DMA keep board placement.
 * No new task, no PSRAM stack for flash-writing NimBLE work. */
#ifdef LS_LAP_PERIPH_HOST_TEST
#include "ble_peripheral_host_fake.h"
#else
#include "sdkconfig.h"
#endif
#if CONFIG_LS_LAP_BLE_ADVERTISER
#ifndef LS_LAP_PERIPH_HOST_TEST
#include "ble_link.h"
#include "esp_heap_caps.h"
#include "host/ble_att.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "os/os_mbuf.h"
#endif
#include "tui/ls_lap_ble_peripheral.h"
static int handle;
static uint16_t connection = BLE_HS_CONN_HANDLE_NONE;
static bool closing, command_pending, indication;
static int command_result, actual_power;
static uint8_t old_io, old_bond, old_mitm, old_sc;
static bool security_saved;
static EXT_RAM_BSS_ATTR ls_lap_peripheral_command_t command;
/* State events are coalesced only on host->screen lock contention. Latest
 * connection/security/MTU/subscription state always reaches the shared service.
 * ATT read/write events instead fail atomically on overflow. */
static EXT_RAM_BSS_ATTR struct {
    int kind, id, error, value;
    bool pending;
} pending_events[22];
static void emit(int h, int kind, int id, int error, int value) {
    int slot = kind == LAP_PERIPHERAL_CONNECTED || kind == LAP_PERIPHERAL_DISCONNECTED ? 0
               : kind == LAP_PERIPHERAL_SUBSCRIBE                                      ? 6
               : kind == LAP_PERIPHERAL_ADVERTISING                                    ? 3
                                                                                       : kind;
    if (kind == LAP_PERIPHERAL_SUBSCRIBE) {
        slot = -1;
        for (int i = 6; i < 22; i++)
            if (pending_events[i].pending && pending_events[i].id == id) {
                slot = i;
                break;
            }
        if (slot < 0)
            for (int i = 6; i < 22; i++)
                if (!pending_events[i].pending) {
                    slot = i;
                    break;
                }
        if (slot < 0) {
            ls_lap_peripheral_lost();
            return;
        }
    }
    if (pending_events[slot].pending)
        ls_lap_peripheral_lost();
    pending_events[slot].kind = kind;
    pending_events[slot].id = id;
    pending_events[slot].error = error;
    pending_events[slot].value = value;
    pending_events[slot].pending = true;
    (void)h; /* all slots belong to the one acknowledged host session */
}
void ls_lap_peripheral_host_reset(void) {
    connection = BLE_HS_CONN_HANDLE_NONE;
    indication = false;
    if (handle) {
        emit(handle, LAP_PERIPHERAL_DISCONNECTED, 0, LAP_IO_EIO, 0);
        command_result = LAP_IO_EIO;
    }
}
#if CONFIG_LS_LAP_GATT_SERVER
/* Pinned IDF 5.5.4 internal hooks, firmware-only. Public reset removes
 * attributes but retains count_cfg totals. Stop clears those totals and frees
 * pools; enumerating first preserves the firmware's GAP/GATT definitions.
 * Avoid the boot-only svc_*_init wrappers here: they assert on allocation
 * failure instead of reporting an app operation error. All calls run on the
 * exclusive host queue with no connected peer/GAP operation. */
extern void ble_gatts_stop(void);
extern void ble_gatts_lcl_svc_foreach(ble_gatt_svc_foreach_fn cb, void *arg);
static EXT_RAM_BSS_ATTR struct {
    struct ble_gatt_svc_def services[5];
    unsigned count;
    bool saved;
} baseline;
static void save_service(const struct ble_gatt_svc_def *svc, uint16_t first, uint16_t last,
                         void *arg) {
    (void)first;
    (void)last;
    (void)arg;
    if (baseline.count < 4)
        baseline.services[baseline.count] = *svc;
    baseline.count++;
}
static int reset_database(void) {
    if (!baseline.saved) {
        memset(&baseline, 0, sizeof(baseline));
        ble_gatts_lcl_svc_foreach(save_service, NULL);
        if (baseline.count > 4)
            return BLE_HS_EBUSY;
        baseline.saved = true;
    }
    int rc = ble_gatts_reset();
    if (rc)
        return rc;
    ble_gatts_stop();
    rc = ble_gatts_count_cfg(baseline.services);
    if (!rc)
        rc = ble_gatts_add_svcs(baseline.services);
    return rc;
}
/* Each row has a terminating zero slot. At most 16 total app attributes. */
static EXT_RAM_BSS_ATTR struct {
    struct ble_gatt_svc_def services[17];
    struct ble_gatt_chr_def chars[32];
    struct ble_gatt_dsc_def descs[32];
    ble_uuid_any_t uuids[16];
    uint16_t handles[16], ids[16];
    int count;
    bool dirty;
} db;
static int attribute_index(uint16_t id) {
    for (int i = 0; i < db.count; i++)
        if (db.ids[i] == id)
            return i;
    return -1;
}
static int att(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr;
    if (conn != connection || closing)
        return BLE_ATT_ERR_UNLIKELY;
    uint16_t id = (uintptr_t)arg;
    bool write =
        ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR || ctxt->op == BLE_GATT_ACCESS_OP_WRITE_DSC;
    uint8_t value[128];
    size_t n = sizeof(value);
    if (write) {
        n = OS_MBUF_PKTLEN(ctxt->om);
        if (n > 128)
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (os_mbuf_copydata(ctxt->om, 0, n, value))
            return BLE_ATT_ERR_UNLIKELY;
    }
    int rc = ls_lap_peripheral_access(handle, id, write, value, &n);
    if (!rc && !write && os_mbuf_append(ctxt->om, value, n))
        rc = BLE_ATT_ERR_INSUFFICIENT_RES;
    return rc;
}
static void uuid(ble_uuid_any_t *out, const lap_uuid_t *in) {
    memset(out, 0, sizeof(*out));
    if (in->length == 16) {
        out->u128.u.type = BLE_UUID_TYPE_128;
        memcpy(out->u128.value, in->bytes, 16);
    } else {
        uint32_t v = 0;
        for (int j = 0; j < in->length; j++)
            v |= (uint32_t)in->bytes[j] << (8 * j);
        if (in->length == 2) {
            out->u16.u.type = BLE_UUID_TYPE_16;
            out->u16.value = v;
        } else {
            out->u32.u.type = BLE_UUID_TYPE_32;
            out->u32.value = v;
        }
    }
}
static int clear_db(void) {
    int rc = reset_database();
    if (rc)
        return rc;
    rc = ble_gatts_start();
    if (!rc) {
        memset(&db, 0, sizeof(db));
    }
    return rc;
}
static int register_db(void) {
    int rc = reset_database();
    if (rc)
        return rc;
    memset(&db, 0, sizeof(db));
    db.dirty = true; /* retain cleanup responsibility even if registration fails */
    unsigned s = 0, c = 0, d = 0;
    struct ble_gatt_chr_def *chr = NULL;
    for (int i = 0; i < command.count; i++) {
        lap_gatt_attribute_t *a = &command.attrs[i];
        uuid(&db.uuids[i], &a->uuid);
        db.ids[i] = a->id;
        if (a->kind == LAP_GATTS_SERVICE) {
            if (chr) {
                if (chr->descriptors)
                    d++;
                c++;
                chr = NULL;
            } /* previous characteristic terminator */
            struct ble_gatt_svc_def *svc = &db.services[s++];
            svc->type = BLE_GATT_SVC_TYPE_PRIMARY;
            svc->uuid = &db.uuids[i].u;
            svc->characteristics = &db.chars[c];
        } else if (a->kind == LAP_GATTS_CHARACTERISTIC) {
            if (chr && chr->descriptors)
                d++;
            chr = &db.chars[c++];
            chr->uuid = &db.uuids[i].u;
            chr->access_cb = att;
            chr->arg = (void *)(uintptr_t)a->id;
            chr->val_handle = &db.handles[i];
            unsigned f = a->flags;
            if (f & LAP_GATTS_READ)
                chr->flags |= BLE_GATT_CHR_F_READ;
            if (f & LAP_GATTS_WRITE)
                chr->flags |= BLE_GATT_CHR_F_WRITE;
            if (f & LAP_GATTS_WRITE_NR)
                chr->flags |= BLE_GATT_CHR_F_WRITE_NO_RSP;
            if (f & LAP_GATTS_NOTIFY)
                chr->flags |= BLE_GATT_CHR_F_NOTIFY;
            if (f & LAP_GATTS_INDICATE)
                chr->flags |= BLE_GATT_CHR_F_INDICATE;
            if (f & LAP_GATTS_ENCRYPT) {
                if (f & LAP_GATTS_READ)
                    chr->flags |= BLE_GATT_CHR_F_READ_ENC;
                if (f & (LAP_GATTS_WRITE | LAP_GATTS_WRITE_NR))
                    chr->flags |= BLE_GATT_CHR_F_WRITE_ENC;
            }
            if (f & LAP_GATTS_AUTHENTICATE) {
                if (f & LAP_GATTS_READ)
                    chr->flags |= BLE_GATT_CHR_F_READ_AUTHEN;
                if (f & (LAP_GATTS_WRITE | LAP_GATTS_WRITE_NR))
                    chr->flags |= BLE_GATT_CHR_F_WRITE_AUTHEN;
            }
        } else {
            struct ble_gatt_dsc_def *v = &db.descs[d++];
            if (!chr->descriptors)
                chr->descriptors = v;
            v->uuid = &db.uuids[i].u;
            v->access_cb = att;
            v->arg = (void *)(uintptr_t)a->id;
            if (a->flags & LAP_GATTS_READ)
                v->att_flags |= BLE_ATT_F_READ;
            if (a->flags & LAP_GATTS_WRITE)
                v->att_flags |= BLE_ATT_F_WRITE;
            if (a->flags & LAP_GATTS_ENCRYPT) {
                if (a->flags & LAP_GATTS_READ)
                    v->att_flags |= BLE_ATT_F_READ_ENC;
                if (a->flags & LAP_GATTS_WRITE)
                    v->att_flags |= BLE_ATT_F_WRITE_ENC;
            }
            if (a->flags & LAP_GATTS_AUTHENTICATE) {
                if (a->flags & LAP_GATTS_READ)
                    v->att_flags |= BLE_ATT_F_READ_AUTHEN;
                if (a->flags & LAP_GATTS_WRITE)
                    v->att_flags |= BLE_ATT_F_WRITE_AUTHEN;
            }
        }
    }
    db.count = command.count;
    rc = ble_gatts_count_cfg(db.services);
    if (!rc)
        rc = ble_gatts_add_svcs(db.services);
    if (!rc)
        rc = ble_gatts_start();
    return rc;
}
#endif
static void stop_advert(void) {
    ble_gap_ext_adv_stop(0);
    ble_gap_ext_adv_remove(0);
}
static int gap(struct ble_gap_event *ev, void *arg) {
    if ((int)(intptr_t)arg != handle)
        return 0;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (!ev->connect.status) {
            connection = ev->connect.conn_handle;
            if (closing) {
                ble_gap_terminate(connection, BLE_ERR_REM_USER_CONN_TERM);
                break;
            }
        }
        emit(handle, LAP_PERIPHERAL_CONNECTED, 0, ev->connect.status ? LAP_IO_EIO : 0, 0);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        connection = BLE_HS_CONN_HANDLE_NONE;
        if (indication)
            command_result = LAP_IO_ECLOSED;
        indication = false;
        emit(handle, LAP_PERIPHERAL_DISCONNECTED, 0, ev->disconnect.reason, 0);
        break;
    case BLE_GAP_EVENT_MTU:
        emit(handle, LAP_PERIPHERAL_MTU, 0, 0, ev->mtu.value);
        break;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc desc;
        int rc = ble_gap_conn_find(connection, &desc);
        emit(handle, LAP_PERIPHERAL_BOND, 0, ev->enc_change.status ? LAP_IO_EAUTH : 0,
             !rc ? (desc.sec_state.bonded | (desc.sec_state.encrypted << 1) |
                    (desc.sec_state.authenticated << 2))
                 : 0);
        break;
    }
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        /* Never fake MITM. NoIO Just Works peers should not request a passkey.
           A peer requiring MITM is rejected; keys/UI remain firmware-owned. */
        ble_gap_terminate(connection, BLE_ERR_AUTH_FAIL);
        break;
#if CONFIG_LS_LAP_GATT_SERVER
    case BLE_GAP_EVENT_SUBSCRIBE:
        for (int i = 0; i < db.count; i++)
            if (db.handles[i] == ev->subscribe.attr_handle)
                emit(handle, LAP_PERIPHERAL_SUBSCRIBE, db.ids[i], 0,
                     ev->subscribe.cur_notify | (ev->subscribe.cur_indicate << 1));
        break;
    case BLE_GAP_EVENT_NOTIFY_TX:
        if (ev->notify_tx.indication) {
            if (ev->notify_tx.status == 0)
                break; /* sent, await confirmation (EDONE) */
            indication = false;
            int rc = ev->notify_tx.status == BLE_HS_EDONE ? 0 : LAP_IO_EIO;
            command_result = rc; /* outstanding command is held until ack */
        }
        break;
#endif
    case BLE_GAP_EVENT_ADV_COMPLETE:
        emit(handle, LAP_PERIPHERAL_ADVERTISING, 0, ev->adv_complete.reason,
             ble_gap_ext_adv_active(0));
        break;
    default:
        break;
    }
    return 0;
}
static int advertise(void) {
    lap_ble_advert_config_t *a = &command.advert;
    if (connection != BLE_HS_CONN_HANDLE_NONE)
        return LAP_IO_EBUSY;
    stop_advert();
    struct ble_gap_ext_adv_params cfg = {.legacy_pdu = 1,
                                         .connectable = a->connectable,
                                         .scannable = a->connectable || a->scan_response_len,
                                         .own_addr_type = BLE_OWN_ADDR_PUBLIC,
                                         .itvl_min = a->interval_min_ms * 8u / 5,
                                         .itvl_max = a->interval_max_ms * 8u / 5,
                                         .primary_phy = BLE_HCI_LE_PHY_1M,
                                         .secondary_phy = BLE_HCI_LE_PHY_1M,
                                         .tx_power = a->tx_power_dbm};
    int8_t power = 0;
    int rc = ble_gap_ext_adv_configure(0, &cfg, &power, gap, (void *)(intptr_t)handle);
    if (rc)
        return LAP_IO_EIO;
    actual_power = power;
    if (power > a->tx_power_dbm) {
        stop_advert();
        return LAP_IO_EPOWER;
    }
    struct os_mbuf *m = ble_hs_mbuf_from_flat(a->data, a->data_len);
    if (!m)
        return LAP_IO_ENOSPC;
    rc = ble_gap_ext_adv_set_data(0, m); /* consumes m on success and failure */
    if (!rc && a->scan_response_len) {
        m = ble_hs_mbuf_from_flat(a->scan_response, a->scan_response_len);
        if (!m)
            rc = BLE_HS_ENOMEM;
        else
            rc = ble_gap_ext_adv_rsp_set_data(0, m);
    }
    if (!rc)
        rc = ble_gap_ext_adv_start(0, 0, 0);
    if (rc)
        stop_advert();
    return rc ? LAP_IO_EIO : 0;
}
void ls_lap_peripheral_host_pump(void) {
    if (!ble_hs_synced())
        return;
    for (int i = 0; i < 22; i++)
        if (pending_events[i].pending) {
            if (ls_lap_peripheral_event(handle, pending_events[i].kind, pending_events[i].id,
                                        pending_events[i].error, pending_events[i].value))
                pending_events[i].pending = false;
        }
    if (handle && !ls_lap_peripheral_live(handle) && !closing) {
        closing = true;
        stop_advert();
        if (connection != BLE_HS_CONN_HANDLE_NONE)
            ble_gap_terminate(connection, BLE_ERR_REM_USER_CONN_TERM);
    }
    if (closing) {
        if (connection != BLE_HS_CONN_HANDLE_NONE)
            return;
        stop_advert();
#if CONFIG_LS_LAP_GATT_SERVER
        if (db.dirty && clear_db())
            return; /* don't release a live database */
#endif
        if (ls_lap_peripheral_complete(handle, PERIPH_CLOSE, 0, 0)) {
            if (security_saved) {
                ble_hs_cfg.sm_io_cap = old_io;
                ble_hs_cfg.sm_bonding = old_bond;
                ble_hs_cfg.sm_mitm = old_mitm;
                ble_hs_cfg.sm_sc = old_sc;
                security_saved = false;
            }
            handle = 0;
            closing = false;
            command_pending = false;
            indication = false;
            memset(pending_events, 0, sizeof(pending_events));
        }
        return;
    }
    if (command_pending) {
        if (indication)
            return;
        if (!ls_lap_peripheral_complete(command.handle, command.kind, command_result, actual_power))
            return;
        command_pending = false;
    }
    if (!ls_lap_peripheral_command(&command))
        return;
    if (command.kind == PERIPH_CLOSE) {
        if (!handle)
            handle = command.handle;
        closing = true;
        if (connection != BLE_HS_CONN_HANDLE_NONE)
            ble_gap_terminate(connection, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    int rc = 0;
    if (command.kind == PERIPH_OPEN) {
        handle = command.handle;
        old_io = ble_hs_cfg.sm_io_cap;
        old_bond = ble_hs_cfg.sm_bonding;
        old_mitm = ble_hs_cfg.sm_mitm;
        old_sc = ble_hs_cfg.sm_sc;
        security_saved = true;
        ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
        ble_hs_cfg.sm_bonding = 1;
        ble_hs_cfg.sm_mitm = 0;
#if CONFIG_BT_NIMBLE_SM_SC
        ble_hs_cfg.sm_sc = 1;
#else
        ble_hs_cfg.sm_sc = 0;
#endif
        /* Admission protects internal DMA/host reserve; never increase pools or
           connections just to serve an app. Actual delta measured on board. */
        if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) <
                (command.count ? 12288 : 4096) ||
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) <
                (command.count ? 24576 : 8192))
            rc = LAP_IO_ENOSPC;
#if CONFIG_LS_LAP_GATT_SERVER
        if (!rc && command.count) {
            int err = register_db();
            if (err) {
                clear_db();
                rc = LAP_IO_ENOSPC;
            }
        }
#else
        if (command.count)
            rc = LAP_IO_ENOTSUP;
#endif
    } else if (command.handle != handle)
        rc = LAP_IO_ECLOSED;
    else if (command.kind == PERIPH_ADVERTISE)
        rc = advertise();
    else if (command.kind == PERIPH_STOP)
        stop_advert();
    else if (command.kind == PERIPH_SEND) {
#if CONFIG_LS_LAP_GATT_SERVER
        int i = attribute_index(command.id);
        if (i < 0 || connection == BLE_HS_CONN_HANDLE_NONE)
            rc = LAP_IO_ECLOSED;
        else {
            struct os_mbuf *m = ble_hs_mbuf_from_flat(command.data, command.len);
            if (!m)
                rc = LAP_IO_ENOSPC;
            else {
                int err = command.indicate ? ble_gatts_indicate_custom(connection, db.handles[i], m)
                                           : ble_gatts_notify_custom(connection, db.handles[i], m);
                rc = err ? LAP_IO_EIO : 0;
                indication = !err && command.indicate;
            }
        }
#else
        rc = LAP_IO_ENOTSUP;
#endif
    }
    command_result = rc;
    command_pending = true;
}
#else
void ls_lap_peripheral_host_reset(void) {}
#endif /* CONFIG_LS_LAP_BLE_ADVERTISER */
