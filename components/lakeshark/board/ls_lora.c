/* See ls_lora.h for why BUSY and DIO1 shape this file. */
#include "ls_lora.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ls_board.h"
#include "ls_spi.h"
#include "ls_xl9535.h"

static const char *TAG = "ls_lora";

/* ------------------------------------------------------------ ladder -- */

/* The chip's LoRa bandwidth ladder, once. */

static const struct { uint32_t hz; uint8_t code; } BW_LADDER[] = {
    {   7810, 0x00 }, {  10420, 0x08 }, {  15630, 0x01 }, {  20830, 0x09 },
    {  31250, 0x02 }, {  41670, 0x0A }, {  62500, 0x03 }, { 125000, 0x04 },
    { 250000, 0x05 }, { 500000, 0x06 },
};
#define BW_LADDER_N ((int)(sizeof(BW_LADDER) / sizeof(BW_LADDER[0])))

/* -------------------------------------------------------- sweep plan -- */

/* At 500 kHz, which is the case it was measured in and the case it
   stays the figure for. The plan below adds to it as the filter narrows. */
#define SCAN_SETTLE_US   300
#define SCAN_BW_WIDEST   500000u
#define SCAN_SETTLE_TAUS 8

/* The filter follows the bins, and the wait follows the filter. */

static uint32_t scan_settle_us(uint32_t bw_hz)
{
    if (bw_hz == 0 || bw_hz > SCAN_BW_WIDEST) bw_hz = SCAN_BW_WIDEST;
    const uint32_t taus = (SCAN_SETTLE_TAUS * 1000000u + bw_hz - 1) / bw_hz;
    const uint32_t paid = (SCAN_SETTLE_TAUS * 1000000u + SCAN_BW_WIDEST - 1)
                        / SCAN_BW_WIDEST;
    return SCAN_SETTLE_US + (taus > paid ? taus - paid : 0);
}

void ls_lora_scan_plan(uint32_t min_hz, uint32_t max_hz, int n,
                       ls_lora_scan_plan_t *out)
{
    if (!out) return;
    out->bw_hz = SCAN_BW_WIDEST;
    out->looks = 1;
    /* A single bin or an empty band has no spacing to plan from, and gets
       what every sweep got before: the widest filter and one look. */
    if (n > 1 && max_hz > min_hz) {
        const uint64_t span = (uint64_t)(max_hz - min_hz);
        const uint64_t gaps = (uint64_t)(n - 1);

        for (int r = 0; r < BW_LADDER_N; r++) {
            if ((uint64_t)BW_LADDER[r].hz * gaps >= span) {
                out->bw_hz = BW_LADDER[r].hz;
                break;
            }
        }
        const uint64_t covered = (uint64_t)out->bw_hz * gaps;
        if (covered < span) {
            uint64_t looks = (span + covered - 1) / covered;
            if (looks > LS_LORA_SCAN_LOOKS_MAX) looks = LS_LORA_SCAN_LOOKS_MAX;
            out->looks = (int)looks;
        }
    }
    out->settle_us = scan_settle_us(out->bw_hz);
}

uint32_t ls_lora_scan_look_hz(uint32_t min_hz, uint32_t max_hz, int n, int i,
                              int looks, int k)
{
    if (looks <= 1 || n <= 1 || max_hz <= min_hz)
        return ls_lora_scan_bin_hz(min_hz, max_hz, n, i);
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    if (k < 0) k = 0;
    if (k >= looks) k = looks - 1;
    /* Bin i owns the slice from half a spacing below its centre to half a spacing above, and look k sits in the middle of the k-th of `looks` equal parts of it: */

    const int64_t span = (int64_t)(max_hz - min_hz);
    const int64_t num  = 2LL * looks * i - looks + 2LL * k + 1;
    const int64_t den  = 2LL * looks * (n - 1);
    int64_t off = (span * num) / den;
    if (off < 0)    off = 0;
    if (off > span) off = span;
    return min_hz + (uint32_t)off;
}

#if defined(LS_BOARD_LORA_CS_GPIO) && defined(LS_BOARD_LORA_BUSY_GPIO) && \
    defined(LS_BOARD_XL_RADIO_RST)

#define CS_PIN    ((gpio_num_t)LS_BOARD_LORA_CS_GPIO)
#define BUSY_PIN  ((gpio_num_t)LS_BOARD_LORA_BUSY_GPIO)

/* SX1262 opcodes, datasheet table 11-1. */
#define OP_GET_STATUS       0xC0
#define OP_SET_STANDBY      0x80
#define OP_READ_REGISTER    0x1D
#define OP_WRITE_REGISTER   0x0D
#define OP_WRITE_BUFFER     0x0E
#define OP_READ_BUFFER      0x1E
#define OP_SET_TX           0x83
#define OP_SET_RX           0x82
#define OP_SET_RF_FREQ      0x86
#define OP_SET_PKT_TYPE     0x8A
#define OP_SET_TX_PARAMS    0x8E
#define OP_SET_PA_CFG       0x95
#define OP_SET_BUF_BASE     0x8F
#define OP_SET_MOD_PARAMS   0x8B
#define OP_SET_PKT_PARAMS   0x8C
#define OP_SET_DIO_IRQ      0x08
#define OP_GET_IRQ_STATUS   0x12
#define OP_CLR_IRQ_STATUS   0x02
#define OP_SET_DIO2_RFSW    0x9D
#define OP_SET_DIO3_TCXO    0x97
#define OP_CAL              0x89
#define OP_CAL_IMAGE        0x98
#define OP_SET_REGULATOR    0x96
#define OP_SET_FALLBACK     0x93
#define OP_GET_RX_BUF_STAT  0x13
#define OP_GET_PKT_STATUS   0x14
#define OP_GET_RSSI_INST    0x15

#define STANDBY_RC       0x00
/* Standby with the crystal LEFT RUNNING. */

#define STANDBY_XOSC     0x01
#define PKT_TYPE_LORA    0x01

/* IRQ bits, datasheet table 13-29. */
#define IRQ_TX_DONE      (1u << 0)
#define IRQ_RX_DONE      (1u << 1)
#define IRQ_HEADER_ERR   (1u << 5)
#define IRQ_CRC_ERR      (1u << 6)
#define IRQ_TIMEOUT      (1u << 9)

/* Registers touched by hand. */
#define REG_LORA_SYNCWORD   0x0740
#define REG_OCP             0x08E7
#define REG_RX_GAIN         0x08AC
#define REG_TX_CLAMP        0x08D8

/* The one transmit in a thousand that never raises TX_DONE must not wedge the
   sender for good. The longest legal frame here is well under 5 s. */
#define TX_TIMEOUT_MS    8000

static spi_device_handle_t s_dev;
static bool s_present;

/* SPI wants DMA-capable memory for a transfer this size and DMA cannot reach
   PSRAM, so this is internal RAM and there is no choice about that. 259 bytes
   carries opcode + offset + a full 255-byte payload in one transaction;
   splitting it would save RAM and put a BUSY wait in the middle of a buffer
   write, which is the more expensive mistake. One buffer, not one per
   direction: the driver is single-threaded by contract. */
static uint8_t *s_pkt;

/* The part holds BUSY high while it works and ignores anything clocked in
   meanwhile. 1 ms is generous for every command used here; the datasheet's
   worst case is the crystal start after a cold reset, which is handled
   separately below with a longer budget. */
/* Spin briefly, then yield. */

static bool wait_not_busy(int timeout_ms)
{
    for (int i = 0; i < 256; i++) {
        if (!gpio_get_level(BUSY_PIN)) return true;
        esp_rom_delay_us(1);
    }
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (gpio_get_level(BUSY_PIN)) {
        if (esp_timer_get_time() > deadline) return false;
        vTaskDelay(1);
    }
    return true;
}

/* One command: opcode, then any parameters, then any reply. The SX1262 is
   full duplex and returns a status byte during the opcode, but nothing here
   needs it, so the transaction is kept simple deliberately. */
static esp_err_t cmd(uint8_t opcode, const uint8_t *tx, size_t tx_len,
                     uint8_t *rx, size_t rx_len)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    if (!wait_not_busy(20)) {
        ESP_LOGW(TAG, "busy stuck high before 0x%02X", opcode);
        return ESP_ERR_TIMEOUT;
    }

    uint8_t buf[16];
    size_t n = 1 + tx_len + rx_len;
    if (n > sizeof(buf)) return ESP_ERR_INVALID_SIZE;
    memset(buf, 0, n);
    buf[0] = opcode;
    if (tx && tx_len) memcpy(&buf[1], tx, tx_len);

    spi_transaction_t t = {
        .length = n * 8,
        .tx_buffer = buf,
        .rx_buffer = buf,
    };
    esp_err_t err = spi_device_transmit(s_dev, &t);
    if (err == ESP_OK && rx && rx_len) memcpy(rx, &buf[1 + tx_len], rx_len);
    return err;
}

esp_err_t ls_lora_read_reg(uint16_t addr, uint8_t *buf, size_t len)
{
    /* ReadRegister is address high, address low, then one status byte the
       part inserts before the data. That NOP is easy to forget and shifts
       every byte by one, which reads as plausible-but-wrong data. */
    uint8_t tx[3] = { (uint8_t)(addr >> 8), (uint8_t)addr, 0x00 };
    return cmd(OP_READ_REGISTER, tx, sizeof(tx), buf, len);
}

esp_err_t ls_lora_status(uint8_t *out)
{
    uint8_t st = 0;
    esp_err_t err = cmd(OP_GET_STATUS, NULL, 0, &st, 1);
    if (err == ESP_OK && out) *out = st;
    return err;
}

esp_err_t ls_lora_start(void)
{
    if (s_present) return ESP_OK;

    esp_err_t err = ls_spi_bus(LS_SPI_RADIO);
    if (err != ESP_OK) return err;

    gpio_config_t busy = {
        .pin_bit_mask = 1ULL << BUSY_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&busy);

    /* Hardware chip select is right for this part: unlike the CC1101 it has
       no handshake that needs CS held across a wait. 2 MHz is well inside
       the 16 MHz limit and leaves the wiring no excuse. */
    /* Register once. ls_lora_stop() clears s_present so the next start
       re-runs the reset and the liveness check, and without this guard it
       would also register a second device every time. The IDF allows three
       per host and four parts share this one, so a stop/start cycle would
       quietly consume the budget the other radios need. */
    if (!s_dev) {
        err = ls_spi_device(LS_SPI_RADIO, LS_BOARD_LORA_CS_GPIO, 0,
                            2 * 1000 * 1000, 1, &s_dev);
        if (err != ESP_OK) return err;
    }

    /* ls_board_hw parks the radio in reset at boot, deliberately. Releasing
       it is this driver's job and nobody else's, so a board with no LoRa
       consumer leaves the part quiet. */
    /* DIO1 is only ever read, so make it an input once here rather
       than hoping it defaulted that way. The XL9535 powers up with every pin
       an input, but ls_board_hw touches this expander and a later change
       there should not silently turn the radio's interrupt line into an
       output. */
    ls_xl9535_set_dir(LS_BOARD_XL_RADIO_DIO1, false);

    ls_xl9535_out(LS_BOARD_XL_RADIO_RST, false);
    vTaskDelay(pdMS_TO_TICKS(2));
    ls_xl9535_out(LS_BOARD_XL_RADIO_RST, true);

    /* Cold start runs the 32 MHz crystal up; the datasheet allows several
       milliseconds and BUSY stays high throughout. */
    if (!wait_not_busy(100)) {
        ESP_LOGE(TAG, "busy never fell after reset - part unpowered or absent");
        return ESP_ERR_TIMEOUT;
    }

    uint8_t standby = STANDBY_RC;
    cmd(OP_SET_STANDBY, &standby, 1, NULL, 0);

    uint8_t st = 0;
    err = ls_lora_status(&st);
    if (err != ESP_OK) return err;
    if (st == 0x00 || st == 0xFF) {
        /* A floating MISO reads as one of these depending on the pull, and
           both mean nothing is driving the bus. Reporting "not found" is more
           useful than reporting a status of zero as though it were real. */
        ESP_LOGE(TAG, "status 0x%02X - nothing is driving MISO", st);
        return ESP_ERR_NOT_FOUND;
    }

    if (!s_pkt) {
        s_pkt = heap_caps_malloc(259, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_pkt) {
            ESP_LOGE(TAG, "no DMA memory for the packet buffer");
            return ESP_ERR_NO_MEM;
        }
    }

    s_present = true;
    ESP_LOGI(TAG, "SX1262 up: status 0x%02X (mode %u, cmd %u)",
             st, (unsigned)((st >> 4) & 7), (unsigned)((st >> 1) & 7));
    return ESP_OK;
}

bool ls_lora_present(void) { return s_present; }

void ls_lora_stop(void)
{
    /* The SPI device is left registered on purpose: the bus is shared and
       tearing it down would disturb the other parts on it. Forgetting that
       the part answered is the whole job, so the next start re-runs the
       reset and the liveness check. */
    s_present = false;
}

static ls_lora_cfg_t s_cfg;
static bool     s_cfg_valid;
static bool     s_scanning;        /* a sweep owns the synthesiser */
static bool     s_rx_mode;
static bool     s_tx_busy;
static int64_t  s_tx_deadline;

static esp_err_t xfer(const uint8_t *tx, uint8_t *rx, size_t n)
{
    if (!s_dev || !s_pkt) return ESP_ERR_INVALID_STATE;
    if (n > 259) return ESP_ERR_INVALID_SIZE;
    if (!wait_not_busy(20)) return ESP_ERR_TIMEOUT;
    memcpy(s_pkt, tx, n);
    spi_transaction_t t = { .length = n * 8, .tx_buffer = s_pkt, .rx_buffer = s_pkt };
    esp_err_t err = spi_device_transmit(s_dev, &t);
    if (err == ESP_OK && rx) memcpy(rx, s_pkt, n);
    return err;
}

static esp_err_t write_reg(uint16_t addr, uint8_t value)
{
    uint8_t tx[4] = { OP_WRITE_REGISTER, (uint8_t)(addr >> 8), (uint8_t)addr, value };
    return xfer(tx, NULL, sizeof(tx));
}

/* The chip's bandwidth ladder (BW_LADDER, above). A value between two rungs
   snaps DOWN, so a caller never silently gets a wider channel than it asked
   for - too wide is a receiver that hears its neighbours, which is far
   harder to diagnose than too narrow. */
static uint8_t bw_code(uint32_t hz)
{
    uint8_t code = BW_LADDER[0].code;
    for (int i = 0; i < BW_LADDER_N; i++)
        if (hz >= BW_LADDER[i].hz) code = BW_LADDER[i].code;
    return code;
}

static uint32_t bw_hz_of(uint8_t code)
{
    for (int i = 0; i < BW_LADDER_N; i++)
        if (BW_LADDER[i].code == code) return BW_LADDER[i].hz;
    return 500000;
}

void ls_lora_cfg_default(ls_lora_cfg_t *out)
{
    if (!out) return;
    /* US915. The image calibration band is 902-928 because that is what the
       vendor driver calibrates for this board; calibrating for the wrong band
       costs several dB of sensitivity and nothing announces it. */
    out->freq_hz     = 910525000u;
    /* SF7, which is what the stock preset actually uses. */

    out->sf          = 7;
    out->bw_hz       = 62500;
    out->cr          = 5;
    out->power_dbm   = 22;
    /* 32 symbols at SF8, not 8. */

    out->preamble    = (out->sf <= 8) ? 32 : 16;
    out->sync_word   = 0x12;
    out->crc_on      = true;
    out->invert_iq   = false;
    out->cal_min_mhz = 902;
    out->cal_max_mhz = 928;
}

const ls_lora_cfg_t *ls_lora_cfg(void) { return s_cfg_valid ? &s_cfg : NULL; }

static esp_err_t set_packet_params(uint8_t payload_len)
{
    uint8_t tx[7] = {
        OP_SET_PKT_PARAMS,
        (uint8_t)(s_cfg.preamble >> 8), (uint8_t)s_cfg.preamble,
        0x00,                                   /* explicit, variable length */
        payload_len,
        (uint8_t)(s_cfg.crc_on ? 1 : 0),
        (uint8_t)(s_cfg.invert_iq ? 1 : 0),
    };
    return xfer(tx, NULL, sizeof(tx));
}

esp_err_t ls_lora_configure(const ls_lora_cfg_t *cfg)
{
    if (!s_present) { esp_err_t e = ls_lora_start(); if (e != ESP_OK) return e; }
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (cfg->sf < 5 || cfg->sf > 12) return ESP_ERR_INVALID_ARG;
    if (cfg->cr < 5 || cfg->cr > 8)  return ESP_ERR_INVALID_ARG;
    if (cfg->power_dbm < -9 || cfg->power_dbm > 22) return ESP_ERR_INVALID_ARG;
    if (cfg->cal_min_mhz >= cfg->cal_max_mhz) return ESP_ERR_INVALID_ARG;

    s_cfg = *cfg;
    s_cfg.bw_hz = bw_hz_of(bw_code(cfg->bw_hz));   /* report what was set */
    s_cfg_valid = false;
    s_rx_mode = false;
    s_tx_busy = false;

    uint8_t b;
    esp_err_t err;

    b = STANDBY_RC;
    if ((err = cmd(OP_SET_STANDBY, &b, 1, NULL, 0)) != ESP_OK) return err;

    /* DCDC. The board has the inductor fitted; LDO would work and burns
       roughly twice the receive current. */
    b = 0x01;
    if ((err = cmd(OP_SET_REGULATOR, &b, 1, NULL, 0)) != ESP_OK) return err;

    /* DIO2 as an RF switch control: high in transmit, low in receive.

       That is TX versus RX and nothing else. It does NOT drive the SKY13453,
       which selects internal antenna against external MMCX1 and is on
       expander IO1 - see ls_lora.h, which said the opposite until the vendor
       README was read properly. The two switches are independent and both
       have to be right before this board transmits. */
    b = 0x01;
    if ((err = cmd(OP_SET_DIO2_RFSW, &b, 1, NULL, 0)) != ESP_OK) return err;

    /* TCXO on DIO3 at 1.6 V, 320 RTC steps of 15.625 us = 5 ms. This must
       come before Calibrate: calibrating against a crystal that has not
       started produces a radio that configures without error and hears
       nothing, which is a miserable thing to debug from the far end. */
    {
        uint8_t tx[5] = { OP_SET_DIO3_TCXO, 0x01, 0x00, 0x01, 0x40 };
        if ((err = xfer(tx, NULL, sizeof(tx))) != ESP_OK) return err;
    }

    /* Re-run the full calibration now the reference is real. */
    b = 0x7F;
    if ((err = cmd(OP_CAL, &b, 1, NULL, 0)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(5));
    if (!wait_not_busy(100)) return ESP_ERR_TIMEOUT;

    {   /* Image calibration for the band actually in use. */
        uint8_t tx[3] = { OP_CAL_IMAGE,
                          (uint8_t)(s_cfg.cal_min_mhz / 4),
                          (uint8_t)(s_cfg.cal_max_mhz / 4) };
        if ((err = xfer(tx, NULL, sizeof(tx))) != ESP_OK) return err;
    }

    b = PKT_TYPE_LORA;
    if ((err = cmd(OP_SET_PKT_TYPE, &b, 1, NULL, 0)) != ESP_OK) return err;

    {   /* RF frequency in steps of Fxtal / 2^25, Fxtal = 32 MHz. */
        uint64_t steps = ((uint64_t)s_cfg.freq_hz << 25) / 32000000ull;
        uint8_t tx[5] = { OP_SET_RF_FREQ, (uint8_t)(steps >> 24),
                          (uint8_t)(steps >> 16), (uint8_t)(steps >> 8),
                          (uint8_t)steps };
        if ((err = xfer(tx, NULL, sizeof(tx))) != ESP_OK) return err;
    }

    {   /* PA for the SX1262 high-power path. These four are a matched set;
           mixing them with SX1261 values can damage the PA. */
        uint8_t tx[5] = { OP_SET_PA_CFG, 0x04, 0x07, 0x00, 0x01 };
        if ((err = xfer(tx, NULL, sizeof(tx))) != ESP_OK) return err;
    }

    {   /* TX clamp workaround, datasheet 15.2: set bits 1..4 of 0x08D8. */
        uint8_t v = 0;
        ls_lora_read_reg(REG_TX_CLAMP, &v, 1);
        write_reg(REG_TX_CLAMP, (uint8_t)(v | 0x1E));
    }

    {   /* Power and ramp. 40 us is the vendor's ramp for this board. */
        uint8_t tx[3] = { OP_SET_TX_PARAMS, (uint8_t)s_cfg.power_dbm, 0x02 };
        if ((err = xfer(tx, NULL, sizeof(tx))) != ESP_OK) return err;
    }

    write_reg(REG_OCP, 0x38);       /* 140 mA, the SX1262 high-power figure */
    write_reg(REG_RX_GAIN, 0x96);   /* boosted receive gain */

    {   /* Modulation. LDRO goes on when a symbol runs past ~16 ms, which is
           the datasheet's condition; without it the receiver loses lock on
           the slow spreading factors as the crystal drifts. */
        uint32_t bw = s_cfg.bw_hz;
        uint32_t sym_ms = bw ? ((1u << s_cfg.sf) * 1000u) / bw : 0;
        uint8_t ldro = sym_ms >= 16 ? 1 : 0;
        uint8_t tx[5] = { OP_SET_MOD_PARAMS, s_cfg.sf, bw_code(bw),
                          (uint8_t)(s_cfg.cr - 4), ldro };
        if ((err = xfer(tx, NULL, sizeof(tx))) != ESP_OK) return err;
    }

    if ((err = set_packet_params(255)) != ESP_OK) return err;

    {   /* The sync word lives in a register, not a command. 0x12 private
           becomes 0x1424 - the low nibbles are fixed at 4. */
        write_reg(REG_LORA_SYNCWORD,     (uint8_t)((s_cfg.sync_word & 0xF0) | 0x04));
        write_reg(REG_LORA_SYNCWORD + 1, (uint8_t)(((s_cfg.sync_word & 0x0F) << 4) | 0x04));
    }

    {   /* Buffer bases: transmit at 0, receive at 0. */
        uint8_t tx[3] = { OP_SET_BUF_BASE, 0x00, 0x00 };
        if ((err = xfer(tx, NULL, sizeof(tx))) != ESP_OK) return err;
    }

    {   /* Everything worth knowing on the IRQ status word. DIO1 carries the
           same mask because it is the only line brought out on this board. */
        uint16_t mask = IRQ_TX_DONE | IRQ_RX_DONE | IRQ_CRC_ERR |
                        IRQ_HEADER_ERR | IRQ_TIMEOUT;
        uint8_t tx[9] = { OP_SET_DIO_IRQ,
                          (uint8_t)(mask >> 8), (uint8_t)mask,
                          (uint8_t)(mask >> 8), (uint8_t)mask,
                          0, 0, 0, 0 };
        if ((err = xfer(tx, NULL, sizeof(tx))) != ESP_OK) return err;
    }

    {

        uint8_t fb = 0x20;
        if ((err = cmd(OP_SET_FALLBACK, &fb, 1, NULL, 0)) != ESP_OK) return err;
    }

    s_cfg_valid = true;
    ESP_LOGI(TAG, "configured %.4f MHz SF%u BW%lu CR4/%u %+d dBm sync 0x%02X",
             s_cfg.freq_hz / 1e6, (unsigned)s_cfg.sf,
             (unsigned long)s_cfg.bw_hz, (unsigned)s_cfg.cr,
             (int)s_cfg.power_dbm, (unsigned)s_cfg.sync_word);
    return ESP_OK;
}

static esp_err_t clear_irq(uint16_t mask)
{
    uint8_t tx[3] = { OP_CLR_IRQ_STATUS, (uint8_t)(mask >> 8), (uint8_t)mask };
    return xfer(tx, NULL, sizeof(tx));
}

static esp_err_t get_irq(uint16_t *out)
{
    uint8_t rx[4] = { OP_GET_IRQ_STATUS, 0, 0, 0 };
    esp_err_t err = xfer(rx, rx, sizeof(rx));
    if (err == ESP_OK && out) *out = (uint16_t)((rx[2] << 8) | rx[3]);
    return err;
}

esp_err_t ls_lora_receive(void)
{
    if (!s_cfg_valid) return ESP_ERR_INVALID_STATE;
    esp_err_t err = clear_irq(0xFFFF);
    if (err != ESP_OK) return err;

    uint8_t tx[4] = { OP_SET_RX, 0xFF, 0xFF, 0xFF };
    err = xfer(tx, NULL, sizeof(tx));
    if (err == ESP_OK) { s_rx_mode = true; s_tx_busy = false; }
    return err;
}

bool ls_lora_is_receiving(void) { return s_rx_mode; }

esp_err_t ls_lora_send(const uint8_t *data, size_t len)
{
    if (!s_cfg_valid) return ESP_ERR_INVALID_STATE;
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    if (len > 255) return ESP_ERR_INVALID_SIZE;
    if (s_tx_busy)  return ESP_ERR_INVALID_STATE;

    if (s_scanning) return ESP_ERR_INVALID_STATE;
    if (!s_pkt)     return ESP_ERR_INVALID_STATE;

    esp_err_t err;
    uint8_t b = STANDBY_RC;
    if ((err = cmd(OP_SET_STANDBY, &b, 1, NULL, 0)) != ESP_OK) return err;
    if ((err = clear_irq(0xFFFF)) != ESP_OK) return err;
    if ((err = set_packet_params((uint8_t)len)) != ESP_OK) return err;

    /* WriteBuffer: opcode, offset, then the payload. */
    if (!wait_not_busy(20)) return ESP_ERR_TIMEOUT;
    s_pkt[0] = OP_WRITE_BUFFER;
    s_pkt[1] = 0x00;
    memcpy(&s_pkt[2], data, len);
    spi_transaction_t t = { .length = (2 + len) * 8, .tx_buffer = s_pkt, .rx_buffer = NULL };
    if ((err = spi_device_transmit(s_dev, &t)) != ESP_OK) return err;

    /* A SetTx timeout of 0 means the chip imposes none; the transmit ends
       when it ends. The deadline below is ours, not the chip's. */
    uint8_t tx[4] = { OP_SET_TX, 0x00, 0x00, 0x00 };
    if ((err = xfer(tx, NULL, sizeof(tx))) != ESP_OK) return err;

    s_tx_busy = true;
    s_rx_mode = false;
    s_tx_deadline = esp_timer_get_time() + (int64_t)TX_TIMEOUT_MS * 1000;
    ESP_LOGI(TAG, "TX begin: %u bytes at %d dBm, estimated %lu ms",
             (unsigned)len, s_cfg.power_dbm,
             (unsigned long)ls_lora_airtime_ms((int)len));
    return ESP_OK;
}

bool ls_lora_send_done(void)
{
    if (!s_tx_busy) return true;
    uint16_t irq = 0;
    if (get_irq(&irq) == ESP_OK && (irq & (IRQ_TX_DONE | IRQ_TIMEOUT))) {
        clear_irq(0xFFFF);
        s_tx_busy = false;
        ESP_LOGI(TAG, "TX finished: irq=0x%04x", irq);
        return true;
    }
    if (esp_timer_get_time() > s_tx_deadline) {

        ESP_LOGW(TAG, "transmit did not complete in %d ms", TX_TIMEOUT_MS);
        clear_irq(0xFFFF);
        s_tx_busy = false;
        return true;
    }
    return false;
}

int ls_lora_poll(uint8_t *buf, size_t max, float *rssi_dbm, float *snr_db)
{
    if (!s_cfg_valid || !buf || !max || !s_pkt) return 0;

    /* DIO1 first: one expander read is much cheaper than an SPI round trip
       and this runs in a loop. Low means there is nothing to collect. */
bool dio1 = false;
    if (ls_xl9535_get(LS_BOARD_XL_RADIO_DIO1, &dio1) != ESP_OK || !dio1) return 0;

    uint16_t irq = 0;
    if (get_irq(&irq) != ESP_OK) return 0;
    if (!irq) return 0;
    clear_irq(0xFFFF);

    if (irq & (IRQ_CRC_ERR | IRQ_HEADER_ERR)) { ls_lora_receive(); return -1; }
    if (irq & IRQ_TX_DONE) s_tx_busy = false;
    if (!(irq & IRQ_RX_DONE)) return 0;

    uint8_t st[4] = { OP_GET_RX_BUF_STAT, 0, 0, 0 };
    if (xfer(st, st, sizeof(st)) != ESP_OK) { ls_lora_receive(); return 0; }
    uint8_t len = st[2], off = st[3];
    if (len == 0) { ls_lora_receive(); return 0; }
    if (len > max) len = (uint8_t)max;

    /* ReadBuffer is opcode, offset, one NOP the part uses to turn the bus
       around, then the data. Forgetting that NOP shifts every byte by one -
       the same trap ls_lora_read_reg already documents. */
    if (!wait_not_busy(20)) { ls_lora_receive(); return 0; }
    memset(s_pkt, 0, (size_t)3 + len);
    s_pkt[0] = OP_READ_BUFFER;
    s_pkt[1] = off;
    spi_transaction_t t = { .length = (3 + len) * 8, .tx_buffer = s_pkt, .rx_buffer = s_pkt };
    if (spi_device_transmit(s_dev, &t) != ESP_OK) { ls_lora_receive(); return 0; }
    memcpy(buf, &s_pkt[3], len);

    uint8_t ps[4] = { OP_GET_PKT_STATUS, 0, 0, 0 };
    if (xfer(ps, ps, sizeof(ps)) == ESP_OK) {
        if (rssi_dbm) *rssi_dbm = -((float)ps[2]) / 2.0f;
        if (snr_db)   *snr_db   = ((float)(int8_t)ps[3]) / 4.0f;
    }

    ls_lora_receive();
    return len;
}

/* See ls_lora.h. Both pure, both used once per bin. */
uint32_t ls_lora_freq_steps(uint32_t hz)
{
    return (uint32_t)(((uint64_t)hz << 25) / 32000000ull);
}

uint32_t ls_lora_scan_bin_hz(uint32_t min_hz, uint32_t max_hz, int n, int i)
{
    if (n <= 0) return min_hz;
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    if (n == 1 || max_hz <= min_hz) return min_hz;

    const uint64_t span = (uint64_t)(max_hz - min_hz);
    return min_hz + (uint32_t)((span * (uint64_t)i) / (uint64_t)(n - 1));
}

/* --------------------------------------------------------------- sweep -- */

static ls_lora_cfg_t s_scan_saved;
static bool          s_scan_saved_valid;
static uint32_t      s_scan_min_hz, s_scan_max_hz;
/* The plan in force, the bin count it was made for, and which of its
   looks the next pass takes. SCAN_SETTLE_US and its note moved up
   beside the plan, which is the only thing that reads it now. */
static ls_lora_scan_plan_t s_scan_plan;
static int           s_scan_plan_n;
static int           s_scan_look;

bool ls_lora_scanning(void) { return s_scanning; }

/* Configure the part for the band in s_scan_min_hz..s_scan_max_hz,
   planned for `n` bins. Built on the configuration the sweep saved rather
   than on s_cfg, because once a sweep is running s_cfg is the previous
   band's scan configuration and not the mesh's. */
static esp_err_t scan_configure(int n)
{
    ls_lora_scan_plan(s_scan_min_hz, s_scan_max_hz, n, &s_scan_plan);
    s_scan_plan_n = n;
    s_scan_look = 0;

    ls_lora_cfg_t sc = s_scan_saved;
    sc.freq_hz     = s_scan_min_hz;
    sc.bw_hz       = s_scan_plan.bw_hz;
    sc.cal_min_mhz = (uint16_t)(s_scan_min_hz / 1000000u);
    sc.cal_max_mhz = (uint16_t)((s_scan_max_hz + 999999u) / 1000000u);

    sc.cal_min_mhz = (uint16_t)((sc.cal_min_mhz / 4) * 4);
    sc.cal_max_mhz = (uint16_t)(((sc.cal_max_mhz + 3) / 4) * 4);

    esp_err_t err = ls_lora_configure(&sc);
    if (err != ESP_OK) return err;
    return ls_lora_receive();
}

esp_err_t ls_lora_scan_begin(uint32_t min_hz, uint32_t max_hz)
{
    /* The same band again is nothing to do: the waterfall asks once
       a frame. A different band while sweeping is retuned below, in place. */
    if (s_scanning && min_hz == s_scan_min_hz && max_hz == s_scan_max_hz)
        return ESP_OK;
    if (!s_cfg_valid) return ESP_ERR_INVALID_STATE;
    /* Never on top of a transmission. The part would be retuned mid-burst
       and the burst would finish somewhere it was never meant to be. */
    if (s_tx_busy) return ESP_ERR_INVALID_STATE;
    if (max_hz <= min_hz) return ESP_ERR_INVALID_ARG;
    if (min_hz < 150000000u || max_hz > 960000000u) return ESP_ERR_INVALID_ARG;

    /* A band change is a retune, not an end and a beginning. */

    const bool retune = s_scanning;
    if (!retune) {
        s_scan_saved = s_cfg;
        s_scan_saved_valid = true;
    }
    s_scan_min_hz = min_hz;
    s_scan_max_hz = max_hz;

    esp_err_t err = scan_configure(LS_LORA_SCAN_BINS);
    if (err != ESP_OK) {
        /* A first begin that fails never had the radio. A retune that fails
           did, and gives it back exactly as ending the sweep would. */
        if (retune) (void)ls_lora_scan_end();
        else        s_scan_saved_valid = false;
        return err;
    }
    s_scanning = true;
    return ESP_OK;
}

static esp_err_t scan_tune(uint32_t hz)
{
    const uint32_t steps = ls_lora_freq_steps(hz);
    uint8_t tx[5] = { OP_SET_RF_FREQ, (uint8_t)(steps >> 24),
                      (uint8_t)(steps >> 16), (uint8_t)(steps >> 8),
                      (uint8_t)steps };
    return xfer(tx, NULL, sizeof(tx));
}

static ls_lora_scan_prof_t s_prof;

void ls_lora_scan_profile(ls_lora_scan_prof_t *out)
{
    if (out) *out = s_prof;
}

int ls_lora_scan_pass(float *dbm, int n, bool *row_done)
{
    if (row_done) *row_done = false;
    if (!s_scanning || !dbm || n <= 0) return 0;
    /* A bin count the plan was not made for is planned again, once:
       the filter depends on the spacing and the spacing on the count. */
    if (n != s_scan_plan_n && scan_configure(n) != ESP_OK) return 0;

    const int looks = s_scan_plan.looks > 0 ? s_scan_plan.looks : 1;
    const int k = (s_scan_look >= 0 && s_scan_look < looks) ? s_scan_look : 0;

    uint32_t t_standby = 0, t_tune = 0, t_rx = 0, t_settle = 0, t_rssi = 0;
    int64_t mark;

    int got = 0;
    for (int i = 0; i < n; i++) {
        const uint32_t hz = ls_lora_scan_look_hz(s_scan_min_hz, s_scan_max_hz,
                                                 n, i, looks, k);

        uint8_t b = STANDBY_XOSC;
        mark = esp_timer_get_time();
        if (cmd(OP_SET_STANDBY, &b, 1, NULL, 0) != ESP_OK) break;
        t_standby += (uint32_t)(esp_timer_get_time() - mark);

        mark = esp_timer_get_time();
        if (scan_tune(hz) != ESP_OK) break;
        t_tune += (uint32_t)(esp_timer_get_time() - mark);

        uint8_t rx_cmd[4] = { OP_SET_RX, 0xFF, 0xFF, 0xFF };
        mark = esp_timer_get_time();
        if (xfer(rx_cmd, NULL, sizeof(rx_cmd)) != ESP_OK) break;
        t_rx += (uint32_t)(esp_timer_get_time() - mark);

        mark = esp_timer_get_time();
        esp_rom_delay_us(s_scan_plan.settle_us);
        t_settle += (uint32_t)(esp_timer_get_time() - mark);

        uint8_t r[3] = { OP_GET_RSSI_INST, 0, 0 };
        mark = esp_timer_get_time();
        if (xfer(r, r, sizeof(r)) != ESP_OK) break;
        t_rssi += (uint32_t)(esp_timer_get_time() - mark);

        /* The first look of a row writes; every later one keeps the
           peak, so a bin reports the strongest thing anywhere in its slice. */
        const float v = -((float)r[2]) / 2.0f;
        if (k == 0 || v > dbm[i]) dbm[i] = v;
        got++;
    }

    if (got) {
        s_prof.standby_us = t_standby / (uint32_t)got;
        s_prof.tune_us    = t_tune    / (uint32_t)got;
        s_prof.rx_us      = t_rx      / (uint32_t)got;
        s_prof.settle_us  = t_settle  / (uint32_t)got;
        s_prof.rssi_us    = t_rssi    / (uint32_t)got;
    }

    if (got == n) {
        s_scan_look = (k + 1) % looks;
        if (row_done) *row_done = (k + 1 == looks);
    }
    return got;
}

int ls_lora_scan_sweep(float *dbm, int n)
{
    if (!s_scanning || !dbm || n <= 0) return 0;
    /* Every look of one row, starting from the first. The console
       prints a single sweep and has no frame to spread the looks across, so
       it waits for all of them; the waterfall calls ls_lora_scan_pass. */
    s_scan_look = 0;
    bool done = false;
    int got = 0;
    while (!done) {
        got = ls_lora_scan_pass(dbm, n, &done);
        if (got < n) break;
    }
    return got;
}

esp_err_t ls_lora_scan_end(void)
{
    if (!s_scanning) return ESP_OK;
    s_scanning = false;
    s_scan_plan_n = 0;   /* the next sweep plans afresh */
    if (!s_scan_saved_valid) return ESP_ERR_INVALID_STATE;

    const ls_lora_cfg_t back = s_scan_saved;
    s_scan_saved_valid = false;
    esp_err_t err = ls_lora_configure(&back);
    if (err != ESP_OK) return err;
    return ls_lora_receive();
}

esp_err_t ls_lora_rssi_inst(float *dbm)
{
    if (!s_cfg_valid || !s_rx_mode) return ESP_ERR_INVALID_STATE;
    uint8_t rx[3] = { OP_GET_RSSI_INST, 0, 0 };
    esp_err_t err = xfer(rx, rx, sizeof(rx));
    if (err == ESP_OK && dbm) *dbm = -((float)rx[2]) / 2.0f;
    return err;
}

uint32_t ls_lora_airtime_ms(int len)
{
    if (!s_cfg_valid || len < 0) return 0;
    /* Semtech AN1200.13, in microseconds throughout. This feeds a duty-cycle
       budget, and a budget built on a rounded guess is how a device ends up
       transmitting more than its licence allows. */
    const uint32_t bw = s_cfg.bw_hz;
    const uint32_t sf = s_cfg.sf;
    if (!bw) return 0;

    uint64_t ts_us = ((uint64_t)(1u << sf) * 1000000ull) / bw;
    uint32_t sym_ms = (uint32_t)(ts_us / 1000);
    int de  = sym_ms >= 16 ? 1 : 0;
    int crc = s_cfg.crc_on ? 1 : 0;

    int num = 8 * len - 4 * (int)sf + 28 + 16 * crc;   /* explicit header */
    int den = 4 * ((int)sf - 2 * de);
    int ceil_term = den > 0 ? (num + den - 1) / den : 0;
    if (ceil_term < 0) ceil_term = 0;
    int payload_symb = 8 + ceil_term * (int)s_cfg.cr;

    uint64_t preamble_us = ts_us * s_cfg.preamble + (ts_us * 425) / 100; /* +4.25 */
    uint64_t payload_us  = ts_us * (uint64_t)payload_symb;
    return (uint32_t)((preamble_us + payload_us + 999) / 1000);
}

void ls_lora_diagnostics(void)
{
    if (!s_present) {
        esp_err_t err = ls_lora_start();
        printf("lora: start %s\n", esp_err_to_name(err));
        if (err != ESP_OK) {
            printf("lora: cs=%d busy=%d rst=expander reset held? check the "
                   "3V3 rail and that ls_board_hw ran\n",
                   (int)CS_PIN, (int)BUSY_PIN);
            return;
        }
    }
    uint8_t st = 0;
    ls_lora_status(&st);
    /* 0x0740 is the LoRa sync word. Its reset value is known, so reading it
       distinguishes a working SPI path from a wire that merely is not shorted. */
    uint8_t sync[2] = { 0, 0 };
    ls_lora_read_reg(0x0740, sync, sizeof(sync));
    printf("lora: status=0x%02X mode=%u cmd=%u syncword=0x%02X%02X busy=%d\n",
           st, (unsigned)((st >> 4) & 7), (unsigned)((st >> 1) & 7),
           sync[0], sync[1], gpio_get_level(BUSY_PIN));
    printf("lora: DIO1 is expander IO17 - polled over I2C, ~10 ms floor. "
           "Receive is fine; anything with a turnaround deadline is not.\n");
}

#else  /* board declares no LoRa */

esp_err_t ls_lora_start(void) { return ESP_ERR_NOT_SUPPORTED; }
void      ls_lora_stop(void) { }
bool      ls_lora_present(void) { return false; }
esp_err_t ls_lora_status(uint8_t *out) { (void)out; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ls_lora_read_reg(uint16_t a, uint8_t *b, size_t l)
{ (void)a; (void)b; (void)l; return ESP_ERR_NOT_SUPPORTED; }
void ls_lora_diagnostics(void) { printf("lora: board declares none\n"); }
void ls_lora_cfg_default(ls_lora_cfg_t *o) { if (o) memset(o, 0, sizeof(*o)); }
esp_err_t ls_lora_configure(const ls_lora_cfg_t *c) { (void)c; return ESP_ERR_NOT_SUPPORTED; }
const ls_lora_cfg_t *ls_lora_cfg(void) { return NULL; }
esp_err_t ls_lora_receive(void) { return ESP_ERR_NOT_SUPPORTED; }
bool ls_lora_is_receiving(void) { return false; }
esp_err_t ls_lora_send(const uint8_t *d, size_t l) { (void)d; (void)l; return ESP_ERR_NOT_SUPPORTED; }
bool ls_lora_send_done(void) { return true; }
int ls_lora_poll(uint8_t *b, size_t m, float *r, float *n)
{ (void)b; (void)m; (void)r; (void)n; return 0; }
esp_err_t ls_lora_rssi_inst(float *d) { (void)d; return ESP_ERR_NOT_SUPPORTED; }
uint32_t ls_lora_airtime_ms(int len) { (void)len; return 0; }

/* The arithmetic is the same with or without a part on the board -
   it describes the SX1262 and not this assembly of one - so it stays out of
   the conditional. The sweep itself cannot exist without a radio. */
esp_err_t ls_lora_scan_begin(uint32_t a, uint32_t b)
{ (void)a; (void)b; return ESP_ERR_NOT_SUPPORTED; }
int ls_lora_scan_sweep(float *d, int n) { (void)d; (void)n; return 0; }
int ls_lora_scan_pass(float *d, int n, bool *done)
{ (void)d; (void)n; if (done) *done = false; return 0; }
esp_err_t ls_lora_scan_end(void) { return ESP_ERR_NOT_SUPPORTED; }
bool ls_lora_scanning(void) { return false; }
void ls_lora_scan_profile(ls_lora_scan_prof_t *o)
{ if (o) { const ls_lora_scan_prof_t z = { 0, 0, 0, 0, 0 }; *o = z; } }

uint32_t ls_lora_freq_steps(uint32_t hz)
{
    return (uint32_t)(((uint64_t)hz << 25) / 32000000ull);
}

uint32_t ls_lora_scan_bin_hz(uint32_t min_hz, uint32_t max_hz, int n, int i)
{
    if (n <= 0) return min_hz;
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    if (n == 1 || max_hz <= min_hz) return min_hz;
    const uint64_t span = (uint64_t)(max_hz - min_hz);
    return min_hz + (uint32_t)((span * (uint64_t)i) / (uint64_t)(n - 1));
}

#endif
