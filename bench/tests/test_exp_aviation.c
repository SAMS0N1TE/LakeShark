/* LS_TEST_SOURCES: aviation_decode.c, exp_aviation.c, ls_experiments.c. */
#include "ls_test.h"
#include "aviation_decode.h"
#include "ls_experiments.h"
#include "ls_lora.h"
#include <string.h>
extern const ls_experiment_t exp_aviation;
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
uint32_t ls_lora_caps(void)
{
    return LS_LORA_CAP_MODES_RX;
}
static ls_ook_cfg_t cfg;
static bool active;
static uint8_t packet[28];
static int pending;
esp_err_t ls_lora_ook_begin(const ls_ook_cfg_t *c)
{
    cfg = *c;
    active = true;
    return ESP_OK;
}
esp_err_t ls_lora_ook_end(void)
{
    active = false;
    return ESP_OK;
}
int ls_lora_ook_poll(uint8_t *b, size_t n, float *r)
{
    if (!pending) return 0;
    int len = cfg.frame_bytes;
    LS_CHECK(n >= (size_t)len);
    memcpy(b, packet, len);
    *r = -76;
    pending = 0;
    return len;
}
static void pulse(uint8_t *b, int at, int w)
{
    for (int i = at; i < at + w; i++)
        b[i / 8] |= 1u << (7 - i % 8);
}
LS_CASE(ssr_a_c_s_and_control_pulses)
{
    uint8_t b[16] = {0};
    pulse(b, 0, 2);
    pulse(b, 4, 2);
    pulse(b, 16, 2);
    LS_EQ_INT(aviation_classify(b, 16, 1030), AV_A);
    memset(b, 0, 16);
    pulse(b, 0, 2);
    pulse(b, 42, 2);
    LS_EQ_INT(aviation_classify(b, 16, 1030), AV_C);
    memset(b, 0, 16);
    pulse(b, 0, 2);
    pulse(b, 4, 2);
    pulse(b, 7, 32);
    LS_EQ_INT(aviation_classify(b, 16, 1030), AV_S);
    memset(b, 0, 16);
    pulse(b, 0, 2);
    pulse(b, 4, 2);
    LS_EQ_INT(aviation_classify(b, 16, 1030), AV_UNKNOWN);
}
LS_CASE(dme_spacing_width_and_phase)
{
    for (int shift = -1; shift <= 1; shift++) {
        uint8_t b[16] = {0};
        pulse(b, 0, 7);
        pulse(b, 24 + shift, 7);
        LS_EQ_INT(aviation_classify(b, 16, 0), AV_X);
        memset(b, 0, 16);
        pulse(b, 0, 7);
        pulse(b, 72 + shift, 7);
        LS_EQ_INT(aviation_classify(b, 16, 0), AV_Y);
    }
    uint8_t b[16] = {0};
    pulse(b, 0, 2);
    pulse(b, 24, 2);
    LS_EQ_INT(aviation_classify(b, 16, 0), AV_UNKNOWN);
}
LS_CASE(ac_framing_and_noise)
{
    uint8_t b[16] = {0};
    pulse(b, 0, 1);
    pulse(b, 41, 1);
    LS_EQ_INT(aviation_classify(b, 16, 1090), AV_AC);
    LS_EQ_INT(aviation_classify(NULL, 16, 1030), AV_UNKNOWN);
    LS_EQ_INT(aviation_classify(b, 3, 1090), AV_UNKNOWN);
    memset(b, 255, 16);
    LS_EQ_INT(aviation_classify(b, 16, 1090), AV_UNKNOWN);
}
LS_CASE(modes_known_adsb_ppm_and_invalid_chips)
{
    /* Published dump1090 example DF17; CRC not required for the DF histogram. */
    const uint8_t frame[] = {0x8d, 0x40, 0x62, 0x1d, 0x58, 0xc3, 0x82,
                             0xd6, 0x90, 0xc8, 0xac, 0x28, 0x63, 0xa7};
    uint8_t b[28] = {0};
    for (int i = 0; i < 112; i++)
        pulse(b, 2 * i + !((frame[i / 8] >> (7 - i % 8)) & 1), 1);
    LS_EQ_INT(aviation_df(b, 28), 17);
    b[0] = 0;
    LS_EQ_INT(aviation_df(b, 28), -1);
    LS_EQ_INT(aviation_df(NULL, 28), -1);
}
LS_CASE(session_settings_counters_expiry_and_stop)
{
    char why[80], *args[] = {"1030"}, out[16][LS_EXP_LINE];
    LS_CHECK(exp_aviation.configure(1, args, why, sizeof(why)));
    ls_shim_time_set(0);
    LS_CHECK(exp_aviation.start(why, sizeof(why)));
    LS_EQ_UINT(cfg.freq_hz, 1030000000);
    LS_EQ_UINT(cfg.bitrate, 2000000);
    LS_EQ_UINT(cfg.pattern, 0x0006);
    LS_EQ_INT(cfg.frame_bytes, 12);
    /* Payload begins after the consumed 0110 trigger: P3 16 chips (8 us)
       after P1 is 3 consumed + 13. */
    memset(packet, 0, 28);
    pulse(packet, 13, 2);
    pending = 1;
    exp_aviation.poll();
    ls_shim_time_advance(1000000);
    exp_aviation.poll();
    int n = exp_aviation.lines(out, 16);
    LS_CHECK(n > 4);
    LS_CHECK(strstr(out[3], "1/s"));
    ls_shim_time_advance(60000000);
    exp_aviation.poll();
    exp_aviation.lines(out, 16);
    LS_CHECK(strstr(out[3], "0/s"));
    exp_aviation.stop();
    LS_CHECK(!active);
    char *dme[] = {"dme", "1163"};
    LS_CHECK(exp_aviation.configure(2, dme, why, sizeof(why)));
    LS_CHECK(exp_aviation.start(why, sizeof(why)));
    LS_EQ_UINT(cfg.freq_hz, 1163000000u);
    exp_aviation.stop();
    char *bad[] = {"dme", "nan"};
    LS_CHECK(!exp_aviation.configure(2, bad, why, sizeof(why)));
}
LS_CASE(reply_round_robin)
{
    char why[80], *a[] = {"1090"};
    LS_CHECK(exp_aviation.configure(1, a, why, sizeof(why)));
    ls_shim_time_set(0);
    LS_CHECK(exp_aviation.start(why, sizeof(why)));
    LS_EQ_INT(cfg.pattern, 0x0285);
    LS_EQ_INT(cfg.frame_bytes, 28);
    ls_shim_time_advance(5000000);
    exp_aviation.poll();
    LS_EQ_INT(cfg.pattern, 0x0006);
    LS_EQ_INT(cfg.frame_bytes, 12);
    exp_aviation.stop();
}
LS_CASE(ac_and_dme_detector_prefix_reconstruction)
{
    char why[80], out[16][LS_EXP_LINE], *reply[] = {"1090"};
    LS_CHECK(exp_aviation.configure(1, reply, why, sizeof(why)));
    ls_shim_time_set(0);
    LS_CHECK(exp_aviation.start(why, sizeof(why)));
    ls_shim_time_advance(5000000);
    exp_aviation.poll();
    memset(packet, 0, sizeof(packet));
    pulse(packet, 38, 1);      /* F2 41 chips (20.3 us) after F1: 3 consumed + 38 */
    pending = 1;
    exp_aviation.poll();
    exp_aviation.lines(out, 16);
    LS_CHECK(strstr(out[3], "1"));
    exp_aviation.stop();
    char *dme[] = {"dme", "1100"};
    LS_CHECK(exp_aviation.configure(2, dme, why, sizeof(why)));
    LS_CHECK(exp_aviation.start(why, sizeof(why)));
    LS_EQ_INT(cfg.pattern, 0x00FE);
    LS_EQ_INT(cfg.pattern_bits, 8);
    memset(packet, 0, sizeof(packet));
    pulse(packet, 17, 7);
    pending = 1;
    exp_aviation.poll();
    memset(packet, 0, sizeof(packet));
    pulse(packet, 65, 7);
    pending = 1;
    exp_aviation.poll();
    exp_aviation.lines(out, 16);
    LS_CHECK(strstr(out[3], "1"));
    LS_CHECK(strstr(out[4], "1"));
    exp_aviation.stop();
}

LS_CASE(aviation_is_marked_lr2021_only)
{
    LS_CHECK(exp_aviation.lr2021_only);
}
