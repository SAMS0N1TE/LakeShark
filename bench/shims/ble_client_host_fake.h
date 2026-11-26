/* NimBLE fake for compiling and driving the actual main/ble_lap_client.c.
 * Names match the local IDF 5.5.4 headers (public and the two private SM
 * channel pieces the client hooks). No radio I/O. */
#ifndef BLE_CLIENT_HOST_FAKE_H
#define BLE_CLIENT_HOST_FAKE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif
#define CONFIG_BT_NIMBLE_SM_SC 1
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define BLE_ERR_REM_USER_CONN_TERM 0x13
#define BLE_OWN_ADDR_PUBLIC 0
#define BLE_HS_EALREADY 2
#define BLE_HS_ENOENT 5
#define BLE_HS_ENOMEM 6
#define BLE_HS_ENOTSUP 8
#define BLE_HS_ETIMEOUT 13
#define BLE_HS_EDONE 14
#define BLE_HS_ERR_ATT_BASE 0x100
#define BLE_HS_ATT_ERR(x) ((x) ? BLE_HS_ERR_ATT_BASE + (x) : 0)
#define BLE_ATT_ERR_INSUFFICIENT_AUTHEN 0x05
#define BLE_ATT_ERR_INVALID_OFFSET 0x07
#define BLE_ATT_ERR_ATTR_NOT_LONG 0x0b
#define BLE_ATT_ERR_INSUFFICIENT_KEY_SZ 0x0c
#define BLE_ATT_ERR_INSUFFICIENT_ENC 0x0f
#define BLE_HS_IO_NO_INPUT_OUTPUT 0x03
#define BLE_HS_IO_KEYBOARD_DISPLAY 0x04
#define BLE_SM_PAIR_KEY_DIST_ENC 0x01
#define BLE_SM_PAIR_KEY_DIST_ID 0x02
#define BLE_SM_IOACT_NONE 0
#define BLE_SM_IOACT_OOB 1
#define BLE_SM_IOACT_INPUT 2
#define BLE_SM_IOACT_DISP 3
#define BLE_SM_IOACT_NUMCMP 4
#define BLE_L2CAP_CID_SM 6
#define BLE_UUID_TYPE_16 16
#define BLE_UUID_TYPE_32 32
#define BLE_UUID_TYPE_128 128
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_DMA 2
#define MALLOC_CAP_8BIT 4
struct os_mbuf {
    uint8_t data[640];
    uint16_t len;
};
#define OS_MBUF_PKTLEN(m) ((m)->len)
int os_mbuf_copydata(const struct os_mbuf *, int, int, void *);
struct os_mbuf *ble_hs_mbuf_from_flat(const void *, uint16_t);
typedef struct { uint8_t type; } ble_uuid_t;
typedef struct { ble_uuid_t u; uint16_t value; } ble_uuid16_t;
typedef struct { ble_uuid_t u; uint32_t value; } ble_uuid32_t;
typedef struct { ble_uuid_t u; uint8_t value[16]; } ble_uuid128_t;
typedef union { ble_uuid_t u; ble_uuid16_t u16; ble_uuid32_t u32; ble_uuid128_t u128; } ble_uuid_any_t;
typedef struct { uint8_t type, val[6]; } ble_addr_t;
struct ble_gatt_error { uint16_t status, att_handle; };
struct ble_gatt_svc { uint16_t start_handle, end_handle; ble_uuid_any_t uuid; };
struct ble_gatt_chr { uint16_t def_handle, val_handle; uint8_t properties; ble_uuid_any_t uuid; };
struct ble_gatt_dsc { uint16_t handle; ble_uuid_any_t uuid; };
struct ble_gatt_attr { uint16_t handle, offset; struct os_mbuf *om; };
typedef int ble_gatt_disc_svc_fn(uint16_t, const struct ble_gatt_error *, const struct ble_gatt_svc *, void *);
typedef int ble_gatt_chr_fn(uint16_t, const struct ble_gatt_error *, const struct ble_gatt_chr *, void *);
typedef int ble_gatt_dsc_fn(uint16_t, const struct ble_gatt_error *, uint16_t, const struct ble_gatt_dsc *, void *);
typedef int ble_gatt_attr_fn(uint16_t, const struct ble_gatt_error *, struct ble_gatt_attr *, void *);
typedef int ble_gatt_mtu_fn(uint16_t, const struct ble_gatt_error *, uint16_t, void *);
enum {
    BLE_GAP_EVENT_CONNECT = 0, BLE_GAP_EVENT_DISCONNECT = 1, BLE_GAP_EVENT_CONN_UPDATE = 3,
    BLE_GAP_EVENT_ENC_CHANGE = 10, BLE_GAP_EVENT_PASSKEY_ACTION = 11, BLE_GAP_EVENT_NOTIFY_RX = 12,
    BLE_GAP_EVENT_MTU = 15, BLE_GAP_EVENT_REPEAT_PAIRING = 17, BLE_GAP_EVENT_PHY_UPDATE_COMPLETE = 18
};
struct ble_gap_passkey_params { uint8_t action; uint32_t numcmp; };
struct ble_gap_event {
    uint8_t type;
    union {
        struct { int status; uint16_t conn_handle; } connect;
        struct { int reason; uint16_t conn_handle; } disconnect;
        struct { int status; uint16_t conn_handle; } conn_update;
        struct { int status; uint16_t conn_handle; } enc_change;
        struct { struct ble_gap_passkey_params params; uint16_t conn_handle; } passkey;
        struct { struct os_mbuf *om; uint16_t conn_handle, attr_handle; uint8_t indication; } notify_rx;
        struct { uint16_t conn_handle, channel_id, value; } mtu;
        struct { int status; uint16_t conn_handle; uint8_t tx_phy, rx_phy; } phy_updated;
    };
};
typedef int ble_gap_event_fn(struct ble_gap_event *, void *);
struct ble_gap_sec_state {
    unsigned encrypted : 1, authenticated : 1, bonded : 1, key_size : 5, authorize : 1;
};
struct ble_gap_conn_desc {
    struct ble_gap_sec_state sec_state;
    ble_addr_t our_id_addr, peer_id_addr, our_ota_addr, peer_ota_addr;
    uint16_t conn_handle, conn_itvl, conn_latency, supervision_timeout;
    uint8_t role, master_clock_accuracy;
};
struct ble_sm_io {
    uint8_t action;
    union {
        uint32_t passkey;
        uint8_t oob[16];
        uint8_t numcmp_accept;
    };
};
extern struct fake_hs_cfg {
    uint8_t sm_io_cap;
    unsigned sm_bonding : 1, sm_mitm : 1, sm_sc : 1;
    uint8_t sm_our_key_dist, sm_their_key_dist;
} ble_hs_cfg;
int ble_gattc_disc_all_svcs(uint16_t, ble_gatt_disc_svc_fn *, void *);
int ble_gattc_disc_all_chrs(uint16_t, uint16_t, uint16_t, ble_gatt_chr_fn *, void *);
int ble_gattc_disc_all_dscs(uint16_t, uint16_t, uint16_t, ble_gatt_dsc_fn *, void *);
int ble_gattc_read_long(uint16_t, uint16_t, uint16_t, ble_gatt_attr_fn *, void *);
int ble_gattc_write_flat(uint16_t, uint16_t, const void *, uint16_t, ble_gatt_attr_fn *, void *);
int ble_gattc_write_long(uint16_t, uint16_t, uint16_t, struct os_mbuf *, ble_gatt_attr_fn *, void *);
int ble_gattc_write_no_rsp_flat(uint16_t, uint16_t, const void *, uint16_t);
int ble_gattc_exchange_mtu(uint16_t, ble_gatt_mtu_fn *, void *);
uint16_t ble_att_mtu(uint16_t);
int ble_gap_connect(uint8_t, const ble_addr_t *, int32_t, const void *, ble_gap_event_fn *, void *);
int ble_gap_conn_cancel(void);
int ble_gap_conn_active(void);
int ble_gap_disc_cancel(void);
int ble_gap_terminate(uint16_t, uint8_t);
int ble_gap_conn_find(uint16_t, struct ble_gap_conn_desc *);
int ble_gap_read_le_phy(uint16_t, uint8_t *, uint8_t *);
int ble_gap_conn_rssi(uint16_t, int8_t *);
int ble_gap_security_initiate(uint16_t);
int ble_sm_inject_io(uint16_t, struct ble_sm_io *);
int ble_store_util_delete_peer(const ble_addr_t *);
size_t heap_caps_get_largest_free_block(unsigned);
uint32_t esp_random(void);
int64_t esp_timer_get_time(void);
/* Private NimBLE pieces (ble_l2cap_priv.h, ble_hs_conn_priv.h). */
struct ble_l2cap_chan;
typedef int ble_l2cap_rx_fn(struct ble_l2cap_chan *chan, struct os_mbuf **om);
struct ble_l2cap_chan { uint16_t conn_handle, dcid, scid; ble_l2cap_rx_fn *rx_fn; };
struct ble_hs_conn;
struct ble_hs_conn *ble_hs_conn_find(uint16_t);
struct ble_l2cap_chan *ble_hs_conn_chan_find_by_scid(struct ble_hs_conn *, uint16_t);
void ble_hs_lock(void);
void ble_hs_unlock(void);
/* ble_hid.h */
enum { HID_BONDS = 4 };
typedef struct { uint8_t address[6], address_type; } hid_device_t;
size_t hid_list_bonds(hid_device_t *out, size_t cap, uint32_t *version);
#endif
