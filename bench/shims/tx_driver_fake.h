#ifndef LS_TX_DRIVER_FAKE_H
#define LS_TX_DRIVER_FAKE_H

#include "tx_driver_private.h"

typedef struct {
    const char *endpoint_id;
    ls_radio_range_t frequency_range;
    uint32_t modulation_mask;
    uint32_t min_bandwidth_hz;
    uint32_t max_bandwidth_hz;
    int32_t min_power_tenths_dbm;
    int32_t max_power_tenths_dbm;
    size_t max_payload_bytes;
    uint32_t max_airtime_us;
    uint32_t estimated_airtime_us;
    ls_radio_err_t estimate_error;
    ls_radio_err_t transmit_error;
    ls_radio_tx_report_t report;
    unsigned transmit_calls;
    ls_radio_tx_packet_t last_packet;
    uint8_t last_payload[LS_RADIO_TX_PAYLOAD_MAX];
} ls_tx_driver_fake_t;

void ls_tx_driver_fake_init(ls_tx_driver_fake_t *fake, const char *endpoint_id);
ls_radio_tx_err_t ls_tx_driver_fake_register(ls_tx_driver_fake_t *fake);
void ls_tx_driver_fake_unregister(ls_tx_driver_fake_t *fake);

#endif
