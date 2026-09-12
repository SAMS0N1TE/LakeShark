#include "tx_driver_fake.h"

#include <string.h>

static ls_radio_err_t estimate(void *ctx,
    const ls_radio_tx_packet_t *packet, uint32_t *airtime_us)
{
    ls_tx_driver_fake_t *fake = (ls_tx_driver_fake_t *)ctx;
    (void)packet;
    if (fake->estimate_error != LS_RADIO_OK) return fake->estimate_error;
    *airtime_us = fake->estimated_airtime_us;
    return LS_RADIO_OK;
}

static ls_radio_err_t transmit(void *ctx,
    const ls_radio_tx_packet_t *packet, ls_radio_tx_report_t *report)
{
    ls_tx_driver_fake_t *fake = (ls_tx_driver_fake_t *)ctx;
    ++fake->transmit_calls;
    fake->last_packet = *packet;
    memcpy(fake->last_payload, packet->payload, packet->payload_bytes);
    fake->last_packet.payload = fake->last_payload;
    *report = fake->report;
    return fake->transmit_error;
}

static const ls_radio_driver_ops_t s_endpoint_ops = {0};
static const ls_radio_tx_driver_ops_t s_tx_ops = {
    .estimate_airtime = estimate,
    .transmit = transmit,
};

void ls_tx_driver_fake_init(ls_tx_driver_fake_t *fake, const char *endpoint_id)
{
    memset(fake, 0, sizeof(*fake));
    fake->endpoint_id = endpoint_id;
    fake->frequency_range.min_hz = 800000000;
    fake->frequency_range.max_hz = 930000000;
    fake->modulation_mask = LS_RADIO_TX_MOD_FSK | LS_RADIO_TX_MOD_LORA |
                            LS_RADIO_TX_MOD_OOK;
    fake->min_bandwidth_hz = 1000;
    fake->max_bandwidth_hz = 1000000;
    fake->min_power_tenths_dbm = -200;
    fake->max_power_tenths_dbm = 400;
    fake->max_payload_bytes = LS_RADIO_TX_PAYLOAD_MAX;
    fake->max_airtime_us = 200000;
    fake->estimated_airtime_us = 50000;
    fake->report.rf_started = true;
    fake->report.completed = true;
    fake->report.rf_start_us = 1000;
    fake->report.rf_end_us = 51000;
}

ls_radio_tx_err_t ls_tx_driver_fake_register(ls_tx_driver_fake_t *fake)
{
    const ls_radio_endpoint_t endpoint = {
        .endpoint_id = fake->endpoint_id,
        .name = "TX policy fake",
        .capabilities = LS_RADIO_TX_PACKET,
        .packet_formats = LS_RADIO_PACKET_FORMAT_RAW,
        .frequency_ranges = &fake->frequency_range,
        .frequency_range_count = 1,
        .ops = &s_endpoint_ops,
        .driver_ctx = fake,
    };
    if (ls_radio_endpoint_register(&endpoint) != LS_RADIO_OK)
        return LS_RADIO_TX_ERR_ENDPOINT;
    const ls_radio_tx_driver_t driver = {
        .endpoint_id = fake->endpoint_id,
        .frequency_ranges = &fake->frequency_range,
        .frequency_range_count = 1,
        .modulation_mask = fake->modulation_mask,
        .min_bandwidth_hz = fake->min_bandwidth_hz,
        .max_bandwidth_hz = fake->max_bandwidth_hz,
        .min_power_tenths_dbm = fake->min_power_tenths_dbm,
        .max_power_tenths_dbm = fake->max_power_tenths_dbm,
        .max_payload_bytes = fake->max_payload_bytes,
        .max_time_on_air_us = fake->max_airtime_us,
        .ops = &s_tx_ops,
        .driver_ctx = fake,
    };
    ls_radio_tx_err_t error = ls_radio_tx_driver_register(&driver);
    if (error != LS_RADIO_TX_OK)
        (void)ls_radio_endpoint_unregister(fake->endpoint_id);
    return error;
}

void ls_tx_driver_fake_unregister(ls_tx_driver_fake_t *fake)
{
    (void)ls_radio_tx_driver_unregister(fake->endpoint_id);
    (void)ls_radio_endpoint_unregister(fake->endpoint_id);
}
