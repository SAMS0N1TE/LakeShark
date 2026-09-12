#pragma once

#include <furi_ble/profile_interface.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*LsBleProfileRxCallback)(const uint8_t* data, uint16_t len, void* context);

extern const FuriHalBleProfileTemplate* ls_ble_profile;

bool ls_ble_profile_tx(FuriHalBleProfileBase* profile, const uint8_t* data, uint16_t len);

void ls_ble_profile_set_rx_callback(
    FuriHalBleProfileBase* profile,
    LsBleProfileRxCallback callback,
    void* context);

uint16_t ls_ble_profile_max_frame(void);

#ifdef __cplusplus
}
#endif
