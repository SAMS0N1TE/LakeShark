/* One SDK central on the existing NimBLE host queue. No tasks or large
 * internal arrays: long values, the command copy and the notify buffer live
 * in PSRAM. Do not move NimBLE transport/DMA buffers or NVS-capable stacks
 * to PSRAM: bond writes run on the host task's internal static stack.
 * Pairing is per request. The global SM settings stay at no bonding, no
 * MITM, no SC (the head link must never pair); lap_ble_pair changes them
 * only inside ble_gap_security_initiate, which builds the Pairing Request. */
#ifdef LS_LAP_CLIENT_HOST_TEST
#include "ble_client_host_fake.h"
#include "ls_lap_net_audio.h"
#else
#include "ble_hid.h"
#include "ble_link.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/ble_l2cap.h"
#include "host/ble_store.h"
#include "os/os_mbuf.h"
#include "tui/ls_lap_net_audio.h"
/* NimBLE private: the SM channel's receive hook, to hold a peer's Security
   Request for the app instead of pairing at once. Resolves against the
   nimble host include directory (host/include/../src). */
#include "../src/ble_hs_conn_priv.h"
void ble_hs_lock(void);
void ble_hs_unlock(void);
#endif
#define SM_OP_SEC_REQ 0x0b
static uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static int handle, operation, pair_op;
static uint16_t attribute, read_len;
static bool closing, connecting, wide, mtu_wait, pairing;
static int64_t rssi_ms;
static lap_ble_conn_info_t info;
static EXT_RAM_BSS_ATTR ls_lap_ble_command_t cmd;
static EXT_RAM_BSS_ATTR uint8_t read_buf[LAP_GATT_VALUE_MAX], note_buf[LAP_GATT_VALUE_MAX];
static EXT_RAM_BSS_ATTR hid_device_t hid_bonds[HID_BONDS];
static void uuid_copy(lap_uuid_t *out, const ble_uuid_t *uuid) {
    memset(out, 0, sizeof(*out));
    if (uuid->type == BLE_UUID_TYPE_128) {
        out->length = 16;
        memcpy(out->bytes, ((const ble_uuid128_t *)uuid)->value, 16);
    } else {
        out->length = uuid->type == BLE_UUID_TYPE_16 ? 2 : 4;
        uint32_t v = uuid->type == BLE_UUID_TYPE_16 ? ((const ble_uuid16_t *)uuid)->value
                                                    : ((const ble_uuid32_t *)uuid)->value;
        for (int i = 0; i < 4; i++)
            out->bytes[i] = v >> (8 * i);
    }
}
static void emit(int kind, int error, uint16_t attr, const void *data, size_t n) {
    lap_ble_event_t e = {
        .connection = handle, .operation = operation, .kind = kind, .error = error, .attribute = attr};
    ls_lap_ble_event(&e, data, n);
}
/* Refusals for missing pairing or encryption: pair, then retry. */
static int gatt_error(int status) {
    if (!status)
        return 0;
    if (status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN) ||
        status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_ENC) ||
        status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_KEY_SZ))
        return LAP_IO_EAUTH;
    return LAP_IO_EIO;
}
static void publish(void) {
    struct ble_gap_conn_desc d;
    if (conn == BLE_HS_CONN_HANDLE_NONE || ble_gap_conn_find(conn, &d))
        return;
    info.size = sizeof(info);
    info.version = 1;
    info.mtu = ble_att_mtu(conn);
    info.our_addr_type = d.our_id_addr.type & 1;
    info.peer_addr_type = d.peer_id_addr.type & 1;
    memcpy(info.peer_addr, d.peer_id_addr.val, 6);
    info.encrypted = d.sec_state.encrypted;
    info.authenticated = d.sec_state.authenticated;
    info.bonded = d.sec_state.bonded;
    info.key_size = d.sec_state.encrypted ? d.sec_state.key_size : 0;
    info.interval = d.conn_itvl;
    info.latency = d.conn_latency;
    info.timeout = d.supervision_timeout;
    ls_lap_ble_info(handle, &info);
}
static void security(int kind, int op, int error, uint8_t action, uint8_t authreq, uint32_t passkey) {
    struct ble_gap_conn_desc d;
    lap_ble_security_t s = {.size = sizeof(s), .version = 1, .action = action, .authreq = authreq, .passkey = passkey};
    if (conn != BLE_HS_CONN_HANDLE_NONE && !ble_gap_conn_find(conn, &d)) {
        s.encrypted = d.sec_state.encrypted;
        s.authenticated = d.sec_state.authenticated;
        s.bonded = d.sec_state.bonded;
        s.key_size = d.sec_state.encrypted ? d.sec_state.key_size : 0;
    }
    lap_ble_event_t e = {.connection = handle, .operation = op, .kind = kind, .error = error};
    ls_lap_ble_event(&e, &s, sizeof(s));
}
/* A peer's Security Request on the app's link goes to the app, never to
   NimBLE's automatic pairing. Everything else passes through. */
static ble_l2cap_rx_fn *sm_rx_next;
static int sm_gate(struct ble_l2cap_chan *chan, struct os_mbuf **om) {
    uint8_t pdu[2] = {0, 0};
    if (conn != BLE_HS_CONN_HANDLE_NONE && chan->conn_handle == conn && *om &&
        !os_mbuf_copydata(*om, 0, 1, pdu) && pdu[0] == SM_OP_SEC_REQ) {
        os_mbuf_copydata(*om, 1, 1, pdu + 1);
        if (!pairing)
            security(LAP_BLE_SECURITY_REQUEST, 0, 0, 0, pdu[1], 0);
        return 0;
    }
    return sm_rx_next ? sm_rx_next(chan, om) : BLE_HS_ENOTSUP;
}
static void sm_hold(void) {
    ble_hs_lock();
    struct ble_hs_conn *hc = ble_hs_conn_find(conn);
    struct ble_l2cap_chan *ch = hc ? ble_hs_conn_chan_find_by_scid(hc, BLE_L2CAP_CID_SM) : NULL;
    if (ch && ch->rx_fn != sm_gate) {
        sm_rx_next = ch->rx_fn;
        ch->rx_fn = sm_gate;
    }
    ble_hs_unlock();
}
/* NimBLE reads these globals only while it builds the Pairing Request inside
   ble_gap_security_initiate (later steps use the stored request), so they
   are back to the defaults before anything else runs on the host task. */
static int sm_initiate(unsigned flags) {
    uint8_t io = ble_hs_cfg.sm_io_cap, ours = ble_hs_cfg.sm_our_key_dist, theirs = ble_hs_cfg.sm_their_key_dist;
    unsigned bond = ble_hs_cfg.sm_bonding, mitm = ble_hs_cfg.sm_mitm, sc = ble_hs_cfg.sm_sc;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_KEYBOARD_DISPLAY;
    ble_hs_cfg.sm_bonding = (flags & LAP_BLE_PAIR_BOND) != 0;
    ble_hs_cfg.sm_mitm = (flags & LAP_BLE_PAIR_MITM) != 0;
#if CONFIG_BT_NIMBLE_SM_SC
    ble_hs_cfg.sm_sc = 1;
#endif
    if (flags & LAP_BLE_PAIR_BOND)
        ble_hs_cfg.sm_our_key_dist = ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    int rc = ble_gap_security_initiate(conn);
    ble_hs_cfg.sm_io_cap = io;
    ble_hs_cfg.sm_bonding = bond;
    ble_hs_cfg.sm_mitm = mitm;
    ble_hs_cfg.sm_sc = sc;
    ble_hs_cfg.sm_our_key_dist = ours;
    ble_hs_cfg.sm_their_key_dist = theirs;
    return rc;
}
/* Bonds of the board's own BLE HID accessories are not the app's to delete. */
static bool hid_owns(const ble_addr_t *a) {
    size_t n = hid_list_bonds(hid_bonds, HID_BONDS, NULL);
    for (size_t i = 0; i < n && i < HID_BONDS; i++)
        if (hid_bonds[i].address_type == a->type && !memcmp(hid_bonds[i].address, a->val, 6))
            return true;
    return false;
}
static int services(uint16_t c, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg) {
    if (c != conn || (int)(intptr_t)arg != handle)
        return 0;
    lap_ble_event_t e = {.connection = handle, .operation = operation, .kind = LAP_GATT_SERVICE};
    if (!err->status) {
        e.attribute = svc->start_handle;
        e.end_handle = svc->end_handle;
        uuid_copy(&e.uuid, &svc->uuid.u);
    } else {
        e.kind = LAP_GATT_COMPLETE;
        e.error = err->status == BLE_HS_EDONE ? 0 : LAP_IO_EIO;
    }
    ls_lap_ble_event(&e, NULL, 0);
    return 0;
}
static int chars(uint16_t c, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg) {
    if (c != conn || (int)(intptr_t)arg != handle)
        return 0;
    lap_ble_event_t e = {.connection = handle, .operation = operation, .kind = LAP_GATT_CHARACTERISTIC};
    if (!err->status) {
        e.attribute = chr->val_handle;
        e.end_handle = chr->def_handle;
        e.properties = chr->properties;
        uuid_copy(&e.uuid, &chr->uuid.u);
    } else {
        e.kind = LAP_GATT_COMPLETE;
        e.error = err->status == BLE_HS_EDONE ? 0 : LAP_IO_EIO;
    }
    ls_lap_ble_event(&e, NULL, 0);
    return 0;
}
static int descriptors(uint16_t c, const struct ble_gatt_error *err, uint16_t ch,
                       const struct ble_gatt_dsc *dsc, void *arg) {
    (void)ch;
    if (c != conn || (int)(intptr_t)arg != handle)
        return 0;
    lap_ble_event_t e = {.connection = handle, .operation = operation, .kind = LAP_GATT_DESCRIPTOR};
    if (!err->status) {
        e.attribute = dsc->handle;
        uuid_copy(&e.uuid, &dsc->uuid.u);
    } else {
        e.kind = LAP_GATT_COMPLETE;
        e.error = err->status == BLE_HS_EDONE ? 0 : LAP_IO_EIO;
    }
    ls_lap_ble_event(&e, NULL, 0);
    return 0;
}
/* Long read: one callback per piece, then EDONE. A value that ends exactly
   on a piece boundary may end with "not long" or "invalid offset" instead. */
static int read_value(uint16_t c, const struct ble_gatt_error *err, struct ble_gatt_attr *a, void *arg) {
    if (c != conn || (int)(intptr_t)arg != handle)
        return 0;
    int st = err->status;
    if (!st && a) {
        size_t n = OS_MBUF_PKTLEN(a->om);
        if (read_len + n > sizeof(read_buf)) {
            read_len = 0;
            emit(LAP_GATT_VALUE, LAP_IO_ELIMIT, attribute, NULL, 0);
            return 1; /* stops the procedure; no further callback */
        }
        if (os_mbuf_copydata(a->om, 0, n, read_buf + read_len)) {
            read_len = 0;
            emit(LAP_GATT_VALUE, LAP_IO_EIO, attribute, NULL, 0);
            return 1;
        }
        read_len += n;
        return 0;
    }
    bool done = st == BLE_HS_EDONE || (read_len && (st == BLE_HS_ATT_ERR(BLE_ATT_ERR_ATTR_NOT_LONG) ||
                                                    st == BLE_HS_ATT_ERR(BLE_ATT_ERR_INVALID_OFFSET)));
    if (done)
        emit(LAP_GATT_VALUE, 0, attribute, read_buf, read_len);
    else
        emit(LAP_GATT_VALUE, gatt_error(st), attribute, NULL, 0);
    read_len = 0;
    return 0;
}
static int written(uint16_t c, const struct ble_gatt_error *err, struct ble_gatt_attr *a, void *arg) {
    (void)a;
    if (c != conn || (int)(intptr_t)arg != handle)
        return 0;
    emit(LAP_GATT_WRITTEN, gatt_error(err->status), attribute, NULL, 0);
    return 0;
}
static void connected(void) {
    mtu_wait = false;
    publish();
    lap_ble_event_t e = {.connection = handle, .kind = LAP_BLE_CONNECTED};
    ls_lap_ble_event(&e, NULL, 0);
}
static int mtu_done(uint16_t c, const struct ble_gatt_error *err, uint16_t mtu, void *arg) {
    (void)err;
    (void)mtu;
    if (c == conn && (int)(intptr_t)arg == handle && mtu_wait)
        connected(); /* a refused exchange keeps MTU 23; still connected */
    return 0;
}
static int gap(struct ble_gap_event *e, void *arg) {
    if ((int)(intptr_t)arg != handle)
        return 0;
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
        connecting = false;
        if (e->connect.status) {
            emit(LAP_BLE_CONNECTED, LAP_IO_EIO, 0, NULL, 0);
            ls_lap_ble_closed(handle);
            handle = 0;
            return 0;
        }
        conn = e->connect.conn_handle;
        if (closing) {
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        sm_hold();
        memset(&info, 0, sizeof(info));
        info.rssi = 127;
        info.phy_tx = info.phy_rx = 1;
        {
            uint8_t tx, rx;
            if (!ble_gap_read_le_phy(conn, &tx, &rx)) {
                info.phy_tx = tx;
                info.phy_rx = rx;
            }
        }
        rssi_ms = esp_timer_get_time() / 1000;
        mtu_wait = wide && !ble_gattc_exchange_mtu(conn, mtu_done, (void *)(intptr_t)handle);
        if (!mtu_wait)
            connected();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        conn = BLE_HS_CONN_HANDLE_NONE;
        mtu_wait = pairing = false;
        read_len = 0;
        emit(LAP_BLE_DISCONNECTED, 0, 0, NULL, 0);
        ls_lap_ble_closed(handle);
        handle = 0;
        closing = false;
        break;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        size_t n = OS_MBUF_PKTLEN(e->notify_rx.om);
        if (n > sizeof(note_buf))
            emit(LAP_GATT_NOTIFY, LAP_IO_ELIMIT, e->notify_rx.attr_handle, NULL, 0);
        else if (!os_mbuf_copydata(e->notify_rx.om, 0, n, note_buf))
            emit(LAP_GATT_NOTIFY, 0, e->notify_rx.attr_handle, note_buf, n);
        break;
    }
    case BLE_GAP_EVENT_MTU:
    case BLE_GAP_EVENT_CONN_UPDATE:
        if (!mtu_wait)
            publish();
        break;
    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
        if (!e->phy_updated.status) {
            info.phy_tx = e->phy_updated.tx_phy;
            info.phy_rx = e->phy_updated.rx_phy;
            publish();
        }
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        if (!pairing || e->passkey.conn_handle != conn)
            return 0; /* nobody asked: the procedure times out */
        uint8_t action = e->passkey.params.action;
        if (action == BLE_SM_IOACT_DISP) {
            uint32_t r;
            do
                r = esp_random();
            while (r >= 4294000000u);
            struct ble_sm_io io = {.action = BLE_SM_IOACT_DISP, .passkey = r % 1000000u};
            if (ble_sm_inject_io(conn, &io))
                security(LAP_BLE_SECURITY, pair_op, LAP_IO_EAUTH, 0, 0, 0);
            else
                security(LAP_BLE_PASSKEY, pair_op, 0, LAP_BLE_PASSKEY_DISPLAY, 0, io.passkey);
        } else if (action == BLE_SM_IOACT_INPUT)
            security(LAP_BLE_PASSKEY, pair_op, 0, LAP_BLE_PASSKEY_INPUT, 0, 0);
        else if (action == BLE_SM_IOACT_NUMCMP)
            security(LAP_BLE_PASSKEY, pair_op, 0, LAP_BLE_PASSKEY_NUMCMP, 0, e->passkey.params.numcmp);
        else {
            pairing = false; /* out of band: not offered */
            security(LAP_BLE_SECURITY, pair_op, LAP_IO_ENOTSUP, 0, 0, 0);
        }
        break;
    }
    case BLE_GAP_EVENT_ENC_CHANGE: {
        if (e->enc_change.conn_handle != conn)
            return 0;
        int st = e->enc_change.status;
        publish();
        security(LAP_BLE_SECURITY, pairing ? pair_op : 0, !st ? 0 : st == BLE_HS_ETIMEOUT ? LAP_IO_ETIMEOUT : LAP_IO_EAUTH,
                 0, 0, 0);
        pairing = false;
        break;
    }
    /* REPEAT_PAIRING returns 0 (ignore): a stored bond is never replaced
       silently. The app may lap_ble_forget, then lap_ble_pair again. */
    default:
        break;
    }
    return 0;
}
/* Once a second while connected; a blocking HCI read on the host task. */
static void poll_rssi(void) {
    if (conn == BLE_HS_CONN_HANDLE_NONE || mtu_wait)
        return;
    int64_t now = esp_timer_get_time() / 1000;
    if (now - rssi_ms < 1000)
        return;
    rssi_ms = now;
    int8_t r;
    if (!ble_gap_conn_rssi(conn, &r)) {
        info.rssi = r;
        publish();
    }
}
static void pair(unsigned flags) {
    struct ble_gap_conn_desc d;
    pair_op = cmd.operation;
    if (!ble_gap_conn_find(conn, &d) && d.sec_state.encrypted &&
        (d.sec_state.authenticated || !(flags & LAP_BLE_PAIR_MITM)) && (d.sec_state.bonded || !(flags & LAP_BLE_PAIR_BOND))) {
        security(LAP_BLE_SECURITY, pair_op, 0, 0, 0, 0); /* already as asked */
        return;
    }
    pairing = true;
    int rc = sm_initiate(flags);
    if (rc) {
        pairing = false;
        security(LAP_BLE_SECURITY, pair_op, rc == BLE_HS_EALREADY ? LAP_IO_EBUSY : LAP_IO_EAUTH, 0, 0, 0);
    }
}
static void answer(void) {
    if (!pairing)
        return;
    struct ble_sm_io io = {.action = cmd.flags};
    if (cmd.flags == LAP_BLE_PASSKEY_INPUT)
        io.passkey = cmd.value;
    else
        io.numcmp_accept = cmd.value != 0;
    if (ble_sm_inject_io(conn, &io)) {
        pairing = false;
        security(LAP_BLE_SECURITY, pair_op, LAP_IO_EAUTH, 0, 0, 0);
    }
}
static void forget(void) {
    struct ble_gap_conn_desc d;
    int error = LAP_IO_EIO;
    if (!ble_gap_conn_find(conn, &d)) {
        error = hid_owns(&d.peer_id_addr) ? LAP_IO_EACCES : 0;
        if (!error) {
            int rc = ble_store_util_delete_peer(&d.peer_id_addr);
            error = rc && rc != BLE_HS_ENOENT ? LAP_IO_EIO : 0;
        }
    }
    emit(LAP_GATT_COMPLETE, error, 0, NULL, 0);
}
void ls_lap_ble_host_pump(void) {
    poll_rssi();
    if (!ls_lap_ble_command(&cmd))
        return;
    ls_lap_ble_command_t *c = &cmd;
    if (c->kind == LS_BLE_CMD_CLOSE) {
        if (handle == c->handle) {
            closing = true;
            if (connecting)
                ble_gap_conn_cancel();
            else if (conn != BLE_HS_CONN_HANDLE_NONE)
                ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
            else {
                ls_lap_ble_closed(handle);
                handle = 0;
            }
        } else
            ls_lap_ble_closed(c->handle);
        return;
    }
    int rc = 0;
    if (c->kind == LS_BLE_CMD_CONNECT) {
        if (handle || ble_gap_conn_active() ||
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT) < 8192) {
            lap_ble_event_t e = {.connection = c->handle, .kind = LAP_BLE_CONNECTED, .error = LAP_IO_EBUSY};
            ls_lap_ble_event(&e, NULL, 0);
            return;
        }
        handle = c->handle;
        operation = 0;
        closing = mtu_wait = pairing = false;
        wide = c->flags & LS_BLE_CONNECT_WIDE;
        ble_addr_t peer = {.type = c->addr_type};
        memcpy(peer.val, c->addr, 6);
        ble_gap_disc_cancel();
        rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &peer, 10000, NULL, gap, (void *)(intptr_t)handle);
        connecting = !rc;
    } else {
        if (c->handle != handle || conn == BLE_HS_CONN_HANDLE_NONE)
            return;
        operation = c->operation;
        attribute = c->attribute;
        void *token = (void *)(intptr_t)handle;
        switch (c->kind) {
        case LS_BLE_CMD_DISCOVER:
            rc = c->attribute ? ble_gattc_disc_all_chrs(conn, c->attribute, c->end, chars, token)
                              : ble_gattc_disc_all_svcs(conn, services, token);
            break;
        case LS_BLE_CMD_READ:
            read_len = 0;
            rc = ble_gattc_read_long(conn, c->attribute, 0, read_value, token);
            break;
        case LS_BLE_CMD_WRITE:
            if (c->len <= ble_att_mtu(conn) - 3)
                rc = ble_gattc_write_flat(conn, c->attribute, c->data, c->len, written, token);
            else {
                struct os_mbuf *om = ble_hs_mbuf_from_flat(c->data, c->len);
                rc = om ? ble_gattc_write_long(conn, c->attribute, 0, om, written, token) : BLE_HS_ENOMEM;
            }
            break;
        case LS_BLE_CMD_WRITE_CMD: {
            /* No reply comes from the peer: WRITTEN reports the send itself. */
            int error = 0;
            if (c->len > ble_att_mtu(conn) - 3)
                error = LAP_IO_ELIMIT;
            else {
                int r = ble_gattc_write_no_rsp_flat(conn, c->attribute, c->data, c->len);
                error = !r ? 0 : r == BLE_HS_ENOMEM ? LAP_IO_EBUSY : LAP_IO_EIO;
            }
            emit(LAP_GATT_WRITTEN, error, attribute, NULL, 0);
            return;
        }
        case LS_BLE_CMD_DESCRIPTORS:
            rc = ble_gattc_disc_all_dscs(conn, c->attribute, c->end, descriptors, token);
            break;
        case LS_BLE_CMD_PAIR:
            pair(c->flags);
            return;
        case LS_BLE_CMD_PASSKEY:
            answer();
            return;
        case LS_BLE_CMD_FORGET:
            forget();
            return;
        }
    }
    if (rc) {
        emit(c->kind == LS_BLE_CMD_CONNECT ? LAP_BLE_CONNECTED : LAP_GATT_COMPLETE, LAP_IO_EIO, c->attribute, NULL, 0);
        if (c->kind == LS_BLE_CMD_CONNECT)
            handle = 0;
    }
}
