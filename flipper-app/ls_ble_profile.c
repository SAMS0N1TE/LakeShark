#include "ls_ble_profile.h"

#include <furi.h>
#include <furi_hal_version.h>
#include <furi_ble/gatt.h>
#include <furi_ble/event_dispatcher.h>

#include <ble/core/ble_defs.h>
#include <ble/core/ble_std.h>
#include <ble/core/auto/ble_types.h>

#include "ls_dbg.h"

#define TAG "LsBleProfile"

#define ACI_GATT_ATTRIBUTE_MODIFIED_VSEVT_CODE 0x0C01U

typedef __PACKED_STRUCT {
    uint8_t type;
    uint8_t data[1];
}
LsHciUartPckt;

typedef __PACKED_STRUCT {
    uint8_t evt;
    uint8_t plen;
    uint8_t data[1];
}
LsHciEventPckt;

typedef __PACKED_STRUCT {
    uint16_t ecode;
    uint8_t data[1];
}
LsEvtBlecoreAci;

#define LS_UUID_BYTES(last)                                                            \
    {                                                                                  \
        (last), 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4b, 0x52, 0x41, 0x48, 0x53, 0x45, \
            0x4b, 0x41, 0x4c                                                           \
    }

static const uint8_t ls_uuid_service[16] = LS_UUID_BYTES(0x01);

#define LS_FRAME_MAX 244

typedef enum {
    LsCharRx,
    LsCharTx,
    LsCharCount,
} LsCharIdx;

typedef struct {
    FuriHalBleProfileBase base;

    uint16_t svc_handle;
    BleGattCharacteristicInstance chars[LsCharCount];
    GapSvcEventHandler* event_handler;

    LsBleProfileRxCallback rx_cb;
    void* rx_ctx;
} LsBleProfileInstance;

typedef struct {
    const uint8_t* data;
    uint16_t len;
} LsTxFrame;

static bool ls_tx_data_cb(const void* context, const uint8_t** data, uint16_t* data_len) {

    const LsTxFrame* frame = context;
    if(!frame || !data) {
        if(data_len) *data_len = LS_FRAME_MAX;
        return false;
    }
    *data = frame->data;
    *data_len = frame->len;
    return false;
}

static const BleGattCharacteristicParams ls_char_templates[LsCharCount] = {
    [LsCharRx] =
        {.name = "LS RX",
         .data_prop_type = FlipperGattCharacteristicDataFixed,
         .data.fixed.length = LS_FRAME_MAX,
         .uuid.Char_UUID_128 = LS_UUID_BYTES(0x02),
         .uuid_type = UUID_TYPE_128,
         .char_properties = CHAR_PROP_WRITE | CHAR_PROP_WRITE_WITHOUT_RESP,
         .security_permissions = ATTR_PERMISSION_NONE,
         .gatt_evt_mask = GATT_NOTIFY_ATTRIBUTE_WRITE,
         .is_variable = CHAR_VALUE_LEN_VARIABLE},
    [LsCharTx] =
        {.name = "LS TX",
         .data_prop_type = FlipperGattCharacteristicDataCallback,
         .data.callback.fn = ls_tx_data_cb,
         .data.callback.context = NULL,
         .uuid.Char_UUID_128 = LS_UUID_BYTES(0x03),
         .uuid_type = UUID_TYPE_128,

         .char_properties = CHAR_PROP_NOTIFY,
         .security_permissions = ATTR_PERMISSION_NONE,
         .gatt_evt_mask = GATT_DONT_NOTIFY_EVENTS,
         .is_variable = CHAR_VALUE_LEN_VARIABLE},
};

uint16_t ls_ble_profile_max_frame(void) {
    return LS_FRAME_MAX;
}

static BleEventAckStatus ls_profile_event_handler(void* event, void* context) {
    LsBleProfileInstance* inst = context;
    BleEventAckStatus ret = BleEventNotAck;

    LsHciEventPckt* pckt = (LsHciEventPckt*)(((LsHciUartPckt*)event)->data);
    if(pckt->evt != HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE) return ret;

    LsEvtBlecoreAci* blecore = (LsEvtBlecoreAci*)pckt->data;
    if(blecore->ecode != ACI_GATT_ATTRIBUTE_MODIFIED_VSEVT_CODE) return ret;

    aci_gatt_attribute_modified_event_rp0* mod =
        (aci_gatt_attribute_modified_event_rp0*)blecore->data;

    if(mod->Attr_Handle == inst->chars[LsCharRx].handle + 1) {
        if(inst->rx_cb && mod->Attr_Data_Length) {
            inst->rx_cb(mod->Attr_Data, mod->Attr_Data_Length, inst->rx_ctx);
        }
        ret = BleEventAckFlowEnable;
    }
    return ret;
}

static FuriHalBleProfileBase* ls_profile_start(FuriHalBleProfileParams params) {
    UNUSED(params);

    ls_dbg("    profile_start: entered");
    LsBleProfileInstance* inst = malloc(sizeof(LsBleProfileInstance));
    memset(inst, 0, sizeof(LsBleProfileInstance));
    inst->base.config = ls_ble_profile;

    Service_UUID_t svc_uuid;
    memcpy(svc_uuid.Service_UUID_128, ls_uuid_service, sizeof(ls_uuid_service));

    ls_dbg("    profile_start: adding service");
    if(!ble_gatt_service_add(UUID_TYPE_128, &svc_uuid, PRIMARY_SERVICE, 8, &inst->svc_handle)) {
        FURI_LOG_E(TAG, "service add failed");
        free(inst);
        return NULL;
    }

    ls_dbg("    profile_start: service ok (handle=%u), adding chars", inst->svc_handle);
    for(size_t i = 0; i < LsCharCount; i++) {
        ls_dbg("      char[%u] '%s' init...", (unsigned)i, ls_char_templates[i].name);
        ble_gatt_characteristic_init(inst->svc_handle, &ls_char_templates[i], &inst->chars[i]);
        ls_dbg("      char[%u] ok, handle=%u", (unsigned)i, inst->chars[i].handle);
    }

    ls_dbg("    profile_start: chars ok, registering handler");
    inst->event_handler = ble_event_dispatcher_register_svc_handler(ls_profile_event_handler, inst);

    FURI_LOG_I(
        TAG, "profile up: svc=%u rx=%u tx=%u", inst->svc_handle,
        inst->chars[LsCharRx].handle, inst->chars[LsCharTx].handle);
    return &inst->base;
}

static void ls_profile_stop(FuriHalBleProfileBase* profile) {
    furi_check(profile);
    furi_check(profile->config == ls_ble_profile);
    LsBleProfileInstance* inst = (LsBleProfileInstance*)profile;

    if(inst->event_handler) {
        ble_event_dispatcher_unregister_svc_handler(inst->event_handler);
        inst->event_handler = NULL;
    }
    for(size_t i = 0; i < LsCharCount; i++) {
        ble_gatt_characteristic_delete(inst->svc_handle, &inst->chars[i]);
    }
    ble_gatt_service_delete(inst->svc_handle);
    free(inst);
}

static void ls_profile_get_gap_config(GapConfig* config, FuriHalBleProfileParams params) {
    UNUSED(params);
    furi_check(config);
    ls_dbg("    get_gap_config: entered");
    memset(config, 0, sizeof(GapConfig));

    config->adv_service.UUID_Type = UUID_TYPE_16;
    config->adv_service.Service_UUID_16 = 0x18FF;

    config->pairing_method = GapPairingNone;
    config->bonding_mode = true;

    memcpy(config->mac_address, furi_hal_version_get_ble_mac(), sizeof(config->mac_address));

    config->mac_address[2]++;

    config->conn_param.conn_int_min = 6;
    config->conn_param.conn_int_max = 36;
    config->conn_param.slave_latency = 0;
    config->conn_param.supervisor_timeout = 0x100;
    config->appearance_char = 0x8600;

    strlcpy(
        config->adv_name,
        furi_hal_version_get_ble_local_device_name_ptr(),
        FURI_HAL_VERSION_DEVICE_NAME_LENGTH);
}

static const FuriHalBleProfileTemplate ls_profile_template = {
    .start = ls_profile_start,
    .stop = ls_profile_stop,
    .get_gap_config = ls_profile_get_gap_config,
};

const FuriHalBleProfileTemplate* ls_ble_profile = &ls_profile_template;

bool ls_ble_profile_tx(FuriHalBleProfileBase* profile, const uint8_t* data, uint16_t len) {
    furi_check(profile);
    furi_check(profile->config == ls_ble_profile);
    if(!data || !len || len > LS_FRAME_MAX) return false;

    LsBleProfileInstance* inst = (LsBleProfileInstance*)profile;
    LsTxFrame frame = {.data = data, .len = len};
    return ble_gatt_characteristic_update(inst->svc_handle, &inst->chars[LsCharTx], &frame);
}

void ls_ble_profile_set_rx_callback(
    FuriHalBleProfileBase* profile,
    LsBleProfileRxCallback callback,
    void* context) {
    furi_check(profile);
    furi_check(profile->config == ls_ble_profile);
    LsBleProfileInstance* inst = (LsBleProfileInstance*)profile;
    inst->rx_ctx = context;
    inst->rx_cb = callback;
}
