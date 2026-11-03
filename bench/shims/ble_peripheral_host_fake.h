/* NimBLE fake for compiling/exercising the actual firmware peripheral adapter.
 * Function/field names match the local IDF 5.5.4 public headers. No radio I/O.
 */
#ifndef BLE_PERIPHERAL_HOST_FAKE_H
#define BLE_PERIPHERAL_HOST_FAKE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define CONFIG_LS_LAP_BLE_ADVERTISER 1
#ifndef CONFIG_LS_LAP_GATT_SERVER
#define CONFIG_LS_LAP_GATT_SERVER 1
#endif
#define EXT_RAM_BSS_ATTR
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define BLE_ERR_REM_USER_CONN_TERM 0x13
#define BLE_ERR_AUTH_FAIL 5
#define BLE_OWN_ADDR_PUBLIC 0
#define BLE_HCI_LE_PHY_1M 1
#define BLE_HS_EDONE 14
#define BLE_HS_ENOMEM 6
#define BLE_HS_EBUSY 15
#define BLE_HS_IO_NO_INPUT_OUTPUT 3
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define BLE_UUID_TYPE_16 16
#define BLE_UUID_TYPE_32 32
#define BLE_UUID_TYPE_128 128
#define BLE_GATT_SVC_TYPE_PRIMARY 1
#define BLE_GATT_ACCESS_OP_READ_CHR 0
#define BLE_GATT_ACCESS_OP_WRITE_CHR 1
#define BLE_GATT_ACCESS_OP_READ_DSC 2
#define BLE_GATT_ACCESS_OP_WRITE_DSC 3
#define BLE_ATT_ERR_UNLIKELY 0x0e
#define BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN 0x0d
#define BLE_ATT_ERR_INSUFFICIENT_RES 0x11
#define BLE_GATT_CHR_F_READ 2
#define BLE_GATT_CHR_F_WRITE_NO_RSP 4
#define BLE_GATT_CHR_F_WRITE 8
#define BLE_GATT_CHR_F_NOTIFY 16
#define BLE_GATT_CHR_F_INDICATE 32
#define BLE_GATT_CHR_F_READ_ENC 0x200
#define BLE_GATT_CHR_F_READ_AUTHEN 0x400
#define BLE_GATT_CHR_F_WRITE_ENC 0x1000
#define BLE_GATT_CHR_F_WRITE_AUTHEN 0x2000
#define BLE_ATT_F_READ 1
#define BLE_ATT_F_WRITE 2
#define BLE_ATT_F_READ_ENC 4
#define BLE_ATT_F_READ_AUTHEN 8
#define BLE_ATT_F_WRITE_ENC 32
#define BLE_ATT_F_WRITE_AUTHEN 64
struct os_mbuf {
    uint8_t data[256];
    uint16_t len;
};
#define OS_MBUF_PKTLEN(m) ((m)->len)
typedef struct {
    uint8_t type;
} ble_uuid_t;
typedef union {
    ble_uuid_t u;
    struct {
        ble_uuid_t u;
        uint16_t value;
    } u16;
    struct {
        ble_uuid_t u;
        uint32_t value;
    } u32;
    struct {
        ble_uuid_t u;
        uint8_t value[16];
    } u128;
} ble_uuid_any_t;
struct ble_gatt_access_ctxt {
    uint8_t op;
    struct os_mbuf *om;
    uint16_t offset;
};
typedef int ble_gatt_access_fn(uint16_t, uint16_t, struct ble_gatt_access_ctxt *, void *);
struct ble_gatt_dsc_def {
    const ble_uuid_t *uuid;
    uint8_t att_flags, min_key_size;
    ble_gatt_access_fn *access_cb;
    void *arg;
};
struct ble_gatt_chr_def {
    const ble_uuid_t *uuid;
    ble_gatt_access_fn *access_cb;
    void *arg;
    struct ble_gatt_dsc_def *descriptors;
    uint32_t flags;
    uint8_t min_key_size;
    uint16_t *val_handle;
    void *cpfd;
};
struct ble_gatt_svc_def {
    uint8_t type;
    const ble_uuid_t *uuid;
    const struct ble_gatt_svc_def **includes;
    const struct ble_gatt_chr_def *characteristics;
};
enum {
    BLE_GAP_EVENT_CONNECT = 1,
    BLE_GAP_EVENT_DISCONNECT,
    BLE_GAP_EVENT_MTU,
    BLE_GAP_EVENT_ENC_CHANGE,
    BLE_GAP_EVENT_PASSKEY_ACTION,
    BLE_GAP_EVENT_SUBSCRIBE,
    BLE_GAP_EVENT_NOTIFY_TX,
    BLE_GAP_EVENT_ADV_COMPLETE
};
struct ble_gap_event {
    int type;
    union {
        struct {
            int status;
            uint16_t conn_handle;
        } connect;
        struct {
            int reason;
        } disconnect;
        struct {
            uint16_t value;
        } mtu;
        struct {
            int status;
        } enc_change;
        struct {
            uint16_t attr_handle;
            uint8_t cur_notify, cur_indicate;
        } subscribe;
        struct {
            uint16_t attr_handle;
            uint8_t indication;
            int status;
        } notify_tx;
        struct {
            int reason;
        } adv_complete;
    };
};
struct ble_gap_conn_desc {
    struct {
        uint8_t bonded, encrypted, authenticated;
    } sec_state;
};
struct ble_gap_ext_adv_params {
    bool legacy_pdu, connectable, scannable;
    uint8_t own_addr_type, primary_phy, secondary_phy;
    uint32_t itvl_min, itvl_max;
    int8_t tx_power;
};
typedef int ble_gap_event_fn(struct ble_gap_event *, void *);
extern struct fake_ble_config {
    uint8_t sm_io_cap, sm_bonding, sm_mitm, sm_sc;
} ble_hs_cfg;
int ble_hs_synced(void);
size_t heap_caps_get_largest_free_block(unsigned);
size_t heap_caps_get_free_size(unsigned);
int ble_gatts_reset(void);
void ble_gatts_stop(void);
typedef void (*ble_gatt_svc_foreach_fn)(const struct ble_gatt_svc_def *, uint16_t, uint16_t,
                                        void *);
void ble_gatts_lcl_svc_foreach(ble_gatt_svc_foreach_fn, void *);
void ble_svc_gap_init(void);
void ble_svc_gatt_init(void);
int ble_gatts_start(void);
int ble_gatts_count_cfg(const struct ble_gatt_svc_def *);
int ble_gatts_add_svcs(const struct ble_gatt_svc_def *);
int ble_gap_ext_adv_stop(uint8_t);
int ble_gap_ext_adv_active(uint8_t);
int ble_gap_ext_adv_remove(uint8_t);
int ble_gap_ext_adv_configure(uint8_t, const struct ble_gap_ext_adv_params *, int8_t *,
                              ble_gap_event_fn *, void *);
int ble_gap_ext_adv_set_data(uint8_t, struct os_mbuf *);
int ble_gap_ext_adv_rsp_set_data(uint8_t, struct os_mbuf *);
int ble_gap_ext_adv_start(uint8_t, int, int);
int ble_gap_terminate(uint16_t, uint8_t);
int ble_gap_conn_find(uint16_t, struct ble_gap_conn_desc *);
struct os_mbuf *ble_hs_mbuf_from_flat(const void *, uint16_t);
int os_mbuf_copydata(const struct os_mbuf *, int, int, void *);
int os_mbuf_append(struct os_mbuf *, const void *, uint16_t);
int ble_gatts_indicate_custom(uint16_t, uint16_t, struct os_mbuf *);
int ble_gatts_notify_custom(uint16_t, uint16_t, struct os_mbuf *);
#endif
