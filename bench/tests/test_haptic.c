/* LS_TEST_SOURCES: ${FW}/components/lakeshark/board/ls_haptic.c */

#include "ls_test.h"
#include "ls_haptic.h"
#include "ls_board.h"
#include "ls_i2c.h"
#include "driver/i2c_master.h"

#include <math.h>
#include <string.h>

/* --------------------------------------------------------- part model --- */

#define R_SRST      0x00
#define R_SYSST     0x01
#define R_SYSST2    0x04
#define R_PLAYCFG3  0x08
#define R_PLAYCFG4  0x09
#define R_CONTCFG2  0x19
#define R_CONTCFG3  0x1A
#define R_CONTCFG5  0x1C
#define R_CONTCFG6  0x1D
#define R_CONTCFG7  0x1E
#define R_CONTCFG11 0x22
#define R_CONTRD16  0x27
#define R_CONTRD17  0x28
#define R_SYSCTRL1  0x43
#define R_SYSCTRL7  0x49
#define R_DETCFG1   0x51
#define R_DETCFG2   0x52
#define R_DET_RL    0x53
#define R_DET_VBAT  0x55
#define R_DET_LO    0x57
#define R_CHIPID    0x64

static uint8_t s_reg[0x80];
static bool    s_bus_ok;
static uint8_t s_chipid;

static uint8_t s_log[128][2];
static int     s_log_n;

static int s_go_count;
static int s_stop_count;

static void part_reset(void)
{
    memset(s_reg, 0, sizeof(s_reg));
    s_reg[R_SYSST2]   = 0x09;   /* LDO_OK set, reserved bits at their default */
    s_reg[R_SYSCTRL7] = 0x14;   /* D2S_GAIN default 4                         */
    s_bus_ok  = true;
    s_chipid  = 0x80;           /* an AW86224: not address-selectable, 9 pin  */
    s_log_n   = 0;
    s_go_count = s_stop_count = 0;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *buf,
                              size_t len, int timeout)
{
    (void)dev; (void)timeout;
    if (!s_bus_ok) return ESP_FAIL;
    if (len != 2) return ESP_ERR_INVALID_ARG;

    if (buf[0] == R_SRST && buf[1] == 0xAA) {

        const uint8_t id = s_chipid;
        part_reset();
        s_chipid = id;
        s_log[0][0] = buf[0];
        s_log[0][1] = buf[1];
        s_log_n = 1;
        return ESP_OK;
    }
    if (buf[0] == R_PLAYCFG4) {
        if (buf[1] & 0x01) s_go_count++;
        if (buf[1] & 0x02) {
            s_stop_count++;
            /* The tracking loop reports what it locked onto during the
               playback that just ended. 0x08 0xE4 is 384000/2276 = 168.7 Hz,
               a plausible answer for a 170 Hz LRA. */
            s_reg[R_CONTRD16] = 0x08;
            s_reg[R_CONTRD17] = 0xE4;
        }
    }
    if (buf[0] == R_DETCFG2) {
        if (buf[1] & 0x02) { s_reg[R_DET_VBAT] = 0xB0; s_reg[R_DET_LO] |= 0x10; }
        if (buf[1] & 0x01) { s_reg[R_DET_RL]   = 0x0A; s_reg[R_DET_LO] |= 0x01; }
    }
    if (buf[0] < sizeof(s_reg)) s_reg[buf[0]] = buf[1];
    if (s_log_n < 128) { s_log[s_log_n][0] = buf[0]; s_log[s_log_n][1] = buf[1]; s_log_n++; }
    return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev,
                                      const uint8_t *tx, size_t tx_len,
                                      uint8_t *rx, size_t rx_len, int timeout)
{
    (void)dev; (void)timeout;
    if (!s_bus_ok) return ESP_FAIL;
    if (tx_len != 1 || rx_len != 1) return ESP_ERR_INVALID_ARG;
    *rx = tx[0] == R_CHIPID ? s_chipid
        : (tx[0] < sizeof(s_reg) ? s_reg[tx[0]] : 0);
    return ESP_OK;
}

static struct { int x; } s_fake_dev;

esp_err_t ls_i2c_probe(ls_i2c_bus_id_t id, uint8_t addr, int ms)
{
    (void)id; (void)addr; (void)ms;
    return s_bus_ok ? ESP_OK : ESP_FAIL;
}

esp_err_t ls_i2c_device(ls_i2c_bus_id_t id, uint8_t addr, uint32_t hz,
                        i2c_master_dev_handle_t *out)
{
    (void)hz;
    /* The part is on the SECONDARY bus. A driver reaching for the primary
       would find the expander and the RTC instead, and 0x58 is free there -
       so this would ACK nothing and look like absent hardware. */
    LS_CHECK(id == LS_I2C_SECONDARY);
    LS_CHECK(addr == LS_BOARD_HAPTIC_I2C_ADDR);
    if (!s_bus_ok) return ESP_FAIL;
    if (out) *out = (i2c_master_dev_handle_t)&s_fake_dev;
    return ESP_OK;
}

/* Find the last value written to a register, or -1 if it was never written. */
static int last_write(uint8_t reg)
{
    for (int i = s_log_n - 1; i >= 0; i--)
        if (s_log[i][0] == reg) return s_log[i][1];
    return -1;
}

/* The bring-up runs ONCE, here, and it is scripted. */

static uint8_t s_init_log[128][2];
static int     s_init_log_n;
static uint8_t s_init_reg[0x80];

static void ensure_up(void)
{
    static bool once;
    if (!once) {
        once = true;

        /* Bit 6 set is an AW86214: address-selectable, and a different part.
           Something else answering 0x58 must not be configured as a motor
           driver. */
        part_reset();
        s_chipid = 0xC0;
        LS_CHECK(ls_haptic_start() == ESP_ERR_NOT_FOUND);
        LS_CHECK(!ls_haptic_present());

        /* A bus that will not answer at all is absent, not broken. */
        part_reset();
        s_bus_ok = false;
        LS_CHECK(ls_haptic_start() != ESP_OK);
        LS_CHECK(!ls_haptic_present());

        part_reset();
        LS_CHECK(ls_haptic_start() == ESP_OK);
        memcpy(s_init_log, s_log, sizeof(s_init_log));
        s_init_log_n = s_log_n;
        memcpy(s_init_reg, s_reg, sizeof(s_init_reg));
    }
    LS_CHECK(ls_haptic_present());
    s_bus_ok = true;
    s_log_n = 0;
    s_go_count = s_stop_count = 0;
}

static int init_write_index(uint8_t reg)
{
    for (int i = 0; i < s_init_log_n; i++)
        if (s_init_log[i][0] == reg) return i;
    return -1;
}

/* --------------------------------------------------- the arithmetic ----- */

LS_CASE(f_pre_is_the_datasheet_divisor_and_nothing_else)
{

    const int hz[] = { 150, 170, 175, 205, 235 };
    for (unsigned i = 0; i < sizeof(hz) / sizeof(hz[0]); i++) {
        const uint8_t code = ls_haptic_f_pre_code(hz[i]);
        const double back = 24000.0 / code;
        /* Round trip has to land within one code step of where it started. */
        LS_CHECK(fabs(back - hz[i]) < (24000.0 / code / code) + 0.5);
    }

    LS_EQ_INT(0x8D, ls_haptic_f_pre_code(170));
    LS_EQ_INT(0x88, ls_haptic_f_pre_code(LS_BOARD_HAPTIC_F0_HZ));
}

LS_CASE(f_pre_clamps_instead_of_wrapping)
{
    /* Eight bits: below 94 Hz the divisor no longer fits, and a wrap would
       put a 50 Hz request somewhere in the hundreds with no complaint. */
    LS_CHECK(ls_haptic_f_pre_code(1) <= 255);
    LS_CHECK(ls_haptic_f_pre_code(1) >= 1);
    LS_CHECK(ls_haptic_f_pre_code(100000) >= 1);
    LS_EQ_INT(255, ls_haptic_f_pre_code(94));
}

LS_CASE(drv_width_stays_under_the_half_cycle)
{

    for (int f0 = 120; f0 <= 300; f0++) {
        const int half = (24000 + f0 / 2) / f0;
        const uint8_t w = ls_haptic_drv_width_code(f0, 15, 8);
        LS_CHECK(w >= 1);
        LS_CHECK(w < half);
    }
}

LS_CASE(drv_width_follows_the_recommendation_where_it_fits)
{
    /* (24000/F0) - 8 - TRACK_MARGIN - BRK_GAIN. At 170 Hz that is
       141 - 8 - 15 - 8 = 110. */
    LS_EQ_INT(110, ls_haptic_drv_width_code(170, 15, 8));

    /* And it moves with its inputs - a driver that ignored TRACK_MARGIN
       would still pass the constraint case above. */
    LS_CHECK(ls_haptic_drv_width_code(170, 20, 8) <
             ls_haptic_drv_width_code(170, 15, 8));
    LS_CHECK(ls_haptic_drv_width_code(170, 15, 12) <
             ls_haptic_drv_width_code(170, 15, 8));
}

LS_CASE(level_never_reaches_the_track_enable_bit)
{
    /* CONTCFG6 keeps TRACK_EN in bit 7. A full-scale level of 128 or 255
       would clear resonance tracking as a side effect of asking for maximum
       strength - which feels like a weak motor at full power, the least
       diagnosable failure this part has. */
    for (int pct = 0; pct <= 100; pct++)
        LS_CHECK((ls_haptic_level_code(pct) & 0x80) == 0);

    LS_EQ_INT(0,   ls_haptic_level_code(0));
    LS_EQ_INT(127, ls_haptic_level_code(100));
    LS_EQ_INT(127, ls_haptic_level_code(500));
    LS_EQ_INT(0,   ls_haptic_level_code(-20));
    /* Monotonic, so a strength control cannot go backwards mid-scale. */
    for (int pct = 1; pct <= 100; pct++)
        LS_CHECK(ls_haptic_level_code(pct) >= ls_haptic_level_code(pct - 1));
}

LS_CASE(the_ten_bit_measurements_come_from_two_registers_each)
{
    /* RL = 678 * (RL*4 + RL_LO) / (1024 * D2S_GAIN), with RL_LO in bits 1:0
       of DET_LO. 0x0A and low bits 01 is code 41: 678*41/(1024*20) = 1.357. */
    const float rl = ls_haptic_rl_ohms(0x0A, 0x01, 20);
    LS_CHECK(fabsf(rl - 1.3569f) < 0.001f);

    /* VBAT_LO is bits 5:4 of the SAME register, not bits 1:0. Feeding it the
       whole byte and taking the wrong two bits is the mistake this checks
       for: 0x31 has RL_LO 01 and VBAT_LO 11, which are different numbers. */
    const float v_hi = ls_haptic_vbat_volts(0xB0, 0x31);
    const float v_lo = ls_haptic_vbat_volts(0xB0, 0x01);
    LS_CHECK(v_hi > v_lo);
    /* 0xB0*4 + 3 = 707; 6.1*707/1024 = 4.211 V. */
    LS_CHECK(fabsf(v_hi - 4.2114f) < 0.001f);

    LS_CHECK(ls_haptic_rl_ohms(0x0A, 0x01, 0) == 0.0f);
}

LS_CASE(tracked_resonance_reads_back_in_hertz)
{
    /* F0 = 384000 / (H*256 + L). */
    LS_CHECK(fabsf(ls_haptic_f0_hz(0x08, 0xE4) - 168.71f) < 0.05f);
    /* Nothing tracked yet is zero, not a division by zero and not infinity. */
    LS_EQ_INT(0, (int)ls_haptic_f0_hz(0, 0));
}

/* ------------------------------------------------------- the sequence --- */

LS_CASE(a_part_that_is_not_this_part_is_refused)
{
    /* Both refusal paths run inside the scripted bring-up above, which is the
       only place a clean part exists. Asking for a working driver is what
       drives them. */
    ensure_up();
}

LS_CASE(init_resets_the_part_before_configuring_it)
{
    ensure_up();

    /* The software reset clears every configuration register, so anything
       written before it is lost - and lost silently, because the part ACKs
       the write either way. All of it has to come after. */
    int rst = -1;
    for (int i = 0; i < s_init_log_n; i++)
        if (s_init_log[i][0] == R_SRST && s_init_log[i][1] == 0xAA) { rst = i; break; }
    LS_CHECK(rst >= 0);

    static const uint8_t after[] = { R_CONTCFG2, R_CONTCFG3, R_CONTCFG5,
                                     R_CONTCFG11, R_SYSCTRL1, R_PLAYCFG3 };
    for (unsigned i = 0; i < sizeof(after) / sizeof(after[0]); i++)
        LS_CHECK(init_write_index(after[i]) > rst);
}

LS_CASE(init_writes_the_frequency_the_board_declares)
{
    ensure_up();

    /* The board's LRA frequency has to reach the part, and DRV_WIDTH has to
       be derived from the SAME frequency. Two that disagree is the failure
       this catches: the part would drive at one resonance and brake for
       another, and the only symptom is a motor that feels weak. */
    LS_EQ_INT(ls_haptic_f_pre_code(LS_BOARD_HAPTIC_F0_HZ),
              s_init_reg[R_CONTCFG2]);
    LS_EQ_INT(ls_haptic_drv_width_code(LS_BOARD_HAPTIC_F0_HZ,
                                       s_init_reg[R_CONTCFG11],
                                       s_init_reg[R_CONTCFG5]),
              s_init_reg[R_CONTCFG3]);
}

LS_CASE(vbat_mode_is_on_so_strength_does_not_follow_the_battery)
{

    ensure_up();
    LS_CHECK((s_init_reg[R_SYSCTRL1] & 0x80) != 0);
    /* And EN_RAMINIT is left OFF after init: it is the digital clock enable
       for the measurement path, not something to leave running. */
    LS_CHECK((s_init_reg[R_SYSCTRL1] & 0x08) == 0);
}

LS_CASE(a_buzz_starts_the_drive_and_then_stops_it)
{
    ensure_up();
    LS_CHECK(ls_haptic_buzz(30, 100));
    while (ls_haptic_service(0)) { }

    LS_CHECK(s_go_count >= 1);
    LS_CHECK(s_stop_count >= 1);

    /* CONT mode with braking, not RAM mode and not "no play". */
    const int p3 = last_write(R_PLAYCFG3);
    LS_CHECK(p3 >= 0);
    LS_EQ_INT(0x02, p3 & 0x03);
    LS_CHECK((p3 & 0x04) != 0);

    /* Full strength keeps tracking on, which is the whole point of the
       level-code case above expressed against the real write. */
    const int c6 = last_write(R_CONTCFG6);
    LS_CHECK(c6 >= 0);
    LS_CHECK((c6 & 0x80) != 0);
    LS_EQ_INT(127, c6 & 0x7F);
    LS_EQ_INT(127, last_write(R_CONTCFG7));
}

LS_CASE(the_kick_is_never_quieter_than_the_sustain)
{
    /* DRV1 exists to get the LRA to amplitude before the sustain takes over.
       A DRV1 below DRV2 is a fade-in, which is the thing it is there to
       prevent - and on a 12 ms tick it is the difference between an event
       and nothing. */
    ensure_up();
    LS_CHECK(ls_haptic_buzz(20, 60));
    while (ls_haptic_service(0)) { }
    const int drv1 = last_write(R_CONTCFG6) & 0x7F;
    const int drv2 = last_write(R_CONTCFG7) & 0x7F;
    LS_CHECK(drv1 >= drv2);
}

LS_CASE(a_buzz_is_capped_so_a_stuck_request_cannot_run_the_motor_flat)
{

    LS_EQ_INT(LS_HAPTIC_MAX_MS, ls_haptic_clamp_ms(60000));
    LS_EQ_INT(LS_HAPTIC_MAX_MS, ls_haptic_clamp_ms(LS_HAPTIC_MAX_MS + 1));
    LS_EQ_INT(30, ls_haptic_clamp_ms(30));
    LS_EQ_INT(0, ls_haptic_clamp_ms(-1));
    LS_EQ_INT(0, ls_haptic_clamp_ms(0));

    for (int e = 0; e < LS_HAPTIC_EFFECT_COUNT; e++) {
        ensure_up();
        LS_CHECK(ls_haptic_play((ls_haptic_effect_t)e));
        while (ls_haptic_service(0)) { }
        LS_CHECK(s_stop_count == s_go_count);
    }
}

LS_CASE(a_zero_length_buzz_does_nothing_at_all)
{
    ensure_up();
    LS_CHECK(!ls_haptic_buzz(0, 100));
    LS_CHECK(!ls_haptic_buzz(-5, 100));
    while (ls_haptic_service(0)) { }
    LS_EQ_INT(0, s_go_count);
}

LS_CASE(every_named_effect_actually_drives_the_motor)
{
    /* An effect that queued nothing would be a silent no-op with a name -
       the same failure as the toggle this whole driver exists to fix. */
    for (int e = 0; e < LS_HAPTIC_EFFECT_COUNT; e++) {
        ensure_up();
        LS_CHECK(ls_haptic_play((ls_haptic_effect_t)e));
        while (ls_haptic_service(0)) { }
        LS_CHECK(s_go_count >= 1);
        LS_CHECK(s_stop_count == s_go_count);   /* nothing left running */
    }
}

LS_CASE(alert_is_more_than_one_pulse)
{

    ensure_up();
    LS_CHECK(ls_haptic_play(LS_HAPTIC_ALERT));
    while (ls_haptic_service(0)) { }
    LS_CHECK(s_go_count >= 3);

    ensure_up();
    LS_CHECK(ls_haptic_play(LS_HAPTIC_TICK));
    while (ls_haptic_service(0)) { }
    LS_EQ_INT(1, s_go_count);
}

LS_CASE(an_out_of_range_effect_is_refused_rather_than_indexed)
{
    ensure_up();
    LS_CHECK(!ls_haptic_play((ls_haptic_effect_t)LS_HAPTIC_EFFECT_COUNT));
    LS_CHECK(!ls_haptic_play((ls_haptic_effect_t)-1));
    while (ls_haptic_service(0)) { }
    LS_EQ_INT(0, s_go_count);
}

LS_CASE(a_full_queue_drops_rather_than_blocks)
{
    /* This is reachable from the draw path, which must not block. Five
       requests into a queue of four has to refuse the fifth and return,
       not wait for a motor. */
    ensure_up();
    int accepted = 0;
    for (int i = 0; i < 16; i++)
        if (ls_haptic_play(LS_HAPTIC_TICK)) accepted++;
    LS_CHECK(accepted > 0);
    LS_CHECK(accepted < 16);

    /* And the queue empties again, so a burst of notices costs the ones it
       dropped and nothing after them. */
    while (ls_haptic_service(0)) { }
    LS_CHECK(ls_haptic_play(LS_HAPTIC_TICK));
    while (ls_haptic_service(0)) { }
}

LS_CASE(diag_restores_the_gain_it_borrowed)
{
    ensure_up();
    const int before = s_reg[R_SYSCTRL7];

    ls_haptic_diag_t d;
    LS_CHECK(ls_haptic_diag(&d));

    /* D2S_GAIN suits the coil being measured and then goes back. Leaving it
       at the diagnostic value would change what every later measurement
       means, and the datasheet's own steps say save and restore. */
    LS_EQ_INT(before, s_reg[R_SYSCTRL7]);
    /* And the measurement path is switched back off. */
    LS_EQ_INT(0, s_reg[R_DETCFG2]);
    LS_CHECK((s_reg[R_SYSCTRL1] & 0x08) == 0);

    /* The model's coil reads 0x0A/01 at gain 20, which is 1.36 ohms - a
       number, not a zero, so the conversion ran. */
    LS_CHECK(d.lra_ohms > 0.0f);
    LS_CHECK(d.vdd_v > 3.0f && d.vdd_v < 5.5f);
}

LS_CASE(diag_reports_the_faults_the_part_latches)
{
    ensure_up();
    s_reg[R_SYSST] = 0x04 | 0x02 | 0x20;   /* OCDS, OTS, UVLS */
    ls_haptic_diag_t d;
    LS_CHECK(ls_haptic_diag(&d));
    LS_CHECK(d.over_current);
    LS_CHECK(d.over_temp);
    LS_CHECK(d.under_voltage);

    s_reg[R_SYSST] = 0x00;
    LS_CHECK(ls_haptic_diag(&d));
    LS_CHECK(!d.over_current && !d.over_temp && !d.under_voltage);
}

LS_CASE(resonance_is_read_at_the_stop_because_that_is_when_it_exists)
{
    /* CONTRD16/17 hold what the tracking loop locked onto during the
       playback that just ended, and the next playback overwrites them.
       Reading them at the console instead of at the stop gets whatever the
       last thing to run left behind. */
    ensure_up();
    ls_haptic_diag_t before;
    LS_CHECK(ls_haptic_diag(&before));

    LS_CHECK(ls_haptic_buzz(30, 100));
    while (ls_haptic_service(0)) { }

    ls_haptic_diag_t after;
    LS_CHECK(ls_haptic_diag(&after));
    LS_CHECK(after.f0_hz > 150.0f && after.f0_hz < 190.0f);
}
