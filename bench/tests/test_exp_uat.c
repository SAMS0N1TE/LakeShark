/* LS_TEST_SOURCES: uat_decode.c, exp_uat.c, ls_experiments.c. */
#include "ls_test.h"
#include "uat_decode.h"
#include "ls_experiments.h"
#include "ls_lora.h"
#include <string.h>
extern const ls_experiment_t exp_uat;
void ls_shim_time_set(int64_t);
void ls_shim_time_advance(int64_t);
const char *ls_exp_hw_radio_busy(void)
{
    return NULL;
}
bool ls_exp_hw_radio_take(void)
{
    return true;
}
void ls_exp_hw_radio_give(void) {}
bool ls_exp_hw_wake(void)
{
    return false;
}
static uint32_t s_caps = LS_LORA_CAP_FSK | LS_LORA_CAP_RX_WIDE;
uint32_t ls_lora_caps(void)
{
    return s_caps;
}
static ls_fsk_cfg_t cfg;
static bool active;
static uint8_t packet[48];
static int pending;
esp_err_t ls_lora_fsk_begin(const ls_fsk_cfg_t *c)
{
    cfg = *c;
    active = true;
    return ESP_OK;
}
esp_err_t ls_lora_fsk_end(void)
{
    active = false;
    return ESP_OK;
}
int ls_lora_fsk_poll(uint8_t *b, size_t n, float *r)
{
    if (!pending) return 0;
    LS_CHECK(n >= cfg.payload_bytes);
    memcpy(b, packet, cfg.payload_bytes);
    *r = -76;
    pending = 0;
    return cfg.payload_bytes;
}
esp_err_t ls_lora_rssi_inst(float *r)
{
    *r = -80;
    return ESP_OK;
}
LS_CASE(rs_round_trips_and_every_single_symbol)
{
    for (int k = 18; k <= 34; k += 16) {
        uint8_t d[34], f[48], copy[48];
        int n = k == 18 ? 30 : 48;
        for (int i = 0; i < k; i++)
            d[i] = (uint8_t)(i * 13 + 7);
        LS_CHECK(uat_rs_encode(d, k, f));
        LS_EQ_INT(uat_rs_decode(f, n), 0);
        for (int i = 0; i < n; i++) {
            memcpy(copy, f, n);
            copy[i] ^= 0xa7;
            LS_EQ_INT(uat_rs_decode(copy, n), 1);
            LS_CHECK(!memcmp(copy, f, n));
        }
    }
}
LS_CASE(rs_corrects_to_capacity_and_rejects_overload)
{
    for (int k = 18; k <= 34; k += 16) {
        uint8_t d[34], f[48], copy[48];
        int n = k == 18 ? 30 : 48, cap = (n - k) / 2;
        for (int i = 0; i < k; i++)
            d[i] = (uint8_t)(i * 31 + 9);
        LS_CHECK(uat_rs_encode(d, k, f));
        for (int count = 1; count <= cap; count++) {
            memcpy(copy, f, n);
            for (int i = 0; i < count; i++)
                copy[i * 3] ^= (uint8_t)(i + 1);
            LS_EQ_INT(uat_rs_decode(copy, n), count);
            LS_CHECK(!memcmp(copy, f, n));
        }
        memcpy(copy, f, n);
        for (int i = 0; i < cap + 1; i++)
            copy[i * 3] ^= (uint8_t)(i + 1);
        uint8_t bad[48];
        memcpy(bad, copy, n);
        LS_EQ_INT(uat_rs_decode(copy, n), -1);
        LS_CHECK(!memcmp(bad, copy, n));
    }
}
LS_CASE(valid_address_position_and_frame_type)
{
    uint8_t d[34] = {0}, f[48];
    d[1] = 0xab;
    d[2] = 0xcd;
    d[3] = 0xef;
    uint32_t lat = (uint32_t)(43.44 * 16777216.0 / 360), lon = (uint32_t)((360 - 71.65) * 16777216.0 / 360);
    d[4] = lat >> 15;
    d[5] = lat >> 7;
    d[6] = (lat << 1) | (lon >> 23);
    d[7] = lon >> 15;
    d[8] = lon >> 7;
    d[9] = lon << 1;
    d[11] = 8;
    uat_fix_t fix;
    int corrected;
    LS_CHECK(uat_rs_encode(d, 18, f));
    LS_CHECK(uat_frame_decode(f, 30, &fix, &corrected));
    LS_EQ_UINT(fix.address, 0xabcdef);
    LS_CHECK(fix.position);
    LS_NEAR(fix.lat, 43.44, 0.00003);
    LS_NEAR(fix.lon, -71.65, 0.00003);
    d[0] = 8;
    LS_CHECK(uat_rs_encode(d, 34, f));
    f[5] ^= 0x23;
    LS_CHECK(uat_frame_decode(f, 48, &fix, &corrected));
    LS_EQ_INT(corrected, 1);
    d[0] = 0;
    LS_CHECK(uat_rs_encode(d, 34, f));
    LS_CHECK(!uat_frame_decode(f, 48, &fix, &corrected));
    LS_CHECK(!uat_rs_encode(d, 19, f));
    LS_EQ_INT(uat_rs_decode(f, 29), -1);
    LS_CHECK(!uat_frame_decode(NULL, 30, &fix, NULL));
}
LS_CASE(uat_is_marked_lr2021_only_and_refuses_a_part_that_stops_at_960_mhz)
{
    char why[80];
    LS_CHECK(exp_uat.lr2021_only);
    s_caps = LS_LORA_CAP_FSK;                   /* an SX126x */
    LS_CHECK(!exp_uat.start(why, sizeof(why)));
    LS_EQ_STR(why, LS_EXP_NEEDS_LR2021);
    s_caps = LS_LORA_CAP_FSK | LS_LORA_CAP_RX_WIDE;
}
LS_CASE(fsk_settings_polarities_and_diagnostics)
{
    char why[80], out[16][LS_EXP_LINE];
    ls_shim_time_set(0);
    LS_CHECK(exp_uat.start(why, sizeof(why)));
    LS_EQ_UINT(cfg.freq_hz, 978000000);
    LS_EQ_UINT(cfg.bitrate, 1041667);
    LS_EQ_UINT(cfg.deviation_hz, 312500);
    LS_EQ_UINT(cfg.bandwidth_hz, 2222222);
    LS_EQ_UINT(cfg.sync_word, 0xacdda4e2);
    LS_EQ_INT(cfg.sync_bits, 32);
    LS_EQ_INT(cfg.preamble_bits, 8);
    LS_CHECK(cfg.rssi_at_sync);
    LS_EQ_INT(cfg.payload_bytes, 30);
    uint8_t data[34] = {0};
    data[1] = 0x12;
    LS_CHECK(uat_rs_encode(data, 18, packet));
    pending = 1;
    exp_uat.poll();
    ls_shim_time_advance(1000000);
    exp_uat.poll();
    LS_EQ_INT(cfg.payload_bytes, 48);
    data[0] = 8;
    LS_CHECK(uat_rs_encode(data, 34, packet));
    pending = 1;
    exp_uat.poll();
    ls_shim_time_advance(1000000);
    exp_uat.poll();
    LS_EQ_UINT(cfg.sync_word, ~0xacdda4e2u);
    pending = 1;
    exp_uat.poll();
    int n = exp_uat.lines(out, 16);
    LS_CHECK(n == 12);
    LS_CHECK(strstr(out[2], "SYNC 2"));
    LS_CHECK(strstr(out[3], "SYNC 1"));
    LS_CHECK(strstr(out[6], "basic 1 long 1 bad 0"));
    exp_uat.stop();
    LS_CHECK(!active);
}
#include "uat_vectors.h"
static void unhex(const char *text, uint8_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned v = 0;
        sscanf(text + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}
LS_CASE(dump978_do282b_reception_vectors)
{
    for (unsigned i = 0; i < sizeof(UAT_VECTORS) / sizeof(UAT_VECTORS[0]); i++) {
        uint8_t f[48], expected[34], encoded[48];
        uat_fix_t fix;
        int errors;
        unhex(UAT_VECTORS[i].input, f, 48);
        bool valid = uat_frame_decode(f, 48, &fix, &errors);
        LS_CHECK_MSG(valid == (UAT_VECTORS[i].type > 0), "%s acceptance", UAT_VECTORS[i].name);
        if (valid) {
            int k = UAT_VECTORS[i].type == 1 ? 18 : 34, n = k == 18 ? 30 : 48;
            unhex(UAT_VECTORS[i].expected, expected, k);
            LS_CHECK(uat_rs_encode(expected, k, encoded));
            LS_EQ_INT(uat_rs_decode(f, n), errors);
            LS_CHECK_MSG(!memcmp(f, encoded, n), "%s corrected payload/parity", UAT_VECTORS[i].name);
            LS_EQ_UINT(fix.address,
                       ((uint32_t)expected[1] << 16) | ((uint32_t)expected[2] << 8) | expected[3]);
        }
    }
}
