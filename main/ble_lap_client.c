/* One SDK central on the existing NimBLE host queue. No tasks or large
 * internal arrays. Values stay at MTU 23; queues/data live in apps' PSRAM.
 * Do not move NimBLE transport/DMA buffers or NVS-capable stacks to PSRAM. */
#include "ble_link.h"
#include "esp_heap_caps.h"
#include "host/ble_hs.h"
#include "os/os_mbuf.h"
#include "tui/ls_lap_net_audio.h"
static uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static int handle, operation;
static uint16_t attribute;
static bool closing, connecting;
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
static int read_value(uint16_t c, const struct ble_gatt_error *err, struct ble_gatt_attr *a, void *arg) {
    if (c != conn || (int)(intptr_t)arg != handle)
        return 0;
    uint8_t value[20];
    size_t n = 0;
    int error = err->status ? LAP_IO_EIO : 0;
    if (!error) {
        n = OS_MBUF_PKTLEN(a->om);
        if (n > sizeof(value))
            error = LAP_IO_ELIMIT;
        else if (os_mbuf_copydata(a->om, 0, n, value))
            error = LAP_IO_EIO;
    }
    emit(LAP_GATT_VALUE, error, attribute, value, error ? 0 : n);
    return 0;
}
static int written(uint16_t c, const struct ble_gatt_error *err, struct ble_gatt_attr *a, void *arg) {
    (void)a;
    if (c != conn || (int)(intptr_t)arg != handle)
        return 0;
    emit(LAP_GATT_WRITTEN, err->status ? LAP_IO_EIO : 0, attribute, NULL, 0);
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
        emit(LAP_BLE_CONNECTED, 0, 0, NULL, 0);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        conn = BLE_HS_CONN_HANDLE_NONE;
        emit(LAP_BLE_DISCONNECTED, 0, 0, NULL, 0);
        ls_lap_ble_closed(handle);
        handle = 0;
        closing = false;
        break;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint8_t value[20];
        size_t n = OS_MBUF_PKTLEN(e->notify_rx.om);
        if (n > 20)
            emit(LAP_GATT_NOTIFY, LAP_IO_ELIMIT, e->notify_rx.attr_handle, NULL, 0);
        else if (!os_mbuf_copydata(e->notify_rx.om, 0, n, value))
            emit(LAP_GATT_NOTIFY, 0, e->notify_rx.attr_handle, value, n);
        break;
    }
    /* Security-required ATT errors go back to the app. No silent bond changes. */
    default:
        break;
    }
    return 0;
}
void ls_lap_ble_host_pump(void) {
    ls_lap_ble_command_t c;
    if (!ls_lap_ble_command(&c))
        return;
    if (c.kind == 5) {
        if (handle == c.handle) {
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
            ls_lap_ble_closed(c.handle);
        return;
    }
    int rc = 0;
    if (c.kind == 1) {
        if (handle || ble_gap_conn_active() ||
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT) < 8192) {
            lap_ble_event_t e = {.connection = c.handle, .kind = LAP_BLE_CONNECTED, .error = LAP_IO_EBUSY};
            ls_lap_ble_event(&e, NULL, 0);
            return;
        }
        handle = c.handle;
        operation = 0;
        closing = false;
        ble_addr_t peer = {.type = c.addr_type};
        memcpy(peer.val, c.addr, 6);
        ble_gap_disc_cancel();
        rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &peer, 10000, NULL, gap, (void *)(intptr_t)handle);
        connecting = !rc;
    } else {
        if (c.handle != handle || conn == BLE_HS_CONN_HANDLE_NONE)
            return;
        operation = c.operation;
        attribute = c.attribute;
        void *token = (void *)(intptr_t)handle;
        switch (c.kind) {
        case 2:
            rc = c.attribute ? ble_gattc_disc_all_chrs(conn, c.attribute, c.end, chars, token)
                             : ble_gattc_disc_all_svcs(conn, services, token);
            break;
        case 3:
            rc = ble_gattc_read(conn, c.attribute, read_value, token);
            break;
        case 4:
            rc = ble_gattc_write_flat(conn, c.attribute, c.data, c.len, written, token);
            break;
        case 6:
            rc = ble_gattc_disc_all_dscs(conn, c.attribute, c.end, descriptors, token);
            break;
        }
    }
    if (rc) {
        emit(c.kind == 1 ? LAP_BLE_CONNECTED : LAP_GATT_COMPLETE, LAP_IO_EIO, c.attribute, NULL, 0);
        if (c.kind == 1)
            handle = 0;
    }
}
