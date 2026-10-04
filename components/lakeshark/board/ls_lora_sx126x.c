/* The SX126x backend of ls_lora.c: the code that used to be ls_lora.c, moved
   whole. See ls_lora.h for why BUSY and DIO1 shape it, and ls_lora_priv.h for
   what the dispatcher in ls_lora.c owns (SPI device, BUSY, packet buffer,
   reset, chip detection). */
#include "ls_lora.h"
#include "ls_lora_priv.h"

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

/* The ladder is shared with the sweep plan in ls_lora.c, which is the half of
   this that every board has. */
#define BW_LADDER   ls_lora_sx126x_bw_ladder
#define BW_LADDER_N ls_lora_sx126x_bw_ladder_n

/* How long one ls_lora_scan_pass may run before it hands back. */
#define SCAN_PASS_BUDGET_US 20000

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

/* Forward declarations: the ops table at the bottom is the only user. */
static esp_err_t sx_scan_end(void);
static uint32_t  sx_airtime_ms(int len);

/* One command: opcode, then any parameters, then any reply. The SX1262 is
   full duplex and returns a status byte during the opcode, but nothing here
   needs it, so the transaction is kept simple deliberately. */
static esp_err_t cmd(uint8_t opcode, const uint8_t *tx, size_t tx_len,
                     uint8_t *rx, size_t rx_len)
{
    if (!ls_lora_hw_dev()) return ESP_ERR_INVALID_STATE;
    uint8_t buf[16];
    size_t n = 1 + tx_len + rx_len;
    if (n > sizeof(buf)) return ESP_ERR_INVALID_SIZE;

    /* The BUSY wait and the transaction are one unit: another task's command
       between them would start the part working again under this one. */
    ls_lora_hw_lock();
    if (!ls_lora_hw_wait_not_busy(20)) {
        ls_lora_hw_unlock();
        ESP_LOGW(TAG, "busy stuck high before 0x%02X", opcode);
        return ESP_ERR_TIMEOUT;
    }

    memset(buf, 0, n);
    buf[0] = opcode;
    if (tx && tx_len) memcpy(&buf[1], tx, tx_len);

    spi_transaction_t t = {
        .length = n * 8,
        .tx_buffer = buf,
        .rx_buffer = buf,
    };
    esp_err_t err = ls_lora_hw_transmit(&t);
    ls_lora_hw_unlock();
    if (err == ESP_OK && rx && rx_len) memcpy(rx, &buf[1 + tx_len], rx_len);
    return err;
}

static esp_err_t sx_read_reg(uint16_t addr, uint8_t *buf, size_t len)
{
    /* ReadRegister is address high, address low, then one status byte the
       part inserts before the data. That NOP is easy to forget and shifts
       every byte by one, which reads as plausible-but-wrong data. */
    uint8_t tx[3] = { (uint8_t)(addr >> 8), (uint8_t)addr, 0x00 };
    return cmd(OP_READ_REGISTER, tx, sizeof(tx), buf, len);
}

static esp_err_t sx_status(uint8_t *out)
{
    uint8_t st = 0;
    esp_err_t err = cmd(OP_GET_STATUS, NULL, 0, &st, 1);
    if (err == ESP_OK && out) *out = st;
    return err;
}

/* Liveness for the dispatcher: standby, then GetStatus. Whether the byte
   means anything is the dispatcher's call, since it owns the logging. */
esp_err_t ls_lora_sx126x_probe(uint8_t *status)
{
    uint8_t standby = STANDBY_RC;
    cmd(OP_SET_STANDBY, &standby, 1, NULL, 0);
    return sx_status(status);
}

static ls_lora_cfg_t s_cfg;
static bool     s_cfg_valid;
static bool     s_fsk_active;
static ls_lora_cfg_t s_fsk_saved;
static bool     s_fsk_saved_valid, s_fsk_saved_rx;
static uint8_t  s_fsk_bytes;
static uint16_t s_fsk_preamble_bits;
static uint8_t  s_fsk_sync_bits;
static bool     s_scanning;        /* a sweep owns the synthesiser */
static bool     s_rx_mode;
/* A receive that could not be armed. On the P4 every SPI transfer here takes
   bounce buffers from internal DMA memory, which ADS-B streaming leaves
   short; a failed one after RX_DONE left the part in standby, so nothing
   more was ever heard. sx_poll tries again until it takes. */
static bool     s_rearm;
static bool     s_tx_busy;
static int64_t  s_tx_deadline;

static esp_err_t xfer(const uint8_t *tx, uint8_t *rx, size_t n)
{
    if (!ls_lora_hw_dev() || !ls_lora_hw_pkt()) return ESP_ERR_INVALID_STATE;
    if (n > 259) return ESP_ERR_INVALID_SIZE;
    /* The packet buffer is shared, so it is held from the fill to the copy out. */
    ls_lora_hw_lock();
    if (!ls_lora_hw_wait_not_busy(20)) {
        ls_lora_hw_unlock();
        return ESP_ERR_TIMEOUT;
    }
    memcpy(ls_lora_hw_pkt(), tx, n);
    spi_transaction_t t = { .length = n * 8, .tx_buffer = ls_lora_hw_pkt(), .rx_buffer = ls_lora_hw_pkt() };
    esp_err_t err = ls_lora_hw_transmit(&t);
    if (err == ESP_OK && rx) memcpy(rx, ls_lora_hw_pkt(), n);
    ls_lora_hw_unlock();
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

static const ls_lora_cfg_t *sx_cfg(void) { return s_cfg_valid ? &s_cfg : NULL; }

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

static esp_err_t configure_lora(const ls_lora_cfg_t *cfg)
{
    if (!ls_lora_present()) {
        esp_err_t e = ls_lora_start();
        if (e != ESP_OK) return e;
        /* The part that answered may not be ours: never send SX126x
           opcodes to anything else. */
        if (ls_lora_chip() != LS_LORA_CHIP_SX126X) return ESP_ERR_NOT_SUPPORTED;
    }
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
    if (!ls_lora_hw_wait_not_busy(100)) return ESP_ERR_TIMEOUT;

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
        sx_read_reg(REG_TX_CLAMP, &v, 1);
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

static esp_err_t sx_configure(const ls_lora_cfg_t *cfg)
{
    if (s_fsk_active) return ESP_ERR_INVALID_STATE;
    return configure_lora(cfg);
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

static esp_err_t sx_receive(void)
{
    if (!s_cfg_valid || s_fsk_active) return ESP_ERR_INVALID_STATE;
    esp_err_t err = clear_irq(0xFFFF);
    if (err == ESP_OK) {
        uint8_t tx[4] = { OP_SET_RX, 0xFF, 0xFF, 0xFF };
        err = xfer(tx, NULL, sizeof(tx));
    }
    if (err == ESP_OK) { s_rx_mode = true; s_tx_busy = false; }
    s_rearm = err != ESP_OK;
    return err;
}

static bool sx_is_receiving(void) { return s_rx_mode; }

static esp_err_t sx_send(const uint8_t *data, size_t len)
{
    if (!s_cfg_valid || s_fsk_active) return ESP_ERR_INVALID_STATE;
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    if (len > 255) return ESP_ERR_INVALID_SIZE;
    if (s_tx_busy)  return ESP_ERR_INVALID_STATE;

    if (s_scanning) return ESP_ERR_INVALID_STATE;
    if (!ls_lora_hw_pkt())     return ESP_ERR_INVALID_STATE;

    esp_err_t err;
    uint8_t b = STANDBY_RC;
    if ((err = cmd(OP_SET_STANDBY, &b, 1, NULL, 0)) != ESP_OK) return err;
    s_rearm = true;             /* out of receive until the transmit starts */
    if ((err = clear_irq(0xFFFF)) != ESP_OK) return err;
    if ((err = set_packet_params((uint8_t)len)) != ESP_OK) return err;

    /* WriteBuffer: opcode, offset, then the payload. */
    if (!ls_lora_hw_wait_not_busy(20)) return ESP_ERR_TIMEOUT;
    ls_lora_hw_pkt()[0] = OP_WRITE_BUFFER;
    ls_lora_hw_pkt()[1] = 0x00;
    memcpy(&ls_lora_hw_pkt()[2], data, len);
    spi_transaction_t t = { .length = (2 + len) * 8, .tx_buffer = ls_lora_hw_pkt(), .rx_buffer = NULL };
    if ((err = ls_lora_hw_transmit(&t)) != ESP_OK) return err;

    /* A SetTx timeout of 0 means the chip imposes none; the transmit ends
       when it ends. The deadline below is ours, not the chip's. */
    uint8_t tx[4] = { OP_SET_TX, 0x00, 0x00, 0x00 };
    if ((err = xfer(tx, NULL, sizeof(tx))) != ESP_OK) return err;

    s_tx_busy = true;
    s_rx_mode = false;
    s_rearm = false;
    s_tx_deadline = esp_timer_get_time() + (int64_t)TX_TIMEOUT_MS * 1000;
    ESP_LOGI(TAG, "TX begin: %u bytes at %d dBm, estimated %lu ms",
             (unsigned)len, s_cfg.power_dbm,
             (unsigned long)sx_airtime_ms((int)len));
    return ESP_OK;
}

static bool sx_send_done(void)
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

static int sx_poll(uint8_t *buf, size_t max, float *rssi_dbm, float *snr_db)
{
    if (!s_cfg_valid || s_fsk_active || !buf || !max || !ls_lora_hw_pkt()) return 0;
    if (s_rearm && !s_tx_busy && sx_receive() != ESP_OK) return 0;

    /* DIO1 first: one expander read is much cheaper than an SPI round trip
       and this runs in a loop. Low means there is nothing to collect. */
bool dio1 = false;
    if (ls_xl9535_get(LS_BOARD_XL_RADIO_DIO1, &dio1) != ESP_OK || !dio1) return 0;

    uint16_t irq = 0;
    if (get_irq(&irq) != ESP_OK) return 0;
    if (!irq) return 0;
    clear_irq(0xFFFF);

    if (irq & (IRQ_CRC_ERR | IRQ_HEADER_ERR)) { sx_receive(); return -1; }
    if (irq & IRQ_TX_DONE) s_tx_busy = false;
    if (!(irq & IRQ_RX_DONE)) return 0;

    uint8_t st[4] = { OP_GET_RX_BUF_STAT, 0, 0, 0 };
    if (xfer(st, st, sizeof(st)) != ESP_OK) { sx_receive(); return 0; }
    uint8_t len = st[2], off = st[3];
    if (len == 0) { sx_receive(); return 0; }
    if (len > max) len = (uint8_t)max;

    /* ReadBuffer is opcode, offset, one NOP the part uses to turn the bus
       around, then the data. Forgetting that NOP shifts every byte by one -
       the same trap sx_read_reg already documents. */
    if (!ls_lora_hw_wait_not_busy(20)) { sx_receive(); return 0; }
    memset(ls_lora_hw_pkt(), 0, (size_t)3 + len);
    ls_lora_hw_pkt()[0] = OP_READ_BUFFER;
    ls_lora_hw_pkt()[1] = off;
    spi_transaction_t t = { .length = (3 + len) * 8, .tx_buffer = ls_lora_hw_pkt(), .rx_buffer = ls_lora_hw_pkt() };
    if (ls_lora_hw_transmit(&t) != ESP_OK) { sx_receive(); return 0; }
    memcpy(buf, &ls_lora_hw_pkt()[3], len);

    uint8_t ps[4] = { OP_GET_PKT_STATUS, 0, 0, 0 };
    if (xfer(ps, ps, sizeof(ps)) == ESP_OK) {
        if (rssi_dbm) *rssi_dbm = -((float)ps[2]) / 2.0f;
        if (snr_db)   *snr_db   = ((float)(int8_t)ps[3]) / 4.0f;
    }

    sx_receive();
    return len;
}

static uint8_t fsk_bw_code(uint32_t hz)
{
    static const struct { uint32_t hz; uint8_t code; } bw[] = {
        {4800,0x1f},{5800,0x17},{7300,0x0f},{9700,0x1e},
        {11700,0x16},{14600,0x0e},{19500,0x1d},{23400,0x15},
        {29300,0x0d},{39000,0x1c},{46900,0x14},{58600,0x0c},
        {78200,0x1b},{93800,0x13},{117300,0x0b},{156200,0x1a},
        {187200,0x12},{234300,0x0a},{312000,0x19},{373600,0x11},
        {467000,0x09}
    };
    for (size_t i = 0; i < sizeof(bw) / sizeof(bw[0]); i++)
        if (bw[i].hz == hz) return bw[i].code;
    return 0;
}

/* Fixed length, 32-bit sync, no preamble gate, no CRC and no whitening.

   The CRC is off on purpose and not for lack of hardware. A protocol whose
   CRC uses an initial value the part cannot be told to use has to compute
   its own and carry it inside the payload, and a listener that switched the
   hardware CRC back on would then reject every frame as corrupt. Off is the
   setting that works for both those protocols and for the ones with no CRC
   at all; a caller that wants the radio's own CRC should say so here.

   Transmit and receive both come through here because they must agree on
   every one of these: a frame sent with one preamble length and listened for
   with another is a fault that only shows up on air. */
static esp_err_t fsk_packet_params(uint8_t payload_bytes)
{
    const uint16_t pre = s_fsk_preamble_bits;
    uint8_t packet[] = {OP_SET_PKT_PARAMS, (uint8_t)(pre >> 8), (uint8_t)pre,
                        0, s_fsk_sync_bits, 0, 0, payload_bytes, 1, 0};
    return xfer(packet, NULL, sizeof(packet));
}

static bool sx_fsk_active(void) { return s_fsk_active; }

static esp_err_t sx_fsk_end(void)
{
    if (!s_fsk_active) return ESP_OK;
    uint8_t standby = STANDBY_RC;
    esp_err_t err = cmd(OP_SET_STANDBY, &standby, 1, NULL, 0);
    s_rx_mode = false;
    if (s_fsk_saved_valid) {
        esp_err_t restored = configure_lora(&s_fsk_saved);
        if (restored == ESP_OK && s_fsk_saved_rx) {
            uint8_t rx[] = {OP_SET_RX, 0xff, 0xff, 0xff};
            restored = clear_irq(0xffff);
            if (restored == ESP_OK) restored = xfer(rx, NULL, sizeof(rx));
            s_rx_mode = restored == ESP_OK;
        }
        if (restored != ESP_OK) err = restored;
    } else {
        s_cfg_valid = false;
    }
    s_fsk_active = false;
    return err;
}

static esp_err_t sx_fsk_begin(const ls_fsk_cfg_t *cfg)
{
    if (cfg && (cfg->rssi_at_sync || cfg->preamble_detect_bits || cfg->stream)) return ESP_ERR_NOT_SUPPORTED;
    if (!cfg || cfg->freq_hz < 150000000u || cfg->freq_hz > 960000000u ||
        cfg->bitrate < 600 || cfg->bitrate > 300000 ||
        cfg->deviation_hz < 600 || cfg->deviation_hz > 200000 ||
        !cfg->payload_bytes || !fsk_bw_code(cfg->bandwidth_hz) ||
        cfg->bitrate + 2 * cfg->deviation_hz > cfg->bandwidth_hz ||
        (cfg->preamble_bits && cfg->preamble_bits < 8) ||
        /* 32, not the part's 64: sync_word is a uint32_t and sx_fsk_begin
           writes four registers from it. Asking the radio to match 40 bits
           would have it match four written bytes and one register nobody set,
           which is a session that hears nothing and says nothing about why. */
        (cfg->sync_bits && (cfg->sync_bits > 32 || cfg->sync_bits % 8)) ||
        cfg->power_dbm < -9 || cfg->power_dbm > 22)
        return ESP_ERR_INVALID_ARG;
    if (s_fsk_active || s_scanning || s_tx_busy) return ESP_ERR_INVALID_STATE;

    s_fsk_saved_valid = s_cfg_valid;
    s_fsk_saved_rx = s_rx_mode;
    s_fsk_saved = s_cfg;
    ls_lora_cfg_t setup;
    ls_lora_cfg_default(&setup);
    setup.freq_hz = cfg->freq_hz;
    setup.cal_min_mhz = (uint16_t)((cfg->freq_hz / 4000000u) * 4);
    setup.cal_max_mhz = setup.cal_min_mhz + 4;
    setup.power_dbm = cfg->power_dbm;
    s_fsk_preamble_bits = cfg->preamble_bits ? cfg->preamble_bits : 32;
    s_fsk_sync_bits = cfg->sync_bits ? cfg->sync_bits : 32;
    s_fsk_active = true;
    esp_err_t err = configure_lora(&setup);
    if (err != ESP_OK) goto fail;

    uint8_t type = 0;
    if ((err = cmd(OP_SET_PKT_TYPE, &type, 1, NULL, 0)) != ESP_OK) goto fail;
    const uint32_t br = (1024000000u + cfg->bitrate / 2) / cfg->bitrate;
    const uint32_t dev = (uint32_t)(((uint64_t)cfg->deviation_hz << 25) / 32000000u);
    uint8_t mod[] = {OP_SET_MOD_PARAMS, (uint8_t)(br >> 16), (uint8_t)(br >> 8),
        (uint8_t)br, 0, fsk_bw_code(cfg->bandwidth_hz),
        (uint8_t)(dev >> 16), (uint8_t)(dev >> 8), (uint8_t)dev};
    if ((err = xfer(mod, NULL, sizeof(mod))) != ESP_OK) goto fail;
    if ((err = fsk_packet_params(cfg->payload_bytes)) != ESP_OK) goto fail;
    for (int i = 0; i < 4; i++) {
        err = write_reg((uint16_t)(0x06c0 + i),
                        (uint8_t)(cfg->sync_word >> (24 - i * 8)));
        if (err != ESP_OK) goto fail;
    }
    if ((err = clear_irq(0xffff)) != ESP_OK) goto fail;
    uint8_t rx[] = {OP_SET_RX, 0xff, 0xff, 0xff};
    if ((err = xfer(rx, NULL, sizeof(rx))) != ESP_OK) goto fail;
    s_fsk_bytes = cfg->payload_bytes;
    s_rx_mode = true;
    return ESP_OK;
fail:
    {
        esp_err_t restored = sx_fsk_end();
        return restored == ESP_OK ? err : restored;
    }
}

static int sx_fsk_poll(uint8_t *buf, size_t size, float *rssi_dbm)
{
    if (!s_fsk_active || !buf || size < s_fsk_bytes) return -1;
    uint16_t irq;
    if (get_irq(&irq) != ESP_OK) return -1;
    if (!(irq & (IRQ_RX_DONE | IRQ_CRC_ERR | IRQ_TIMEOUT))) return 0;
    if (clear_irq(irq) != ESP_OK) return -1;
    if (irq & (IRQ_CRC_ERR | IRQ_TIMEOUT)) return -1;
    uint8_t st[] = {OP_GET_RX_BUF_STAT, 0, 0, 0};
    if (xfer(st, st, sizeof(st)) != ESP_OK || st[2] != s_fsk_bytes) return -1;
    uint8_t rx[258] = {OP_READ_BUFFER, st[3], 0};
    if (xfer(rx, rx, (size_t)s_fsk_bytes + 3) != ESP_OK) return -1;
    memcpy(buf, rx + 3, s_fsk_bytes);
    uint8_t status[] = {OP_GET_PKT_STATUS, 0, 0, 0, 0};
    if (rssi_dbm) *rssi_dbm = -200.0f;
    if (xfer(status, status, sizeof(status)) == ESP_OK && rssi_dbm)
        *rssi_dbm = -(float)status[4] / 2.0f;
    /* Continuous RX searches for the next sync without restarting. */
    return s_fsk_bytes;
}

static esp_err_t sx_fsk_send(const uint8_t *data, size_t len)
{
    if (!s_fsk_active || !ls_lora_hw_pkt()) return ESP_ERR_INVALID_STATE;
    if (!data || len == 0)       return ESP_ERR_INVALID_ARG;
    if (len > 255)               return ESP_ERR_INVALID_SIZE;
    if (s_tx_busy || s_scanning) return ESP_ERR_INVALID_STATE;

    esp_err_t err;
    uint8_t b = STANDBY_RC;
    if ((err = cmd(OP_SET_STANDBY, &b, 1, NULL, 0)) != ESP_OK) return err;
    /* Cleared before the buffer write, not after the transmit: a receive
       that was running owns this flag and a failure below must not leave it
       claiming the part is still listening. */
    s_rx_mode = false;
    if ((err = clear_irq(0xFFFF)) != ESP_OK) return err;
    /* The length the payload actually is, which is not the session's receive
       length - sx_fsk_receive puts that back. */
    if ((err = fsk_packet_params((uint8_t)len)) != ESP_OK) return err;

    if (!ls_lora_hw_wait_not_busy(20)) return ESP_ERR_TIMEOUT;
    ls_lora_hw_pkt()[0] = OP_WRITE_BUFFER;
    ls_lora_hw_pkt()[1] = 0x00;
    memcpy(&ls_lora_hw_pkt()[2], data, len);
    spi_transaction_t t = { .length = (2 + len) * 8, .tx_buffer = ls_lora_hw_pkt(),
                            .rx_buffer = NULL };
    if ((err = ls_lora_hw_transmit(&t)) != ESP_OK) return err;

    uint8_t tx[4] = { OP_SET_TX, 0x00, 0x00, 0x00 };
    if ((err = xfer(tx, NULL, sizeof(tx))) != ESP_OK) return err;

    s_tx_busy = true;
    s_tx_deadline = esp_timer_get_time() + (int64_t)TX_TIMEOUT_MS * 1000;
    ESP_LOGI(TAG, "FSK TX: %u byte payload behind a %u-bit preamble and a "
             "32-bit sync word, at %d dBm", (unsigned)len,
             (unsigned)s_fsk_preamble_bits, (int)s_cfg.power_dbm);
    return ESP_OK;
}

static esp_err_t sx_fsk_receive(void)
{
    if (!s_fsk_active) return ESP_ERR_INVALID_STATE;
    if (s_tx_busy)     return ESP_ERR_INVALID_STATE;
    esp_err_t err;
    if ((err = fsk_packet_params(s_fsk_bytes)) != ESP_OK) return err;
    if ((err = clear_irq(0xffff)) != ESP_OK) return err;
    uint8_t rx[] = {OP_SET_RX, 0xff, 0xff, 0xff};
    if ((err = xfer(rx, NULL, sizeof(rx))) != ESP_OK) return err;
    s_rx_mode = true;
    return ESP_OK;
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
static int           s_scan_look, s_scan_bin;

static bool sx_scanning(void) { return s_scanning; }

/* Configure the part for the band in s_scan_min_hz..s_scan_max_hz,
   planned for `n` bins. Built on the configuration the sweep saved rather
   than on s_cfg, because once a sweep is running s_cfg is the previous
   band's scan configuration and not the mesh's. */
static esp_err_t scan_configure(int n)
{
    ls_lora_scan_plan(s_scan_min_hz, s_scan_max_hz, n, &s_scan_plan);
    s_scan_plan_n = n;
    s_scan_look = 0; s_scan_bin=0;

    ls_lora_cfg_t sc = s_scan_saved;
    sc.freq_hz     = s_scan_min_hz;
    sc.bw_hz       = s_scan_plan.bw_hz;
    sc.cal_min_mhz = (uint16_t)(s_scan_min_hz / 1000000u);
    sc.cal_max_mhz = (uint16_t)((s_scan_max_hz + 999999u) / 1000000u);

    sc.cal_min_mhz = (uint16_t)((sc.cal_min_mhz / 4) * 4);
    sc.cal_max_mhz = (uint16_t)(((sc.cal_max_mhz + 3) / 4) * 4);

    esp_err_t err = sx_configure(&sc);
    if (err != ESP_OK) return err;
    return sx_receive();
}

static esp_err_t sx_scan_begin(uint32_t min_hz, uint32_t max_hz)
{
    /* The same band again is nothing to do: the waterfall asks once
       a frame. A different band while sweeping is retuned below, in place. */
    if (s_scanning && min_hz == s_scan_min_hz && max_hz == s_scan_max_hz)
        return ESP_OK;
    if (!s_cfg_valid || s_fsk_active) return ESP_ERR_INVALID_STATE;
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
        if (retune) (void)sx_scan_end();
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

static void sx_scan_profile(ls_lora_scan_prof_t *out)
{
    if (out) *out = s_prof;
}

/* Semtech status: chip mode 5 is RX; command statuses 3..5 are errors.
   An absent SPI peer (FF/00) must never become a quiet RSSI measurement. */
static bool rssi_status_valid(uint8_t status)
{
    unsigned mode=(status>>4)&7, command=(status>>1)&7;
    /* 0/1 occur before a packet has completed (observed idle RX 0x52). */
    return mode==5 && command!=7 && !(command>=3 && command<=5);
}

static int sx_scan_pass(float *dbm, int n, bool *row_done)
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

    int got = 0; bool failed=false;
    const int64_t pass_start=esp_timer_get_time();
    for (int i = s_scan_bin; i < n; i++) {
        const uint32_t hz = ls_lora_scan_look_hz(s_scan_min_hz, s_scan_max_hz,
                                                 n, i, looks, k);

        uint8_t b = STANDBY_XOSC;
        mark = esp_timer_get_time();
        if (cmd(OP_SET_STANDBY, &b, 1, NULL, 0) != ESP_OK) {failed=true;break;}
        t_standby += (uint32_t)(esp_timer_get_time() - mark);

        mark = esp_timer_get_time();
        if (scan_tune(hz) != ESP_OK) {failed=true;break;}
        t_tune += (uint32_t)(esp_timer_get_time() - mark);

        uint8_t rx_cmd[4] = { OP_SET_RX, 0xFF, 0xFF, 0xFF };
        mark = esp_timer_get_time();
        if (xfer(rx_cmd, NULL, sizeof(rx_cmd)) != ESP_OK) {failed=true;break;}
        t_rx += (uint32_t)(esp_timer_get_time() - mark);

        mark = esp_timer_get_time();
        esp_rom_delay_us(s_scan_plan.settle_us);
        t_settle += (uint32_t)(esp_timer_get_time() - mark);

        uint8_t r[3] = { OP_GET_RSSI_INST, 0, 0 };
        mark = esp_timer_get_time();
        if (xfer(r, r, sizeof(r)) != ESP_OK || !rssi_status_valid(r[1])) {failed=true;break;}
        t_rssi += (uint32_t)(esp_timer_get_time() - mark);

        /* The first look of a row writes; every later one keeps the
           peak, so a bin reports the strongest thing anywhere in its slice. */
        const float v = -((float)r[2]) / 2.0f;
        if (k == 0 || v > dbm[i]) dbm[i] = v;
        got++;s_scan_bin=i+1;
        if(esp_timer_get_time()-pass_start>=SCAN_PASS_BUDGET_US)break;
    }

    if (got) {
        s_prof.standby_us = t_standby / (uint32_t)got;
        s_prof.tune_us    = t_tune    / (uint32_t)got;
        s_prof.rx_us      = t_rx      / (uint32_t)got;
        s_prof.settle_us  = t_settle  / (uint32_t)got;
        s_prof.rssi_us    = t_rssi    / (uint32_t)got;
    }

    if(failed){s_scan_bin=0;s_scan_look=0;return 0;}
    if (s_scan_bin == n) {
        s_scan_bin=0;
        s_scan_look = (k + 1) % looks;
        if (row_done) *row_done = (k + 1 == looks);
    }
    return n;
}

static int sx_scan_sweep(float *dbm, int n)
{
    if (!s_scanning || !dbm || n <= 0) return 0;
    /* Every look of one row, starting from the first. The console
       prints a single sweep and has no frame to spread the looks across, so
       it waits for all of them; the waterfall calls sx_scan_pass. */
    /* The lock is taken a pass at a time, not for the sweep: a pass is bounded
       by SCAN_PASS_BUDGET_US and the sweep is as many of them as there are
       looks. Between passes another task may use the part; the next pass
       carries on from s_scan_bin, as the waterfall's passes already do. */
    ls_lora_hw_lock();
    s_scan_look = 0; s_scan_bin=0;
    ls_lora_hw_unlock();
    bool done = false;
    int got = 0;
    while (!done) {
        ls_lora_hw_lock();
        got = sx_scan_pass(dbm, n, &done);
        ls_lora_hw_unlock();
        if (got < n) break;
    }
    return got;
}

static esp_err_t sx_scan_end(void)
{
    if (!s_scanning) return ESP_OK;
    s_scanning = false;
    s_scan_plan_n = 0;   /* the next sweep plans afresh */
    if (!s_scan_saved_valid) return ESP_ERR_INVALID_STATE;

    const ls_lora_cfg_t back = s_scan_saved;
    s_scan_saved_valid = false;
    esp_err_t err = sx_configure(&back);
    if (err != ESP_OK) return err;
    return sx_receive();
}

static esp_err_t sx_rssi_inst(float *dbm)
{
    if (!s_cfg_valid || !s_rx_mode) return ESP_ERR_INVALID_STATE;
    uint8_t rx[3] = { OP_GET_RSSI_INST, 0, 0 };
    esp_err_t err = xfer(rx, rx, sizeof(rx));
    if (err == ESP_OK && !rssi_status_valid(rx[1])) err=ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK && dbm) *dbm = -((float)rx[2]) / 2.0f;
    return err;
}

static uint32_t sx_airtime_ms(int len)
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

static void sx_diagnostics(void)
{
    uint8_t st = 0;
    const esp_err_t st_err = sx_status(&st);
    if (st_err != ESP_OK)
        printf("lora: status read failed: %s (zeros below are not the part's)\n",
               esp_err_to_name(st_err));
    if (s_rearm) printf("lora: receive not armed yet - retried on every poll\n");
    /* 0x0740 is the LoRa sync word. Its reset value is known, so reading it
       distinguishes a working SPI path from a wire that merely is not shorted. */
    uint8_t sync[2] = { 0, 0 };
    sx_read_reg(0x0740, sync, sizeof(sync));
    printf("lora: status=0x%02X mode=%u cmd=%u syncword=0x%02X%02X busy=%d\n",
           st, (unsigned)((st >> 4) & 7), (unsigned)((st >> 1) & 7),
           sync[0], sync[1], gpio_get_level(BUSY_PIN));
    printf("lora: DIO1 is expander IO17 - polled over I2C, ~10 ms floor. "
           "Receive is fine; anything with a turnaround deadline is not.\n");
}

static uint32_t sx_caps(void)
{
    return LS_LORA_CAP_LORA | LS_LORA_CAP_FSK | LS_LORA_CAP_RSSI_INST;
}

const ls_lora_ops_t ls_lora_sx126x_ops = {
    .kind = LS_LORA_CHIP_SX126X,
    .name = "SX126x",
    .caps = sx_caps,
    .status = sx_status,
    .read_reg = sx_read_reg,
    .configure = sx_configure,
    .cfg = sx_cfg,
    .receive = sx_receive,
    .is_receiving = sx_is_receiving,
    .send = sx_send,
    .send_done = sx_send_done,
    .poll = sx_poll,
    .rssi_inst = sx_rssi_inst,
    .airtime_ms = sx_airtime_ms,
    .fsk_begin = sx_fsk_begin,
    .fsk_poll = sx_fsk_poll,
    .fsk_send = sx_fsk_send,
    .fsk_receive = sx_fsk_receive,
    .fsk_end = sx_fsk_end,
    .fsk_active = sx_fsk_active,
    .scan_begin = sx_scan_begin,
    .scan_sweep = sx_scan_sweep,
    .scan_pass = sx_scan_pass,
    .scan_end = sx_scan_end,
    .scanning = sx_scanning,
    .scan_profile = sx_scan_profile,
    .diagnostics = sx_diagnostics,
    .stop = NULL,
};

#else  /* board declares no LoRa: nothing to bind */

typedef int ls_lora_sx126x_not_fitted_t;

#endif
